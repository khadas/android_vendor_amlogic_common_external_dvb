/***************************************************************************
 *  Copyright C 2009 by Amlogic, Inc. All Rights Reserved.
 */
/**\file am_scan.c
 * \brief SCAN模块
 *
 * \author Xia Lei Peng <leipeng.xia@amlogic.com>
 * \date 2010-10-27: create the document
 ***************************************************************************/

#define AM_DEBUG_LEVEL 2

#include <errno.h>
#include <assert.h>
#include <fcntl.h>
#include <am_iconv.h>
#include <am_debug.h>
#include <am_scan.h>
#include "am_scan_internal.h"
#include <am_dmx.h>
#include <am_time.h>
#include <am_aout.h>
#include <am_av.h>
#include <linux/ioctl.h>
#include <linux/types.h>
#include <linux/tvin/tvin.h>

/****************************************************************************
 * Macro definitions
 ***************************************************************************/

#define SCAN_FEND_DEV_NO 0
#define SCAN_DMX_DEV_NO 0

/*位操作*/
#define BIT_MASK(b) (1 << ((b) % 8))
#define BIT_SLOT(b) ((b) / 8)
#define BIT_SET(a, b) ((a)[BIT_SLOT(b)] |= BIT_MASK(b))
#define BIT_CLEAR(a, b) ((a)[BIT_SLOT(b)] &= ~BIT_MASK(b))
#define BIT_TEST(a, b) ((a)[BIT_SLOT(b)] & BIT_MASK(b))

/*超时ms定义*/
#define PAT_TIMEOUT 6000
#define PMT_TIMEOUT 3000
#define SDT_TIMEOUT 6000
#define NIT_TIMEOUT 10000
#define CAT_TIMEOUT 3000
#define BAT_TIMEOUT 10000
#define MGT_TIMEOUT 2500
#define STT_TIMEOUT 0
#define VCT_TIMEOUT 2500
#define RRT_TIMEOUT 70000

/*子表最大个数*/
#define MAX_BAT_SUBTABLE_CNT 32

/*多子表时接收重复最小间隔*/
#define BAT_REPEAT_DISTANCE 3000

/*DVBC默认前端参数*/
#define DEFAULT_DVBC_MODULATION QAM_64
#define DEFAULT_DVBC_SYMBOLRATE 6875000

/*DVBT默认前端参数*/
#define DEFAULT_DVBT_BW BANDWIDTH_8_MHZ
#define DEFAULT_DVBT_FREQ_START    474000
#define DEFAULT_DVBT_FREQ_STOP     858000

/*DVBS盲扫起始结束频率*/
#define DEFAULT_DVBS_BS_FREQ_START	950000000 /*950MHz*/
#define DEFAULT_DVBS_BS_FREQ_STOP	2150000000U /*2150MHz*/

/*电视广播排序是否统一编号*/
#define SORT_TOGETHER
	
/*清除一个subtable控制数据*/
#define SUBCTL_CLEAR(sc)\
	AM_MACRO_BEGIN\
		memset((sc)->mask, 0, sizeof((sc)->mask));\
		(sc)->ver = 0xff;\
	AM_MACRO_END

/*添加数据列表中*/
#define ADD_TO_LIST(_t, _l)\
	AM_MACRO_BEGIN\
		(_t)->p_next = (_l);\
		_l = _t;\
	AM_MACRO_END

/*添加数据到列表尾部*/
#define APPEND_TO_LIST(_type,_t,_l)\
	AM_MACRO_BEGIN\
		_type *tmp = _l;\
		_type *last = NULL;\
		while (tmp != NULL) {\
			last = tmp;\
			tmp = tmp->p_next;\
		}\
		if (last == NULL)\
			_l = _t;\
		else\
			last->p_next = _t;\
	AM_MACRO_END
	
/*释放一个表的所有SI数据*/
#define RELEASE_TABLE_FROM_LIST(_t, _l)\
	AM_MACRO_BEGIN\
		_t *tmp, *next;\
		tmp = (_l);\
		while (tmp){\
			next = tmp->p_next;\
			AM_SI_ReleaseSection(scanner->dtvctl.hsi, tmp->i_table_id, (void*)tmp);\
			tmp = next;\
		}\
		(_l) = NULL;\
	AM_MACRO_END

/*解析section并添加到列表*/
#define COLLECT_SECTION(type, list)\
	AM_MACRO_BEGIN\
		type *p_table;\
		if (AM_SI_DecodeSection(scanner->dtvctl.hsi, sec_ctrl->pid, (uint8_t*)data, len, (void**)&p_table) == AM_SUCCESS)\
		{\
			p_table->p_next = NULL;\
			ADD_TO_LIST(p_table, list); /*添加到搜索结果列表中*/\
			am_scan_tablectl_mark_section(sec_ctrl, &header); /*设置为已接收*/\
		}else\
		{\
			AM_DEBUG(1, "Decode Section failed");\
		}\
	AM_MACRO_END

/*通知一个事件*/
#define SIGNAL_EVENT(e, d)\
	AM_MACRO_BEGIN\
		pthread_mutex_unlock(&scanner->lock);\
		AM_EVT_Signal((int)scanner, e, d);\
		pthread_mutex_lock(&scanner->lock);\
	AM_MACRO_END

/*触发一个进度事件*/
#define SET_PROGRESS_EVT(t, d)\
	AM_MACRO_BEGIN\
		AM_SCAN_Progress_t prog;\
		prog.evt = t;\
		prog.data = (void*)d;\
		SIGNAL_EVENT(AM_SCAN_EVT_PROGRESS, (void*)&prog);\
	AM_MACRO_END

#define GET_MODE(_m) ((_m) & 0x07)

#define dvb_fend_para(_p)	((struct dvb_frontend_parameters*)(&_p))
#define cur_fe_para			scanner->start_freqs[scanner->curr_freq].dtv_para
#define dtv_start_para		scanner->start_para.dtv_para
#define atv_start_para		scanner->start_para.atv_para


/****************************************************************************
 * static data
 ***************************************************************************/

/*DVB-C标准频率表*/
static int dvbc_std_freqs[] = 
{
52500,  60250,  68500,  80000,
88000,  115000, 123000, 131000,
139000, 147000, 155000, 163000,
171000, 179000, 187000, 195000,
203000, 211000, 219000, 227000,
235000, 243000, 251000, 259000,
267000, 275000, 283000, 291000,
299000, 307000, 315000, 323000,
331000, 339000, 347000, 355000,
363000, 371000, 379000, 387000,
395000, 403000, 411000, 419000,
427000, 435000, 443000, 451000,
459000, 467000, 474000, 482000,
490000, 498000, 506000, 514000,
522000, 530000, 538000, 546000,
554000, 562000, 570000, 578000,
586000, 594000, 602000, 610000,
618000, 626000, 634000, 642000,
650000, 658000, 666000, 674000,
682000, 690000, 698000, 706000,
714000, 722000, 730000, 738000,
746000, 754000, 762000, 770000,
778000, 786000, 794000, 802000,
810000, 818000, 826000, 834000,
842000, 850000, 858000, 866000,
874000
};


/*SQLite3 stmts*/
const char *sql_stmts[MAX_STMT] = 
{
	"select db_id from net_table where src=? and network_id=?",
	"insert into net_table(network_id, src) values(?,?)",
	"update net_table set name=? where db_id=?",
	"select db_id from ts_table where src=? and freq=? and db_sat_para_id=? and polar=?",
	"insert into ts_table(src,freq,db_sat_para_id,polar) values(?,?,?,?)",
	"update ts_table set db_net_id=?,ts_id=?,symb=?,mod=?,bw=?,snr=?,ber=?,strength=?,aud_fmt=?,vid_fmt=?,flags=0 where db_id=?",
	"delete  from srv_table where db_ts_id=?",
	"select db_id from srv_table where db_net_id=? and db_ts_id=? and service_id=?",
	"insert into srv_table(db_net_id, db_ts_id,service_id) values(?,?,?)",
	"update srv_table set src=?, name=?,service_type=?,eit_schedule_flag=?, eit_pf_flag=?,\
	 running_status=?,free_ca_mode=?,volume=?,aud_track=?,vid_pid=?,vid_fmt=?,aud_pids=?,aud_fmts=?,\
	 aud_langs=?,db_sub_id=-1,skip=0,lock=0,chan_num=?,major_chan_num=?,minor_chan_num=?,access_controlled=?,\
	 hidden=?,hide_guide=?, source_id=?,favor=?,current_aud=?,db_sat_para_id=?,scrambled_flag=?,lcn=-1,hd_lcn=-1,\
	 sd_lcn=-1,default_chan_num=-1,chan_order=0 where db_id=?",
	"select db_id,service_type from srv_table where db_ts_id=? order by service_id",
	"update srv_table set chan_num=? where db_id=?",
	"delete  from evt_table where db_ts_id=?",
	"select db_id from ts_table where src=? order by freq",
	"delete from grp_map_table where db_srv_id in (select db_id from srv_table where db_ts_id=?)",
	"insert into subtitle_table(db_srv_id,pid,type,composition_page_id,ancillary_page_id,language) values(?,?,?,?,?,?)",
	"insert into teletext_table(db_srv_id,pid,type,magazine_number,page_number,language) values(?,?,?,?,?,?)",
	"delete from subtitle_table where db_srv_id in (select db_id from srv_table where db_ts_id=?)",
	"delete from teletext_table where db_srv_id in (select db_id from srv_table where db_ts_id=?)",
	"select srv_table.service_id, ts_table.ts_id, net_table.network_id from srv_table, ts_table, net_table where srv_table.db_id=? and ts_table.db_id=srv_table.db_ts_id and net_table.db_id=srv_table.db_net_id",
	"update srv_table set skip=? where db_id=?",
	"select max(chan_num) from srv_table where service_type=? and src=?",
	"select max(chan_num) from srv_table where src=?",
	"select max(major_chan_num) from srv_table",
	"update srv_table set major_chan_num=?, chan_num=? where db_id=(select db_id from srv_table where db_ts_id=?)",
	"select db_id from srv_table where src=? and chan_num=?",
	"select db_id from sat_para_table where lnb_num=? and motor_num=? and pos_num=?",
	"select db_id from sat_para_table where lnb_num=? and sat_longitude=?",
	"insert into sat_para_table(sat_name,lnb_num,lof_hi,lof_lo,lof_threshold,signal_22khz,voltage,motor_num,pos_num,lo_direction,la_direction,\
	longitude,latitude,diseqc_mode,tone_burst,committed_cmd,uncommitted_cmd,repeat_count,sequence_repeat,fast_diseqc,cmd_order,sat_longitude) \
	values(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
	"select db_id from srv_table where src=? and lcn=?",
	"select max(lcn) from srv_table where src=?",
	"update srv_table set lcn=?,hd_lcn=?,sd_lcn=? where db_id=?",
	"update srv_table set default_chan_num=? where db_id=?",
	"select service_type from srv_table where db_id=?",
	"select max(default_chan_num) from srv_table where service_type=? and src=?",
	"select max(default_chan_num) from srv_table where src=?",
	"select db_id,hd_lcn from srv_table where src=? and sd_lcn=?",
	"select db_id,service_type from srv_table where src=? order by default_chan_num",
	"select db_id,service_type from srv_table where src=? order by lcn",
};

/****************************************************************************
 * static functions
 ***************************************************************************/
static AM_ErrorCode_t am_scan_start_next_ts(AM_SCAN_Scanner_t *scanner);
static AM_ErrorCode_t am_scan_request_section(AM_SCAN_Scanner_t *scanner, AM_SCAN_TableCtl_t *scl);
static AM_ErrorCode_t am_scan_request_pmts(AM_SCAN_Scanner_t *scanner);
static AM_ErrorCode_t am_scan_request_next_pmt(AM_SCAN_Scanner_t *scanner);
static AM_ErrorCode_t am_scan_try_nit(AM_SCAN_Scanner_t *scanner);
static AM_ErrorCode_t am_scan_start_dtv(AM_SCAN_Scanner_t *scanner);
static AM_ErrorCode_t am_scan_start_atv(AM_SCAN_Scanner_t *scanner);

extern AM_ErrorCode_t AM_SI_ConvertDVBTextCode(char *in_code,int in_len,char *out_code,int out_len);

static void am_scan_rec_tab_init(AM_SCAN_RecTab_t *tab)
{
	memset(tab, 0, sizeof(AM_SCAN_RecTab_t));
}

static void am_scan_rec_tab_release(AM_SCAN_RecTab_t *tab)
{
	if(tab->srv_ids){
		free(tab->srv_ids);
		free(tab->tses);
	}
}

static int am_scan_rec_tab_add_srv(AM_SCAN_RecTab_t *tab, int id, AM_SCAN_TS_t *ts)
{
	int i;

	for(i=0; i<tab->srv_cnt; i++){
		if(tab->srv_ids[i]==id)
			return 0;
	}
	
	if(tab->srv_cnt == tab->buf_size){
		int size = AM_MAX(tab->buf_size*2, 32);
		int *buf, *buf1;

		buf = realloc(tab->srv_ids, sizeof(int)*size);
		if(!buf)
			return -1;
		buf1 = realloc(tab->tses, sizeof(AM_SCAN_TS_t)*size);
		if(!buf1) {
			free(buf);
			return -1;
		}

		tab->buf_size = size;
		tab->srv_ids  = buf;
		tab->tses = buf1;
	}

	tab->srv_ids[tab->srv_cnt] = id;
	tab->tses[tab->srv_cnt++] = ts;

	return 0;
}

static int am_scan_rec_tab_have_src(AM_SCAN_RecTab_t *tab, int id)
{
	int i;

	for(i=0; i<tab->srv_cnt; i++){
		if(tab->srv_ids[i]==id)
			return 1;
	}

	return 0;
}

static int am_scan_format_atv_freq(int tmp_freq) 
{
	int tmp_val = 0;

	tmp_val = (tmp_freq % ATV_1MHZ) / ATV_10KHZ;

	if (tmp_val >= 0 && tmp_val <= 30) 
	{
		tmp_val = 25;
	} 
	else if (tmp_val >= 70 && tmp_val <= 80) 
	{
		tmp_val = 75;
	}

	tmp_freq = (tmp_freq / ATV_1MHZ) * ATV_1MHZ + tmp_val * ATV_10KHZ;

	return tmp_freq;
}


/**\brief Open VDIN device*/
static AM_ErrorCode_t am_scan_open_vdin(AM_SCAN_Scanner_t *scanner)
{
	struct tvin_parm_s vdin_para;
	
	if (scanner->atvctl.vdin_fd < 0)
	{
		char buf[128];
		
		snprintf(buf, sizeof(buf), "/dev/vdin%d", atv_start_para.afe_dev_id);
		scanner->atvctl.vdin_fd = open(buf, O_RDWR);
		if (scanner->atvctl.vdin_fd < 0)
		{
			AM_DEBUG(1, "Open %s failed", buf);
			return AM_FAILURE;
		}
	}

	/* Open port */
	memset(&vdin_para, 0, sizeof(vdin_para));
	vdin_para.port = TVIN_PORT_CVBS0;
	if (ioctl(scanner->atvctl.vdin_fd, TVIN_IOC_OPEN, &vdin_para) < 0) 
	{
	    AM_DEBUG(1, "Vdin open port[%d], error(%s)!", vdin_para.port, strerror(errno));
	}


	return AM_SUCCESS;
}

static AM_ErrorCode_t am_scan_close_vdin(AM_SCAN_Scanner_t *scanner)
{
if (scanner->atvctl.vdin_fd >= 0) 
{
//*************************ADD**********************************
if (ioctl(scanner->atvctl.vdin_fd, TVIN_IOC_CLOSE) < 0) 
{
            AM_DEBUG(1, "Vdin openTVIN_IOC_CLOSEport, error(%s)!", strerror(errno));
        }
//*************************FINISH**********************************
        close(scanner->atvctl.vdin_fd);
        scanner->atvctl.vdin_fd = -1;
}

	
	return AM_SUCCESS;
}


/**\brief 打开AFE设备*/
static AM_ErrorCode_t am_scan_open_afe(AM_SCAN_Scanner_t *scanner)
{
	if (scanner->atvctl.afe_fd < 0)
	{
		char buf[128];
		
		snprintf(buf, sizeof(buf), "/dev/tvafe%d", atv_start_para.afe_dev_id);
		scanner->atvctl.afe_fd = open(buf, O_RDWR);
		if (scanner->atvctl.afe_fd < 0)
		{
			AM_DEBUG(1, "Open %s failed", buf);
			return AM_FAILURE;
		}
	}
	
	return AM_SUCCESS;
}

/**\brief 关闭AFE设备*/
static AM_ErrorCode_t am_scan_close_afe(AM_SCAN_Scanner_t *scanner)
{
	if (scanner->atvctl.afe_fd >= 0)
	{
		close(scanner->atvctl.afe_fd);
		scanner->atvctl.afe_fd = -1;
	}
	
	return AM_SUCCESS;
}

/**\brief 获取CVBS lock状态*/
static AM_Bool_t am_scan_atv_cvbs_lock(AM_SCAN_Scanner_t *scanner)
{
	tvafe_cvbs_video_t cvbs_lock_status;
	int ret, i = 0;
	int condition_val = 3;

	if (scanner->atvctl.afe_fd >= 0)
	{
		while (i < 5)
		{
			ret = ioctl(scanner->atvctl.afe_fd, TVIN_IOC_G_AFE_CVBS_LOCK, &cvbs_lock_status);
			if (ret < 0) 
			{
				AM_DEBUG(1, "TVIN_IOC_G_AFE_CVBS_LOCK, error: return(%d), error(%s)!\n", ret, strerror(errno));
				break;
			} 
			else 
			{
				AM_DEBUG(1, "TVIN_IOC_G_AFE_CVBS_LOCK, value=%d.\n", cvbs_lock_status);
			}
			
			if (cvbs_lock_status == TVAFE_CVBS_VIDEO_H_LOCKED || cvbs_lock_status == TVAFE_CVBS_VIDEO_HV_LOCKED)
				return AM_TRUE;

			usleep(50*1000);
			i++;
		}
    }
    
    return AM_FALSE;
}

/**\brief 插入一个网络记录，返回其索引*/
static int insert_net(sqlite3_stmt **stmts, int src, int orig_net_id)
{
	int db_id = -1;

	/*query wether it exists*/
	sqlite3_bind_int(stmts[QUERY_NET], 1, src);
	sqlite3_bind_int(stmts[QUERY_NET], 2, orig_net_id);
	if (sqlite3_step(stmts[QUERY_NET]) == SQLITE_ROW)
	{
		db_id = sqlite3_column_int(stmts[QUERY_NET], 0);
	}
	sqlite3_reset(stmts[QUERY_NET]);

	/*if not exist , insert a new record*/
	if (db_id == -1)
	{
		sqlite3_bind_int(stmts[INSERT_NET], 1, orig_net_id);
		sqlite3_bind_int(stmts[INSERT_NET], 2, src);
		if (sqlite3_step(stmts[INSERT_NET]) == SQLITE_DONE)
		{
			sqlite3_bind_int(stmts[QUERY_NET], 1, src);
			sqlite3_bind_int(stmts[QUERY_NET], 2, orig_net_id);
			if (sqlite3_step(stmts[QUERY_NET]) == SQLITE_ROW)
			{
				db_id = sqlite3_column_int(stmts[QUERY_NET], 0);
			}
			sqlite3_reset(stmts[QUERY_NET]);
		}
		sqlite3_reset(stmts[INSERT_NET]);
	}

	return db_id;
}

/**\brief 插入一个TS记录，返回其索引*/
static int insert_ts(sqlite3_stmt **stmts, int src, int freq, int db_satpara_id, int polar)
{
	int db_id = -1;

	/*query wether it exists*/
	sqlite3_bind_int(stmts[QUERY_TS], 1, src);
	sqlite3_bind_int(stmts[QUERY_TS], 2, freq);
	sqlite3_bind_int(stmts[QUERY_TS], 3, db_satpara_id);
	sqlite3_bind_int(stmts[QUERY_TS], 4, polar);
	if (sqlite3_step(stmts[QUERY_TS]) == SQLITE_ROW)
	{
		db_id = sqlite3_column_int(stmts[QUERY_TS], 0);
	}
	sqlite3_reset(stmts[QUERY_TS]);

	/*if not exist , insert a new record*/
	if (db_id == -1)
	{
		sqlite3_bind_int(stmts[INSERT_TS], 1, src);
		sqlite3_bind_int(stmts[INSERT_TS], 2, freq);
		sqlite3_bind_int(stmts[INSERT_TS], 3, db_satpara_id);
		sqlite3_bind_int(stmts[INSERT_TS], 4, polar);
		if (sqlite3_step(stmts[INSERT_TS]) == SQLITE_DONE)
		{
			sqlite3_bind_int(stmts[QUERY_TS], 1, src);
			sqlite3_bind_int(stmts[QUERY_TS], 2, freq);
			sqlite3_bind_int(stmts[QUERY_TS], 3, db_satpara_id);
			sqlite3_bind_int(stmts[QUERY_TS], 4, polar);
			if (sqlite3_step(stmts[QUERY_TS]) == SQLITE_ROW)
			{
				db_id = sqlite3_column_int(stmts[QUERY_TS], 0);
			}
			sqlite3_reset(stmts[QUERY_TS]);
		}
		sqlite3_reset(stmts[INSERT_TS]);
	}

	return db_id;
}

/**\brief 插入一个SRV记录，返回其索引*/
static int insert_srv(sqlite3_stmt **stmts, int db_net_id, int db_ts_id, int srv_id)
{
	int db_id = -1;

	/*query wether it exists*/
	sqlite3_bind_int(stmts[QUERY_SRV], 1, db_net_id);
	sqlite3_bind_int(stmts[QUERY_SRV], 2, db_ts_id);
	sqlite3_bind_int(stmts[QUERY_SRV], 3, srv_id);
	if (sqlite3_step(stmts[QUERY_SRV]) == SQLITE_ROW)
	{
		db_id = sqlite3_column_int(stmts[QUERY_SRV], 0);
	}
	sqlite3_reset(stmts[QUERY_SRV]);

	/*if not exist , insert a new record*/
	if (db_id == -1)
	{
		sqlite3_bind_int(stmts[INSERT_SRV], 1, db_net_id);
		sqlite3_bind_int(stmts[INSERT_SRV], 2, db_ts_id);
		sqlite3_bind_int(stmts[INSERT_SRV], 3, srv_id);
		if (sqlite3_step(stmts[INSERT_SRV]) == SQLITE_DONE)
		{
			sqlite3_bind_int(stmts[QUERY_SRV], 1, db_net_id);
			sqlite3_bind_int(stmts[QUERY_SRV], 2, db_ts_id);
			sqlite3_bind_int(stmts[QUERY_SRV], 3, srv_id);
			if (sqlite3_step(stmts[QUERY_SRV]) == SQLITE_ROW)
			{
				db_id = sqlite3_column_int(stmts[QUERY_SRV], 0);
			}
			sqlite3_reset(stmts[QUERY_SRV]);
		}
		sqlite3_reset(stmts[INSERT_SRV]);
	}

	return db_id;
}

/**\brief 插入一个Subtitle记录*/
static int insert_subtitle(sqlite3_stmt **stmts, int db_srv_id, int pid, dvbpsi_subtitle_t *psd)
{
	sqlite3_bind_int(stmts[INSERT_SUBTITLE], 1, db_srv_id);
	sqlite3_bind_int(stmts[INSERT_SUBTITLE], 2, pid);
	sqlite3_bind_int(stmts[INSERT_SUBTITLE], 3, psd->i_subtitling_type);
	sqlite3_bind_int(stmts[INSERT_SUBTITLE], 4, psd->i_composition_page_id);
	sqlite3_bind_int(stmts[INSERT_SUBTITLE], 5, psd->i_ancillary_page_id);
	sqlite3_bind_text(stmts[INSERT_SUBTITLE], 6, (const char*)psd->i_iso6392_language_code, 3, SQLITE_STATIC);
	AM_DEBUG(1, "Insert a new subtitle");
	sqlite3_step(stmts[INSERT_SUBTITLE]);
	sqlite3_reset(stmts[INSERT_SUBTITLE]);

	return 0;
}

/**\brief 插入一个Teletext记录*/
static int insert_teletext(sqlite3_stmt **stmts, int db_srv_id, int pid, dvbpsi_teletextpage_t *ptd)
{
	sqlite3_bind_int(stmts[INSERT_TELETEXT], 1, db_srv_id);
	sqlite3_bind_int(stmts[INSERT_TELETEXT], 2, pid);
	if (ptd)
	{
		sqlite3_bind_int(stmts[INSERT_TELETEXT], 3, ptd->i_teletext_type);
		sqlite3_bind_int(stmts[INSERT_TELETEXT], 4, ptd->i_teletext_magazine_number);
		sqlite3_bind_int(stmts[INSERT_TELETEXT], 5, ptd->i_teletext_page_number);
		sqlite3_bind_text(stmts[INSERT_TELETEXT], 6, (const char*)ptd->i_iso6392_language_code, 3, SQLITE_STATIC);
	}
	else
	{
		sqlite3_bind_int(stmts[INSERT_TELETEXT], 3, 0);
		sqlite3_bind_int(stmts[INSERT_TELETEXT], 4, 1);
		sqlite3_bind_int(stmts[INSERT_TELETEXT], 5, 0);
		sqlite3_bind_text(stmts[INSERT_TELETEXT], 6, "", -1, SQLITE_STATIC);
	}
	AM_DEBUG(1, "Insert a new teletext");
	sqlite3_step(stmts[INSERT_TELETEXT]);
	sqlite3_reset(stmts[INSERT_TELETEXT]);

	return 0;
}

/**\brief 将audio数据格式化成字符串已便存入数据库*/
static void format_audio_strings(AM_SI_AudioInfo_t *ai, char *pids, char *fmts, char *langs)
{
	int i;
	
	if (ai->audio_count < 0)
		ai->audio_count = 0;
		
	pids[0] = 0;
	fmts[0] = 0;
	langs[0] = 0;
	for (i=0; i<ai->audio_count; i++)
	{
		if (i == 0)
		{
			sprintf(pids, "%d", ai->audios[i].pid);
			sprintf(fmts, "%d", ai->audios[i].fmt);
			sprintf(langs, "%s", ai->audios[i].lang);
		}
		else
		{
			sprintf(pids, "%s %d", pids, ai->audios[i].pid);
			sprintf(fmts, "%s %d", fmts, ai->audios[i].fmt);
			sprintf(langs, "%s %s", langs, ai->audios[i].lang);
		}
	}
}

/**\brief 查询一个satellite parameter记录是否已经存在 */
static int get_sat_para_record(sqlite3_stmt **stmts, AM_SCAN_DTVSatellitePara_t *sat_para)
{
	sqlite3_stmt *stmt;
	int db_id = -1;
	
	if (sat_para->sec.m_lnbs.m_cursat_parameters.m_rotorPosNum > 0)
	{
		/* by motor_num + position_num */
		stmt = stmts[QUERY_SAT_PARA_BY_POS_NUM];
		sqlite3_bind_int(stmt, 1, sat_para->lnb_num);
		sqlite3_bind_int(stmt, 2, sat_para->motor_num);
		sqlite3_bind_int(stmt, 3, sat_para->sec.m_lnbs.m_cursat_parameters.m_rotorPosNum);
	}
	else
	{
		/* by satellite longitude*/
		stmt = stmts[QUERY_SAT_PARA_BY_LO_LA];
		sqlite3_bind_int(stmt, 1, sat_para->lnb_num);
		sqlite3_bind_double(stmt, 2, sat_para->sec.m_lnbs.m_rotor_parameters.m_gotoxx_parameters.m_sat_longitude);
	}
	
	if (sqlite3_step(stmt) == SQLITE_ROW)
	{
		db_id = sqlite3_column_int(stmt, 0);
	}
	sqlite3_reset(stmt);
	
	return db_id;
}

/**\brief 添加一个satellite parameter记录，如果已存在，则返回已存在的记录db_id*/
static int insert_sat_para(sqlite3_stmt **stmts, AM_SCAN_DTVSatellitePara_t *sat_para)
{
	sqlite3_stmt *stmt;
	int db_id = -1;
	
	/* Already exist ? */
	db_id = get_sat_para_record(stmts, sat_para);

	/* insert a new record */
	if (db_id == -1)
	{
		AM_DEBUG(1, "@@ Insert a new satellite parameter record !!! @@");
		AM_DEBUG(1, "name(%s),lnb_num(%d),lof_hi(%d),lof_lo(%d),lof_threshold(%d),22k(%d),vol(%d),motor(%d),pos_num(%d),lodir(%d),ladir(%d),\
		lo(%f),la(%f),diseqc_mode(%d),toneburst(%d),cdc(%d),ucdc(%d),repeats(%d),seq_repeat(%d),fast_diseqc(%d),cmd_order(%d), sat_longitude(%d)",
		sat_para->sat_name,sat_para->lnb_num,sat_para->sec.m_lnbs.m_lof_hi,sat_para->sec.m_lnbs.m_lof_lo,sat_para->sec.m_lnbs.m_lof_threshold,
		sat_para->sec.m_lnbs.m_cursat_parameters.m_22khz_signal,sat_para->sec.m_lnbs.m_cursat_parameters.m_voltage_mode,
		sat_para->motor_num,sat_para->sec.m_lnbs.m_cursat_parameters.m_rotorPosNum,
		0,
		0,
		sat_para->sec.m_lnbs.m_rotor_parameters.m_gotoxx_parameters.m_longitude,
		sat_para->sec.m_lnbs.m_rotor_parameters.m_gotoxx_parameters.m_latitude,
		sat_para->sec.m_lnbs.m_diseqc_parameters.m_diseqc_mode,
		sat_para->sec.m_lnbs.m_diseqc_parameters.m_toneburst_param,
		sat_para->sec.m_lnbs.m_diseqc_parameters.m_committed_cmd,
		sat_para->sec.m_lnbs.m_diseqc_parameters.m_uncommitted_cmd,
		sat_para->sec.m_lnbs.m_diseqc_parameters.m_repeats,
		sat_para->sec.m_lnbs.m_diseqc_parameters.m_seq_repeat,
		sat_para->sec.m_lnbs.m_diseqc_parameters.m_use_fast,
		sat_para->sec.m_lnbs.m_diseqc_parameters.m_command_order,
		sat_para->sec.m_lnbs.m_rotor_parameters.m_gotoxx_parameters.m_sat_longitude);
		stmt = stmts[INSERT_SAT_PARA];
		sqlite3_bind_text(stmt, 1, sat_para->sat_name, strlen(sat_para->sat_name), SQLITE_STATIC);
		sqlite3_bind_int(stmt, 2, sat_para->lnb_num);
		sqlite3_bind_int(stmt, 3, sat_para->sec.m_lnbs.m_lof_hi);
		sqlite3_bind_int(stmt, 4, sat_para->sec.m_lnbs.m_lof_lo);
		sqlite3_bind_int(stmt, 5, sat_para->sec.m_lnbs.m_lof_threshold);
		sqlite3_bind_int(stmt, 6, sat_para->sec.m_lnbs.m_cursat_parameters.m_22khz_signal);
		sqlite3_bind_int(stmt, 7, sat_para->sec.m_lnbs.m_cursat_parameters.m_voltage_mode);
		sqlite3_bind_int(stmt, 8, sat_para->motor_num);
		sqlite3_bind_int(stmt, 9, sat_para->sec.m_lnbs.m_cursat_parameters.m_rotorPosNum);
		sqlite3_bind_int(stmt, 10, 0);
		sqlite3_bind_int(stmt, 11, 0);
		sqlite3_bind_double(stmt, 12, sat_para->sec.m_lnbs.m_rotor_parameters.m_gotoxx_parameters.m_longitude);
		sqlite3_bind_double(stmt, 13, sat_para->sec.m_lnbs.m_rotor_parameters.m_gotoxx_parameters.m_latitude);
		sqlite3_bind_int(stmt, 14, sat_para->sec.m_lnbs.m_diseqc_parameters.m_diseqc_mode);
		sqlite3_bind_int(stmt, 15, sat_para->sec.m_lnbs.m_diseqc_parameters.m_toneburst_param);
		sqlite3_bind_int(stmt, 16, sat_para->sec.m_lnbs.m_diseqc_parameters.m_committed_cmd);
		sqlite3_bind_int(stmt, 17, sat_para->sec.m_lnbs.m_diseqc_parameters.m_uncommitted_cmd);
		sqlite3_bind_int(stmt, 18, sat_para->sec.m_lnbs.m_diseqc_parameters.m_repeats);
		sqlite3_bind_int(stmt, 19, sat_para->sec.m_lnbs.m_diseqc_parameters.m_seq_repeat ? 1 : 0);
		sqlite3_bind_int(stmt, 20, sat_para->sec.m_lnbs.m_diseqc_parameters.m_use_fast ? 1 : 0);
		sqlite3_bind_int(stmt, 21, sat_para->sec.m_lnbs.m_diseqc_parameters.m_command_order);
		sqlite3_bind_int(stmt, 22, sat_para->sec.m_lnbs.m_rotor_parameters.m_gotoxx_parameters.m_sat_longitude);
		
		if (sqlite3_step(stmt) == SQLITE_DONE)
			db_id = get_sat_para_record(stmts, sat_para);
	
		sqlite3_reset(stmt);
	}
		
	return db_id;
}

/**\brief 获取一个TS所在网络*/
static int am_scan_get_ts_network(sqlite3_stmt **stmts, int src, dvbpsi_nit_t *nits, AM_SCAN_TS_t *ts)
{
	int orig_net_id = -1, net_dbid = -1;
	dvbpsi_nit_t *nit;
	dvbpsi_descriptor_t *descr;
		
	if (ts->type == FE_ANALOG)
		return net_dbid;
	
	/*获取所在网络*/
	if (ts->digital.sdts)
	{
		/*按照SDT中描述的orignal_network_id查找网络记录，不存在则新添加一个network记录*/
		orig_net_id = ts->digital.sdts->i_network_id;
	}

	if (orig_net_id != -1)
	{
		net_dbid = insert_net(stmts, src, orig_net_id);
		if (net_dbid != -1)
		{
			char netname[256];

			netname[0] = '\0';
			/*查找网络名称*/
			if (nits && (orig_net_id == nits->i_network_id))
			{
				AM_SI_LIST_BEGIN(nits, nit)
				AM_SI_LIST_BEGIN(nit->p_first_descriptor, descr)
				if (descr->p_decoded && descr->i_tag == AM_SI_DESCR_NETWORK_NAME)
				{
					dvbpsi_network_name_dr_t *pnn = (dvbpsi_network_name_dr_t*)descr->p_decoded;

					/*取网络名称*/
					if (descr->i_length > 0)
					{
						AM_SI_ConvertDVBTextCode((char*)pnn->i_network_name, descr->i_length,\
									netname, 255);
						netname[255] = 0;
						break;
					}
				}
				AM_SI_LIST_END()	
				AM_SI_LIST_END()
			}

			AM_DEBUG(1, "###Network Name is '%s'", netname);
			sqlite3_bind_text(stmts[UPDATE_NET], 1, netname, -1, SQLITE_STATIC);
			sqlite3_bind_int(stmts[UPDATE_NET], 2, net_dbid);
			sqlite3_step(stmts[UPDATE_NET]);
			sqlite3_reset(stmts[UPDATE_NET]);
		}
		else
		{
			AM_DEBUG(1, "insert new network error");
		}
	}
	
	return net_dbid;
}

/***\brief 更新一个TS数据*/
static void am_scan_update_ts_info(sqlite3_stmt **stmts, int net_dbid, int dbid, AM_SCAN_TS_t *ts)
{
	/*更新TS数据*/
	sqlite3_bind_int(stmts[UPDATE_TS], 1, net_dbid);
	sqlite3_bind_int(stmts[UPDATE_TS], 2, (ts->type!=AM_SCAN_TS_ANALOG) ? ts->digital.pats->i_ts_id : -1);
	sqlite3_bind_int(stmts[UPDATE_TS], 3, (ts->type!=AM_SCAN_TS_ANALOG) ? (int)dvb_fend_para(ts->digital.fend_para)->u.qam.symbol_rate : 0);
	sqlite3_bind_int(stmts[UPDATE_TS], 4, (ts->type!=AM_SCAN_TS_ANALOG) ? (int)dvb_fend_para(ts->digital.fend_para)->u.qam.modulation : 0);
	sqlite3_bind_int(stmts[UPDATE_TS], 5, (ts->type!=AM_SCAN_TS_ANALOG) ? (int)dvb_fend_para(ts->digital.fend_para)->u.ofdm.bandwidth : 0);
	sqlite3_bind_int(stmts[UPDATE_TS], 6, (ts->type!=AM_SCAN_TS_ANALOG) ? ts->digital.snr : 0);
	sqlite3_bind_int(stmts[UPDATE_TS], 7, (ts->type!=AM_SCAN_TS_ANALOG) ? ts->digital.ber : 0);
	sqlite3_bind_int(stmts[UPDATE_TS], 8, (ts->type!=AM_SCAN_TS_ANALOG) ? ts->digital.strength : 0);
	sqlite3_bind_int(stmts[UPDATE_TS], 9, (ts->type==AM_SCAN_TS_ANALOG) ? ts->analog.audio_std : -1);
	sqlite3_bind_int(stmts[UPDATE_TS], 10,(ts->type==AM_SCAN_TS_ANALOG) ?  ts->analog.video_std : -1);
	sqlite3_bind_int(stmts[UPDATE_TS], 11, dbid);
	sqlite3_step(stmts[UPDATE_TS]);
	sqlite3_reset(stmts[UPDATE_TS]);

	/*清除与该TS下service关联的分组数据*/
	sqlite3_bind_int(stmts[DELETE_SRV_GRP], 1, dbid);
	sqlite3_step(stmts[DELETE_SRV_GRP]);
	sqlite3_reset(stmts[DELETE_SRV_GRP]);

	/*清除该TS下所有subtitles,teletexts*/
	AM_DEBUG(1, "Delete all subtitles & teletexts in TS %d", dbid);
	sqlite3_bind_int(stmts[DELETE_TS_SUBTITLES], 1, dbid);
	sqlite3_step(stmts[DELETE_TS_SUBTITLES]);
	sqlite3_reset(stmts[DELETE_TS_SUBTITLES]);
	sqlite3_bind_int(stmts[DELETE_TS_TELETEXTS], 1, dbid);
	sqlite3_step(stmts[DELETE_TS_TELETEXTS]);
	sqlite3_reset(stmts[DELETE_TS_TELETEXTS]);
	
	/*清除该TS下所有service*/
	sqlite3_bind_int(stmts[DELETE_TS_SRVS], 1, dbid);
	sqlite3_step(stmts[DELETE_TS_SRVS]);
	sqlite3_reset(stmts[DELETE_TS_SRVS]);

	/*清除该TS下所有events*/
	AM_DEBUG(1, "Delete all events in TS %d", dbid);
	sqlite3_bind_int(stmts[DELETE_TS_EVTS], 1, dbid);
	sqlite3_step(stmts[DELETE_TS_EVTS]);
	sqlite3_reset(stmts[DELETE_TS_EVTS]);
}

/**\brief 初始化一个service结构*/
static void am_scan_init_service_info(AM_SCAN_ServiceInfo_t *srv_info)
{
	memset(srv_info, 0, sizeof(AM_SCAN_ServiceInfo_t));
	srv_info->vid = 0x1fff;
	srv_info->free_ca = 1;
	srv_info->srv_id = 0xffff;
	srv_info->srv_dbid = -1;
	srv_info->satpara_dbid = -1;
}

/**\brief 更新一个service数据到数据库*/
static void am_scan_update_service_info(sqlite3_stmt **stmts, int standard, AM_SCAN_ServiceInfo_t *srv_info)
{
	if (standard >= 0)
	{
		if (standard != AM_SCAN_DTV_STD_ATSC)
		{
			if (srv_info->srv_type == 0x1)
				srv_info->srv_type = AM_SCAN_SRV_DTV;
			else if (srv_info->srv_type == 0x2)
				srv_info->srv_type = AM_SCAN_SRV_DRADIO;
		} 
		else 
		{
			if (srv_info->srv_type == 0x2)
				srv_info->srv_type = AM_SCAN_SRV_DTV;
			else if (srv_info->srv_type == 0x3)
				srv_info->srv_type = AM_SCAN_SRV_DRADIO;
		}
	}
	
	if (srv_info->vid != 0x1fff)
		srv_info->srv_type = AM_SCAN_SRV_DTV;
	else if (srv_info->vid == 0x1fff && srv_info->aud_info.audio_count > 0 && srv_info->aud_info.audios[0].pid < 0x1fff)
		srv_info->srv_type = AM_SCAN_SRV_DRADIO;
	/*SDT/VCT描述为TV 或 Radio的节目，但音视频PID无效，不存储为TV或Radio*/
	if (srv_info->vid == 0x1fff && srv_info->aud_info.audio_count <= 0 && 
		(srv_info->srv_type == AM_SCAN_SRV_DTV || srv_info->srv_type == AM_SCAN_SRV_DRADIO))
		srv_info->srv_type = AM_SCAN_SRV_UNKNOWN;
	
	if (!strcmp(srv_info->name, "") && (srv_info->srv_type == AM_SCAN_SRV_DTV || srv_info->srv_type == AM_SCAN_SRV_DRADIO) && 
		standard != AM_SCAN_DTV_STD_ATSC)
		strcpy(srv_info->name, "No Name");
	
	/* Update to database */	
	sqlite3_bind_int(stmts[UPDATE_SRV], 1, srv_info->src);
	sqlite3_bind_text(stmts[UPDATE_SRV], 2, srv_info->name, strlen(srv_info->name), SQLITE_STATIC);
	sqlite3_bind_int(stmts[UPDATE_SRV], 3, srv_info->srv_type);
	sqlite3_bind_int(stmts[UPDATE_SRV], 4, srv_info->eit_sche);
	sqlite3_bind_int(stmts[UPDATE_SRV], 5, srv_info->eit_pf);
	sqlite3_bind_int(stmts[UPDATE_SRV], 6, srv_info->rs);
	sqlite3_bind_int(stmts[UPDATE_SRV], 7, srv_info->free_ca);
	sqlite3_bind_int(stmts[UPDATE_SRV], 8, 50);
	sqlite3_bind_int(stmts[UPDATE_SRV], 9, AM_AOUT_OUTPUT_DUAL_LEFT);
	sqlite3_bind_int(stmts[UPDATE_SRV], 10, srv_info->vid);
	sqlite3_bind_int(stmts[UPDATE_SRV], 11, srv_info->vfmt);
	sqlite3_bind_text(stmts[UPDATE_SRV], 12, srv_info->str_apids, strlen(srv_info->str_apids), SQLITE_STATIC);
	sqlite3_bind_text(stmts[UPDATE_SRV], 13, srv_info->str_afmts, strlen(srv_info->str_afmts), SQLITE_STATIC);
	sqlite3_bind_text(stmts[UPDATE_SRV], 14, srv_info->str_alangs, strlen(srv_info->str_alangs), SQLITE_STATIC);
	sqlite3_bind_int(stmts[UPDATE_SRV], 15, srv_info->chan_num);
	sqlite3_bind_int(stmts[UPDATE_SRV], 16, srv_info->major_chan_num);
	sqlite3_bind_int(stmts[UPDATE_SRV], 17, srv_info->minor_chan_num);
	sqlite3_bind_int(stmts[UPDATE_SRV], 18, srv_info->access_controlled);
	sqlite3_bind_int(stmts[UPDATE_SRV], 19, srv_info->hidden);
	sqlite3_bind_int(stmts[UPDATE_SRV], 20, srv_info->hide_guide);
	sqlite3_bind_int(stmts[UPDATE_SRV], 21, srv_info->source_id);
	sqlite3_bind_int(stmts[UPDATE_SRV], 22, 0);
	sqlite3_bind_int(stmts[UPDATE_SRV], 23, -1);
	sqlite3_bind_int(stmts[UPDATE_SRV], 24, srv_info->satpara_dbid);
	sqlite3_bind_int(stmts[UPDATE_SRV], 25, srv_info->scrambled_flag);
	sqlite3_bind_int(stmts[UPDATE_SRV], 26, srv_info->srv_dbid);
	sqlite3_step(stmts[UPDATE_SRV]);
	sqlite3_reset(stmts[UPDATE_SRV]);
	AM_DEBUG(1, "Updating Program: '%s',srv_type(%d), vpid(%d), vfmt(%d),apids(%s), afmts(%s), alangs(%s) ",
		srv_info->name,srv_info->srv_type,srv_info->vid,srv_info->vfmt,
		srv_info->str_apids, srv_info->str_afmts, srv_info->str_alangs);
}

/**\brief 从一个ES流中提取teletext & subtitle*/
static void am_scan_extract_ttx_sub_from_es(sqlite3_stmt **stmts, int srv_dbid, dvbpsi_pmt_es_t *es)
{
	dvbpsi_descriptor_t *descr;
	
	/*查找Subtilte和Teletext描述符，并添加相关记录*/
	AM_SI_LIST_BEGIN(es->p_first_descriptor, descr)
		if (descr->p_decoded && descr->i_tag == AM_SI_DESCR_SUBTITLING)
		{
			int isub;
			dvbpsi_subtitling_dr_t *psd = (dvbpsi_subtitling_dr_t*)descr->p_decoded;

			AM_DEBUG(1, "Find subtitle descriptor, number:%d",psd->i_subtitles_number);
			for (isub=0; isub<psd->i_subtitles_number; isub++)
			{
				insert_subtitle(stmts, srv_dbid, es->i_pid, &psd->p_subtitle[isub]);
			}
		}
		else if (descr->i_tag == AM_SI_DESCR_TELETEXT)
		{
			int itel;
			dvbpsi_teletext_dr_t *ptd = (dvbpsi_teletext_dr_t*)descr->p_decoded;

			AM_DEBUG(1, "Find teletext descriptor, ptd %p", ptd);
			if (ptd)
			{
				for (itel=0; itel<ptd->i_pages_number; itel++)
				{
					insert_teletext(stmts, srv_dbid, es->i_pid, &ptd->p_pages[itel]);
				}
			}
			else
			{
				insert_teletext(stmts, srv_dbid, es->i_pid, NULL);
			}
		}
	AM_SI_LIST_END()
}

/**\brief 提取CA加扰标识*/
static void am_scan_extract_ca_scrambled_flag(dvbpsi_pmt_es_t *es, int *flag)
{
	dvbpsi_descriptor_t *descr;
	
	AM_SI_LIST_BEGIN(es->p_first_descriptor, descr)
		if (descr->i_tag == AM_SI_DESCR_CA && ! *flag)
		{
			AM_DEBUG(1, "Found CA descr, set scrambled flag to 1");
			*flag = 1;
			break;
		}
	AM_SI_LIST_END()
}

/**\brief 从SDT表获取相关service信息*/
static void am_scan_extract_srv_info_from_sdt(dvbpsi_sdt_t *sdts, AM_SCAN_ServiceInfo_t *srv_info)
{
	dvbpsi_sdt_service_t *srv;
	dvbpsi_sdt_t *sdt;
	dvbpsi_descriptor_t *descr;
	
	AM_SI_LIST_BEGIN(sdts, sdt)
	AM_SI_LIST_BEGIN(sdt->p_first_service, srv)
		/*从SDT表中查找该service并获取信息*/
		if (srv->i_service_id == srv_info->srv_id)
		{
			AM_DEBUG(1 ,"SDT for service %d found!", srv_info->srv_id);
			srv_info->eit_sche = (uint8_t)srv->b_eit_schedule;
			srv_info->eit_pf = (uint8_t)srv->b_eit_present;
			srv_info->rs = srv->i_running_status;
			srv_info->free_ca = (uint8_t)srv->b_free_ca;
		
			AM_SI_LIST_BEGIN(srv->p_first_descriptor, descr)
			if (descr->p_decoded && descr->i_tag == AM_SI_DESCR_SERVICE)
			{
				dvbpsi_service_dr_t *psd = (dvbpsi_service_dr_t*)descr->p_decoded;
		
				/*取节目名称*/
				if (psd->i_service_name_length > 0)
				{
					AM_SI_ConvertDVBTextCode((char*)psd->i_service_name, psd->i_service_name_length,\
								srv_info->name, AM_DB_MAX_SRV_NAME_LEN);
					srv_info->name[AM_DB_MAX_SRV_NAME_LEN] = 0;
				}
				/*业务类型*/
				srv_info->srv_type = psd->i_service_type;
				/*service type 0x16 and 0x19 is user defined, as digital television service*/
				/*service type 0xc0 is type of partial reception service in ISDBT*/
				if((srv_info->srv_type == 0x16) || (srv_info->srv_type == 0x19) || (srv_info->srv_type == 0xc0))
				{
					srv_info->srv_type = 0x1;
				}
			
				return ;
			}
			AM_SI_LIST_END()
		}
	AM_SI_LIST_END()
	AM_SI_LIST_END()
}

static void add_audio(AM_SI_AudioInfo_t *ai, int aud_pid, int aud_fmt, char lang[3])
{
	int i;
	
	for (i=0; i<ai->audio_count; i++)
	{
		if (ai->audios[i].pid == aud_pid)
			return;
	}
	if (ai->audio_count >= AM_SI_MAX_AUD_CNT)
	{
		AM_DEBUG(1, "Too many audios, Max count %d", AM_SI_MAX_AUD_CNT);
		return;
	}
	if (ai->audio_count < 0)
		ai->audio_count = 0;
	ai->audios[ai->audio_count].pid = aud_pid;
	ai->audios[ai->audio_count].fmt = aud_fmt;
	memset(ai->audios[ai->audio_count].lang, 0, sizeof(ai->audios[ai->audio_count].lang));
	if (lang[0] != 0)
	{
		memcpy(ai->audios[ai->audio_count].lang, lang, 3);
	}
	else
	{
		sprintf(ai->audios[ai->audio_count].lang, "Audio%d", ai->audio_count+1);
	}
	
	AM_DEBUG(1, "Add a audio: pid %d, language: %s", aud_pid, ai->audios[ai->audio_count].lang);

	ai->audio_count++;
}

/**\brief Store a Analog TS to database */
static void store_analog_ts(sqlite3_stmt **stmts, AM_SCAN_Result_t *result, AM_SCAN_TS_t *ts)
{
	int dbid;
	AM_SCAN_ServiceInfo_t srv_info;
	int std = result->start_para->dtv_para.standard;
	
	AM_DEBUG(1, "@@ Storing a analog ts @@");
		
	/*检查该TS是否已经添加*/
	dbid = insert_ts(stmts, FE_ANALOG, ts->analog.freq, -1, -1);
	if (dbid == -1)
	{
		AM_DEBUG(1, "insert new ts error");
		return;
	}
	
	/* 更新TS数据 */
	am_scan_update_ts_info(stmts,  -1, dbid, ts);
	
	/* 存储ATV频道 */
	if (ts->type == AM_SCAN_TS_ANALOG)
	{
		char lang_tmp[3];
		
		am_scan_init_service_info(&srv_info);
		srv_info.satpara_dbid = -1;
		srv_info.src = FE_ANALOG;
		
		/*添加新业务到数据库*/
		srv_info.srv_dbid = insert_srv(stmts, -1, dbid, 0xffff);
		if (srv_info.srv_dbid == -1)
		{
			AM_DEBUG(1, "insert new srv error");
			return;
		}
		srv_info.vfmt = ts->analog.video_std;
		memset(lang_tmp, 0, sizeof(lang_tmp));
		add_audio(&srv_info.aud_info, 0x1fff, ts->analog.audio_std, lang_tmp);
		format_audio_strings(&srv_info.aud_info, srv_info.str_apids, srv_info.str_afmts, srv_info.str_alangs);
		srv_info.chan_num = 0;
		srv_info.srv_type = AM_SCAN_SRV_ATV; 
		strcpy(srv_info.name, "ATV Program");
		/* 更新service数据 */
		am_scan_update_service_info(stmts, std, &srv_info);
		
		return;
	}
}

/**\brief 存储一个TS到数据库, ATSC*/
static void store_atsc_ts(sqlite3_stmt **stmts, AM_SCAN_Result_t *result, AM_SCAN_TS_t *ts)
{
	AM_DEBUG(1, "%s: DONOT SUPPORT YET !!!", __func__);
}

/**\brief 存储一个TS到数据库, DVB*/
static void store_dvb_ts(sqlite3_stmt **stmts, AM_SCAN_Result_t *result, AM_SCAN_TS_t *ts, AM_SCAN_RecTab_t *tab)
{
	dvbpsi_pmt_t *pmt;
	dvbpsi_pmt_es_t *es;
	dvbpsi_descriptor_t *descr;
	int src = result->start_para->dtv_para.source;
	int std = result->start_para->dtv_para.standard;
	int net_dbid, dbid, orig_net_id = -1, satpara_dbid = -1;
	char selbuf[256];
	char insbuf[400];
	AM_SCAN_ServiceInfo_t srv_info;
	int frequency;
	sqlite3 *hdb;

	AM_DB_HANDLE_PREPARE(hdb);
	
	/*没有PAT，不存储*/
	if (!ts->digital.pats)
	{
		AM_DEBUG(1, "No PAT found in ts, will not store to dbase");
		return;
	}
	if (src == FE_QPSK)
	{
		/* 存储卫星配置 */
		satpara_dbid = insert_sat_para(stmts, &result->start_para->dtv_para.sat_para);
		if (satpara_dbid == -1)
		{
			AM_DEBUG(1, "Cannot insert satellite parameter!");
			return;
		}
	}
	
	AM_DEBUG(1, "@@ Storing ts, source is %d @@", src);
	net_dbid = am_scan_get_ts_network(stmts, src, result->nits, ts);
	
	/*检查该TS是否已经添加*/
	dbid = insert_ts(stmts, src, (int)dvb_fend_para(ts->digital.fend_para)->frequency, satpara_dbid,
		(src==FE_QPSK)?(int)ts->digital.fend_para.sat.polarisation:-1);
	if (dbid == -1)
	{
		AM_DEBUG(1, "insert new ts error");
		return;
	}
	
	/* 更新TS数据 */
	am_scan_update_ts_info(stmts, net_dbid, dbid, ts);

	/*存储DTV节目，遍历PMT表*/
	AM_SI_LIST_BEGIN(ts->digital.pmts, pmt)
		am_scan_init_service_info(&srv_info);
		srv_info.satpara_dbid = satpara_dbid;
		srv_info.srv_id = pmt->i_program_number;
		srv_info.src = src;
		
		/*添加新业务到数据库*/
		srv_info.srv_dbid = insert_srv(stmts, net_dbid, dbid, srv_info.srv_id);
		if (srv_info.srv_dbid == -1)
		{
			AM_DEBUG(1, "insert new srv error");
			continue;
		}

		am_scan_rec_tab_add_srv(tab, srv_info.srv_dbid, ts);
		
		/* looking for CA descr */
		if (! srv_info.scrambled_flag)
		{
			AM_SI_LIST_BEGIN(pmt-> p_first_descriptor, descr)
				if (descr->i_tag == AM_SI_DESCR_CA && ! srv_info.scrambled_flag)
				{
					AM_DEBUG(1, "Found CA descr, set scrambled flag to 1");
					srv_info.scrambled_flag = 1;
					break;
				}
			AM_SI_LIST_END()
		}

		/*取ES流信息*/
		AM_SI_LIST_BEGIN(pmt->p_first_es, es)
			/* 提取音视频流 */
			AM_SI_ExtractAVFromDVBES(es, &srv_info.vid, &srv_info.vfmt, &srv_info.aud_info);
			/* 提取subtitle & teletext */
			am_scan_extract_ttx_sub_from_es(stmts, srv_info.srv_dbid, es);
			/* 查找CA加扰标识 */
			if (! srv_info.scrambled_flag)
				am_scan_extract_ca_scrambled_flag(es, &srv_info.scrambled_flag);
		AM_SI_LIST_END()
		
		/*格式化音频数据字符串*/
		format_audio_strings(&srv_info.aud_info, srv_info.str_apids, srv_info.str_afmts, srv_info.str_alangs);
		/*获取节目名称，类型等信息*/
		am_scan_extract_srv_info_from_sdt(ts->digital.sdts, &srv_info);

		/*Store this service*/
		am_scan_update_service_info(stmts, std, &srv_info);
	AM_SI_LIST_END()
}

/**\brief 清除数据库中某个源的所有数据*/
static void am_scan_clear_source(sqlite3 *hdb, int src)
{
	char sqlstr[128];
	
	/*删除network记录*/
	snprintf(sqlstr, sizeof(sqlstr), "delete from net_table where src=%d",src);
	sqlite3_exec(hdb, sqlstr, NULL, NULL, NULL);
	/*清空TS记录*/
	snprintf(sqlstr, sizeof(sqlstr), "delete from ts_table where src=%d",src);
	sqlite3_exec(hdb, sqlstr, NULL, NULL, NULL);
	/*清空service group记录*/
	snprintf(sqlstr, sizeof(sqlstr), "delete from grp_map_table where db_srv_id in (select db_id from srv_table where src=%d)",src);
	sqlite3_exec(hdb, sqlstr, NULL, NULL, NULL);
	/*清空subtitle teletext记录*/
	snprintf(sqlstr, sizeof(sqlstr), "delete from subtitle_table where db_srv_id in (select db_id from srv_table where src=%d)",src);
	sqlite3_exec(hdb, sqlstr, NULL, NULL, NULL);
	snprintf(sqlstr, sizeof(sqlstr), "delete from teletext_table where db_srv_id in (select db_id from srv_table where src=%d)",src);
	sqlite3_exec(hdb, sqlstr, NULL, NULL, NULL);
	/*清空SRV记录*/
	snprintf(sqlstr, sizeof(sqlstr), "delete from srv_table where src=%d",src);
	sqlite3_exec(hdb, sqlstr, NULL, NULL, NULL);
	/*清空event记录*/
	snprintf(sqlstr, sizeof(sqlstr), "delete from evt_table where src=%d",src);
	sqlite3_exec(hdb, sqlstr, NULL, NULL, NULL);
}

/**\brief 从一个NIT中查找某个service的LCN*/
static AM_Bool_t am_scan_get_lcn_from_nit(int org_net_id, int ts_id, int srv_id, dvbpsi_nit_t *nits, 
int *sd_lcn, int *sd_visible, int *hd_lcn, int *hd_visible)
{
	dvbpsi_nit_ts_t *ts;
	dvbpsi_descriptor_t *dr;
	dvbpsi_nit_t *nit;
	
	*sd_lcn = -1;
	*hd_lcn = -1;
	AM_SI_LIST_BEGIN(nits, nit)
		AM_SI_LIST_BEGIN(nit->p_first_ts, ts)
			if(ts->i_ts_id==ts_id && ts->i_orig_network_id==org_net_id){
				AM_SI_LIST_BEGIN(ts->p_first_descriptor, dr)
					if(dr->p_decoded && ((dr->i_tag == AM_SI_DESCR_LCN_83))){
						if(dr->i_tag==AM_SI_DESCR_LCN_83)
						{
							dvbpsi_logical_channel_number_83_dr_t *lcn_dr = (dvbpsi_logical_channel_number_83_dr_t*)dr->p_decoded;
							dvbpsi_logical_channel_number_83_t *lcn = lcn_dr->p_logical_channel_number;
							int j;

							for(j=0; j<lcn_dr->i_logical_channel_numbers_number; j++){
								if(lcn->i_service_id == srv_id){
									*sd_lcn = lcn->i_logical_channel_number;
									*sd_visible = lcn->i_visible_service_flag;
									AM_DEBUG(1, "sd lcn for service %d ---> %d", srv_id, *sd_lcn);
									if (*hd_lcn == -1) {
										/* break to wait for lcn88 */
										break;
									} else {
										goto lcn_found;
									}
								}
								lcn++;
							}
						}
					}
					else if (dr->i_tag==AM_SI_DESCR_LCN_88)
					{
						dvbpsi_logical_channel_number_88_dr_t *lcn_dr = (dvbpsi_logical_channel_number_88_dr_t*)dr->p_decoded;
						dvbpsi_logical_channel_number_88_t *lcn = lcn_dr->p_logical_channel_number;
						int j;

						for(j=0; j<lcn_dr->i_logical_channel_numbers_number; j++){
							if(lcn->i_service_id == srv_id){
								*hd_lcn = lcn->i_logical_channel_number;
								*hd_visible = lcn->i_visible_service_flag;
								if (*sd_lcn == -1) {
									/* break to wait for lcn83 */
									break;
								} else {
									goto lcn_found;
								}
							}
							lcn++;
						}
					}
				AM_SI_LIST_END()
			}
		AM_SI_LIST_END()
	AM_SI_LIST_END()
	
lcn_found:
	if (*sd_lcn != -1 || *hd_lcn != -1)
		return AM_TRUE;
	
	return AM_FALSE;
}

/**\brief LCN排序处理*/
static void am_scan_lcn_proc(AM_SCAN_Result_t *result, sqlite3_stmt **stmts, AM_SCAN_RecTab_t *srv_tab)
{
#define LCN_CONFLICT_START 900
#define UPDATE_SRV_LCN(_l, _hl, _sl, _d) \
AM_MACRO_BEGIN\
	sqlite3_bind_int(stmts[UPDATE_LCN], 1, _l);\
	sqlite3_bind_int(stmts[UPDATE_LCN], 2, _hl);\
	sqlite3_bind_int(stmts[UPDATE_LCN], 3, _sl);\
	sqlite3_bind_int(stmts[UPDATE_LCN], 4, _d);\
	sqlite3_step(stmts[UPDATE_LCN]);\
	sqlite3_reset(stmts[UPDATE_LCN]);\
AM_MACRO_END
	if (result->tses && result->start_para->dtv_para.standard != AM_SCAN_DTV_STD_ATSC)
	{
		dvbpsi_nit_t *nit;
		AM_SCAN_TS_t *ts;
		dvbpsi_descriptor_t *dr;
		int i, conflict_lcn_start = LCN_CONFLICT_START;
		
		/*找到当前无LCN频道或LCN冲突频道的频道号起始位置*/
		sqlite3_bind_int(stmts[QUERY_MAX_LCN], 1, result->start_para->dtv_para.source);
		if(sqlite3_step(stmts[QUERY_MAX_LCN])==SQLITE_ROW)
		{
			conflict_lcn_start = sqlite3_column_int(stmts[QUERY_MAX_LCN], 0)+1;
			if (conflict_lcn_start < LCN_CONFLICT_START)
				conflict_lcn_start = LCN_CONFLICT_START;
			AM_DEBUG(1, "Have LCN, will set conflict LCN from %d", conflict_lcn_start);
		}
		sqlite3_reset(stmts[QUERY_MAX_LCN]);
		
		for(i=0; i<srv_tab->srv_cnt; i++)
		{
			int r;
			int srv_id, ts_id, org_net_id;
			int num = -1, visible = 1, hd_num = -1;
			int sd_lcn = -1, hd_lcn = -1;
			int sd_visible = 1, hd_visible = 1;
			AM_Bool_t swapped = AM_FALSE;
			AM_Bool_t got_lcn = AM_FALSE;

			sqlite3_bind_int(stmts[QUERY_SRV_TS_NET_ID], 1, srv_tab->srv_ids[i]);
			r = sqlite3_step(stmts[QUERY_SRV_TS_NET_ID]);
			if(r==SQLITE_ROW)
			{
				srv_id = sqlite3_column_int(stmts[QUERY_SRV_TS_NET_ID], 0);
				ts_id  = sqlite3_column_int(stmts[QUERY_SRV_TS_NET_ID], 1);
				org_net_id = sqlite3_column_int(stmts[QUERY_SRV_TS_NET_ID], 2);

				AM_DEBUG(1, "Looking for lcn of net %x, ts %x, srv %x", org_net_id, ts_id, srv_id);
				if (srv_tab->tses[i] != NULL && 
					am_scan_get_lcn_from_nit(org_net_id, ts_id, srv_id, srv_tab->tses[i]->digital.nits, 
					&sd_lcn, &sd_visible, &hd_lcn, &hd_visible))
				{
					/* find in its own ts */
					AM_DEBUG(1, "Found lcn in its own TS");
					got_lcn = AM_TRUE;
				}
				if (! got_lcn)
				{
					/* find in other tses */
					AM_SI_LIST_BEGIN(result->tses, ts)
						if (ts != srv_tab->tses[i])
						{
							if (am_scan_get_lcn_from_nit(org_net_id, ts_id, srv_id, ts->digital.nits, 
								&sd_lcn, &sd_visible, &hd_lcn, &hd_visible)) 
							{
								AM_DEBUG(1, "Found lcn in other TS");
								break;
							}
						}
					AM_SI_LIST_END()
				}
			}
			sqlite3_reset(stmts[QUERY_SRV_TS_NET_ID]);

			/* Skip Non-TV&Radio services */
			sqlite3_bind_int(stmts[QUERY_SRV_TYPE], 1, srv_tab->srv_ids[i]);
			r = sqlite3_step(stmts[QUERY_SRV_TYPE]);
			if (r == SQLITE_ROW)
			{
				int srv_type = sqlite3_column_int(stmts[QUERY_SRV_TYPE], 0);
				if (srv_type != 1 && srv_type != 2)
				{
					sqlite3_reset(stmts[QUERY_SRV_TYPE]);
					continue;
				}
			}
			sqlite3_reset(stmts[QUERY_SRV_TYPE]);
			
			AM_DEBUG(1, "save: sd_lcn is %d", sd_lcn);
			/* default we use SD */
			num = sd_lcn;
			hd_num = hd_lcn;
			visible = sd_visible;
			
			/* need to swap lcn ? */
			if (sd_lcn != -1 && hd_lcn != -1)
			{
				/* 是否存在一个service的lcn == 本service的hd_lcn? */
				sqlite3_bind_int(stmts[QUERY_SRV_BY_SD_LCN], 1, result->start_para->dtv_para.source);
				sqlite3_bind_int(stmts[QUERY_SRV_BY_SD_LCN], 2, hd_lcn);
				if(sqlite3_step(stmts[QUERY_SRV_BY_SD_LCN])==SQLITE_ROW)
				{
					int cft_db_id = sqlite3_column_int(stmts[QUERY_SRV_BY_SD_LCN], 0);
					int tmp_hd_lcn = sqlite3_column_int(stmts[QUERY_SRV_BY_SD_LCN], 1);
					
					AM_DEBUG(1, "SWAP LCN: from %d -> %d", tmp_hd_lcn, hd_lcn);
					UPDATE_SRV_LCN(tmp_hd_lcn, tmp_hd_lcn, hd_lcn, cft_db_id);
					AM_DEBUG(1, "SWAP LCN: from %d -> %d", hd_lcn, sd_lcn);
					num = hd_lcn; /* Use sd_lcn instead of hd_lcn */
					visible = hd_visible;
					swapped = AM_TRUE;
				}
				sqlite3_reset(stmts[QUERY_SRV_BY_SD_LCN]);
			}
			else if (sd_lcn == -1)
			{
				/* only hd lcn */
				num = hd_lcn;
				visible = hd_visible;
			}
			
			if(!visible){
				sqlite3_bind_int(stmts[UPDATE_CHAN_SKIP], 1, 1);
				sqlite3_bind_int(stmts[UPDATE_CHAN_SKIP], 2, srv_tab->srv_ids[i]);
				sqlite3_step(stmts[UPDATE_CHAN_SKIP]);
				sqlite3_reset(stmts[UPDATE_CHAN_SKIP]);
			}
			
			if (! swapped)
			{
				if (num >= 0)
				{
					/*该频道号是否已存在*/
					sqlite3_bind_int(stmts[QUERY_SRV_BY_LCN], 1, result->start_para->dtv_para.source);
					sqlite3_bind_int(stmts[QUERY_SRV_BY_LCN], 2, num);
					if(sqlite3_step(stmts[QUERY_SRV_BY_LCN])==SQLITE_ROW)
					{
						AM_DEBUG(1, "Find a conflict LCN, set from %d -> %d", num, conflict_lcn_start);
						/*已经存在，将当前service放到900后*/
						num = conflict_lcn_start++;
					}
					sqlite3_reset(stmts[QUERY_SRV_BY_LCN]);
				}
				else
				{
					/*未找到LCN， 将当前service放到900后*/
					num = conflict_lcn_start++;
					AM_DEBUG(1, "No LCN found for this program, automatically set to %d", num);
				}
			}

			UPDATE_SRV_LCN(num, hd_lcn, sd_lcn, srv_tab->srv_ids[i]);
		}
	}
}

/**\brief 按LCN顺序进行默认排序 */
static void am_scan_dtv_default_sort_by_lcn(AM_SCAN_Result_t *result, sqlite3_stmt **stmts, AM_SCAN_RecTab_t *srv_tab)
{
	if (result->tses && result->start_para->dtv_para.standard != AM_SCAN_DTV_STD_ATSC)
	{
		int row = AM_DB_MAX_SRV_CNT_PER_SRC, i, j;
		int r, db_id, rr, srv_type;

		i=1;
		j=1;
		
		/* 重新按default_chan_num 对已有channel排序 */
		sqlite3_bind_int(stmts[QUERY_SRV_BY_LCN_ORDER], 1, result->start_para->dtv_para.source);
		r = sqlite3_step(stmts[QUERY_SRV_BY_LCN_ORDER]);
		while (r == SQLITE_ROW)
		{
			db_id = sqlite3_column_int(stmts[QUERY_SRV_BY_LCN_ORDER], 0);
			srv_type = sqlite3_column_int(stmts[QUERY_SRV_BY_LCN_ORDER], 1);
			if (srv_type == 0x1)
			{
				/*电视节目*/
				sqlite3_bind_int(stmts[UPDATE_DEFAULT_CHAN_NUM], 1, i++);
				sqlite3_bind_int(stmts[UPDATE_DEFAULT_CHAN_NUM], 2, db_id);
				sqlite3_step(stmts[UPDATE_DEFAULT_CHAN_NUM]);
				sqlite3_reset(stmts[UPDATE_DEFAULT_CHAN_NUM]);
			}
			else if (srv_type == 0x2)
			{
				/*广播节目*/
				sqlite3_bind_int(stmts[UPDATE_DEFAULT_CHAN_NUM], 1, j++);
				sqlite3_bind_int(stmts[UPDATE_DEFAULT_CHAN_NUM], 2, db_id);
				sqlite3_step(stmts[UPDATE_DEFAULT_CHAN_NUM]);
				sqlite3_reset(stmts[UPDATE_DEFAULT_CHAN_NUM]);
			}

			r = sqlite3_step(stmts[QUERY_SRV_BY_LCN_ORDER]);
		}
		sqlite3_reset(stmts[QUERY_SRV_BY_LCN_ORDER]);
	}
}

/**\brief 按搜索顺序进行默认排序 */
static void am_scan_dtv_default_sort_by_scan_order(AM_SCAN_Result_t *result, sqlite3_stmt **stmts, AM_SCAN_RecTab_t *srv_tab)
{
	if (result->tses && result->start_para->dtv_para.standard != AM_SCAN_DTV_STD_ATSC)
	{
		int row = AM_DB_MAX_SRV_CNT_PER_SRC, i, j;
		int r, db_id, rr, srv_type;

		i=1;
		j=1;
		
		/* 重新按default_chan_num 对已有channel排序 */
		sqlite3_bind_int(stmts[QUERY_SRV_BY_DEF_CHAN_NUM_ORDER], 1, result->start_para->dtv_para.source);
		r = sqlite3_step(stmts[QUERY_SRV_BY_DEF_CHAN_NUM_ORDER]);
		while (r == SQLITE_ROW)
		{
			db_id = sqlite3_column_int(stmts[QUERY_SRV_BY_DEF_CHAN_NUM_ORDER], 0);
			if (! am_scan_rec_tab_have_src(srv_tab, db_id))
			{
				srv_type = sqlite3_column_int(stmts[QUERY_SRV_BY_DEF_CHAN_NUM_ORDER], 1);
				if (srv_type == 0x1)
				{
					/*电视节目*/
					sqlite3_bind_int(stmts[UPDATE_DEFAULT_CHAN_NUM], 1, i++);
					sqlite3_bind_int(stmts[UPDATE_DEFAULT_CHAN_NUM], 2, db_id);
					sqlite3_step(stmts[UPDATE_DEFAULT_CHAN_NUM]);
					sqlite3_reset(stmts[UPDATE_DEFAULT_CHAN_NUM]);
				}
				else if (srv_type == 0x2)
				{
					/*广播节目*/
					sqlite3_bind_int(stmts[UPDATE_DEFAULT_CHAN_NUM], 1, j++);
					sqlite3_bind_int(stmts[UPDATE_DEFAULT_CHAN_NUM], 2, db_id);
					sqlite3_step(stmts[UPDATE_DEFAULT_CHAN_NUM]);
					sqlite3_reset(stmts[UPDATE_DEFAULT_CHAN_NUM]);
				}
			}
			r = sqlite3_step(stmts[QUERY_SRV_BY_DEF_CHAN_NUM_ORDER]);
		}
		sqlite3_reset(stmts[QUERY_SRV_BY_DEF_CHAN_NUM_ORDER]);
		
		AM_DEBUG(1, "Insert channel default channel num from TV:%d, Radio:%d", i, j);
		/* 将本次搜索到的节目排到最后 */
		for (r=0; r<srv_tab->srv_cnt; r++)
		{
			sqlite3_bind_int(stmts[QUERY_SRV_TYPE], 1, srv_tab->srv_ids[r]);
			rr = sqlite3_step(stmts[QUERY_SRV_TYPE]);
			if (rr == SQLITE_ROW)
			{
				srv_type = sqlite3_column_int(stmts[QUERY_SRV_TYPE], 0);
				if (srv_type == 1)
				{
					/*电视节目*/
					sqlite3_bind_int(stmts[UPDATE_DEFAULT_CHAN_NUM], 1, i++);
					sqlite3_bind_int(stmts[UPDATE_DEFAULT_CHAN_NUM], 2, srv_tab->srv_ids[r]);
					sqlite3_step(stmts[UPDATE_DEFAULT_CHAN_NUM]);
					sqlite3_reset(stmts[UPDATE_DEFAULT_CHAN_NUM]);
				}
				else if (srv_type == 2)
				{
					/*广播节目*/
					sqlite3_bind_int(stmts[UPDATE_DEFAULT_CHAN_NUM], 1, j++);
					sqlite3_bind_int(stmts[UPDATE_DEFAULT_CHAN_NUM], 2, srv_tab->srv_ids[r]);
					sqlite3_step(stmts[UPDATE_DEFAULT_CHAN_NUM]);
					sqlite3_reset(stmts[UPDATE_DEFAULT_CHAN_NUM]);
				}
			}
			sqlite3_reset(stmts[QUERY_SRV_TYPE]);
		}
	}
}

/**\brief 按频点大小和service_id大小排序*/
static void am_scan_dtv_default_sort_by_service_id(AM_SCAN_Result_t *result, sqlite3_stmt **stmts, AM_SCAN_RecTab_t *srv_tab)
{
	if (result->tses && result->start_para->dtv_para.standard != AM_SCAN_DTV_STD_ATSC)
	{
		int row = AM_DB_MAX_SRV_CNT_PER_SRC, i, j, max_num;
		int r, db_ts_id, db_id, rr, srv_type;

		i=1;
		j=1;

		if(!result->start_para->dtv_para.resort_all)
		{
#ifndef SORT_TOGETHER
			sqlite3_bind_int(stmts[QUERY_MAX_DEFAULT_CHAN_NUM_BY_TYPE], 1, 1);
			sqlite3_bind_int(stmts[QUERY_MAX_DEFAULT_CHAN_NUM_BY_TYPE], 2, result->start_para->dtv_para.source);
			r = sqlite3_step(stmts[QUERY_MAX_DEFAULT_CHAN_NUM_BY_TYPE]);
			if(r==SQLITE_ROW)
			{
				i = sqlite3_column_int(stmts[QUERY_MAX_DEFAULT_CHAN_NUM_BY_TYPE], 0)+1;
			}
			sqlite3_reset(stmts[QUERY_MAX_DEFAULT_CHAN_NUM_BY_TYPE]);

			sqlite3_bind_int(stmts[QUERY_MAX_DEFAULT_CHAN_NUM_BY_TYPE], 1, 2);
			sqlite3_bind_int(stmts[QUERY_MAX_DEFAULT_CHAN_NUM_BY_TYPE], 2, result->start_para->dtv_para.source);
			r = sqlite3_step(stmts[QUERY_MAX_DEFAULT_CHAN_NUM_BY_TYPE]);
			if(r==SQLITE_ROW)
			{
				j = sqlite3_column_int(stmts[QUERY_MAX_DEFAULT_CHAN_NUM_BY_TYPE], 0)+1;
			}
			sqlite3_reset(stmts[QUERY_MAX_DEFAULT_CHAN_NUM_BY_TYPE]);
#else
			sqlite3_bind_int(stmts[QUERY_MAX_DEFAULT_CHAN_NUM], 1, result->start_para->dtv_para.source);
			r = sqlite3_step(stmts[QUERY_MAX_DEFAULT_CHAN_NUM]);
			if(r==SQLITE_ROW)
			{
				i = j = sqlite3_column_int(stmts[QUERY_MAX_DEFAULT_CHAN_NUM], 0)+1;
			}
			sqlite3_reset(stmts[QUERY_MAX_DEFAULT_CHAN_NUM]);

#endif
			i = (i<=0) ? 1 : i;
			j = (j<=0) ? 1 : j;
		}

		/*重新对srv_table排序以生成新的频道号*/
		/*首先按频点排序*/
		sqlite3_bind_int(stmts[QUERY_TS_BY_FREQ_ORDER], 1, result->start_para->dtv_para.source);
		r = sqlite3_step(stmts[QUERY_TS_BY_FREQ_ORDER]);

		while (r == SQLITE_ROW)
		{
			/*同频点下按service_id排序*/
			db_ts_id = sqlite3_column_int(stmts[QUERY_TS_BY_FREQ_ORDER], 0);
			sqlite3_bind_int(stmts[QUERY_SRV_BY_TYPE], 1, db_ts_id);
			rr = sqlite3_step(stmts[QUERY_SRV_BY_TYPE]);
			while (rr == SQLITE_ROW)
			{
				db_id = sqlite3_column_int(stmts[QUERY_SRV_BY_TYPE], 0);

				if(result->start_para->dtv_para.resort_all || am_scan_rec_tab_have_src(srv_tab, db_id))
				{
					srv_type = sqlite3_column_int(stmts[QUERY_SRV_BY_TYPE], 1);
					if (srv_type == 1)
					{
						/*电视节目*/
						sqlite3_bind_int(stmts[UPDATE_DEFAULT_CHAN_NUM], 1, i);
						sqlite3_bind_int(stmts[UPDATE_DEFAULT_CHAN_NUM], 2, db_id);
						sqlite3_step(stmts[UPDATE_DEFAULT_CHAN_NUM]);
						sqlite3_reset(stmts[UPDATE_DEFAULT_CHAN_NUM]);
						i++;
					}
#ifndef SORT_TOGETHER 
					else if (srv_type == 2)
					{
						/*广播节目*/
						sqlite3_bind_int(stmts[UPDATE_DEFAULT_CHAN_NUM], 1, j);
						sqlite3_bind_int(stmts[UPDATE_DEFAULT_CHAN_NUM], 2, db_id);
						sqlite3_step(stmts[UPDATE_DEFAULT_CHAN_NUM]);
						sqlite3_reset(stmts[UPDATE_DEFAULT_CHAN_NUM]);
						j++;
					}
				}

				rr = sqlite3_step(stmts[QUERY_SRV_BY_TYPE]);
			}
			sqlite3_reset(stmts[QUERY_SRV_BY_TYPE]);
#else
				}
				rr = sqlite3_step(stmts[QUERY_SRV_BY_TYPE]);
			}
			sqlite3_reset(stmts[QUERY_SRV_BY_TYPE]);
			r = sqlite3_step(stmts[QUERY_TS_BY_FREQ_ORDER]);
		}
		sqlite3_reset(stmts[QUERY_TS_BY_FREQ_ORDER]);
		sqlite3_bind_int(stmts[QUERY_TS_BY_FREQ_ORDER], 1, result->start_para->dtv_para.source);
		r = sqlite3_step(stmts[QUERY_TS_BY_FREQ_ORDER]);
		while (r == SQLITE_ROW)
		{
			/*广播节目放到最后*/
			db_ts_id = sqlite3_column_int(stmts[QUERY_TS_BY_FREQ_ORDER], 0);
			sqlite3_bind_int(stmts[QUERY_SRV_BY_TYPE], 1, db_ts_id);
			rr = sqlite3_step(stmts[QUERY_SRV_BY_TYPE]);
			while (rr == SQLITE_ROW)
			{
				db_id = sqlite3_column_int(stmts[QUERY_SRV_BY_TYPE], 0);
				if(result->start_para->dtv_para.resort_all || am_scan_rec_tab_have_src(srv_tab, db_id))
				{
					srv_type = sqlite3_column_int(stmts[QUERY_SRV_BY_TYPE], 1);
					if (srv_type == 2)
					{
						sqlite3_bind_int(stmts[UPDATE_DEFAULT_CHAN_NUM], 1, i);
						sqlite3_bind_int(stmts[UPDATE_DEFAULT_CHAN_NUM], 2, db_id);
						sqlite3_step(stmts[UPDATE_DEFAULT_CHAN_NUM]);
						sqlite3_reset(stmts[UPDATE_DEFAULT_CHAN_NUM]);
						i++;
					}
				}
				rr = sqlite3_step(stmts[QUERY_SRV_BY_TYPE]);
			}
			sqlite3_reset(stmts[QUERY_SRV_BY_TYPE]);
#endif
			r = sqlite3_step(stmts[QUERY_TS_BY_FREQ_ORDER]);
		}
		sqlite3_reset(stmts[QUERY_TS_BY_FREQ_ORDER]);
	}
}

/**\brief Sort the ATV Program by freqency order */
static void am_scan_atv_default_sort_by_freq(AM_SCAN_Result_t *result, sqlite3_stmt **stmts)
{
	int row = AM_DB_MAX_SRV_CNT_PER_SRC, i;
	int r, db_ts_id, db_id, rr, srv_type;

	i=1;

	sqlite3_bind_int(stmts[QUERY_TS_BY_FREQ_ORDER], 1, FE_ANALOG);
	r = sqlite3_step(stmts[QUERY_TS_BY_FREQ_ORDER]);

	while (r == SQLITE_ROW)
	{
		db_ts_id = sqlite3_column_int(stmts[QUERY_TS_BY_FREQ_ORDER], 0);
		sqlite3_bind_int(stmts[QUERY_SRV_BY_TYPE], 1, db_ts_id);
		rr = sqlite3_step(stmts[QUERY_SRV_BY_TYPE]);
		while (rr == SQLITE_ROW)
		{
			db_id = sqlite3_column_int(stmts[QUERY_SRV_BY_TYPE], 0);
			srv_type = sqlite3_column_int(stmts[QUERY_SRV_BY_TYPE], 1);
			if (srv_type == AM_SCAN_SRV_ATV)
			{
				sqlite3_bind_int(stmts[UPDATE_DEFAULT_CHAN_NUM], 1, i);
				sqlite3_bind_int(stmts[UPDATE_DEFAULT_CHAN_NUM], 2, db_id);
				sqlite3_step(stmts[UPDATE_DEFAULT_CHAN_NUM]);
				sqlite3_reset(stmts[UPDATE_DEFAULT_CHAN_NUM]);
				i++;
			}
			rr = sqlite3_step(stmts[QUERY_SRV_BY_TYPE]);
		}
		sqlite3_reset(stmts[QUERY_SRV_BY_TYPE]);
		r = sqlite3_step(stmts[QUERY_TS_BY_FREQ_ORDER]);
	}
	sqlite3_reset(stmts[QUERY_TS_BY_FREQ_ORDER]);
} 

/**\brief 默认搜索完毕存储函数*/
static void am_scan_default_store(AM_SCAN_Result_t *result)
{
	AM_SCAN_TS_t *ts;
	char sqlstr[128];
	sqlite3_stmt	*stmts[MAX_STMT];
	int i, ret;
	AM_SCAN_RecTab_t srv_tab;
	AM_Bool_t sorted = 0, has_atv = AM_FALSE, has_dtv = AM_FALSE;
	sqlite3 *hdb;
	
	assert(result);

	AM_DB_HANDLE_PREPARE(hdb);
	am_scan_rec_tab_init(&srv_tab);

	/*Prepare sqlite3 stmts*/
	memset(stmts, 0, sizeof(sqlite3_stmt*) * MAX_STMT);
	for (i=0; i<MAX_STMT; i++)
	{
		ret = sqlite3_prepare(hdb, sql_stmts[i], -1, &stmts[i], NULL);
		if (ret != SQLITE_OK)
		{
			AM_DEBUG(1, "Prepare sqlite3 failed, stmts[%d] ret = %x", i, ret);
			goto store_end;
		}
	}
	
	AM_DEBUG(1, "Storing tses ...");
	/*依次存储每个TS*/
	AM_SI_LIST_BEGIN(result->tses, ts)
		if (ts->type == AM_SCAN_TS_ANALOG)
		{
			if (! has_atv) 
			{
				if (result->start_para->atv_para.mode == AM_SCAN_ATVMODE_AUTO)
					am_scan_clear_source(hdb, FE_ANALOG);
				has_atv = AM_TRUE;
			}
			store_analog_ts(stmts, result, ts);
		}
		else
		{
			if (! has_dtv)
			{
				if (result->start_para->dtv_para.source == FE_QPSK)
				{
					int db_sat_id;
					/* 删除同一卫星配置下的所有ts & srv */
					db_sat_id = get_sat_para_record(stmts, &result->start_para->dtv_para.sat_para);
					if (db_sat_id != -1)
					{
						AM_DEBUG(1, "Delete tses & srvs for db_sat_para_id=%d", db_sat_id);
						snprintf(sqlstr, sizeof(sqlstr), "delete from ts_table where db_sat_para_id=%d",db_sat_id);
						sqlite3_exec(hdb, sqlstr, NULL, NULL, NULL);
						snprintf(sqlstr, sizeof(sqlstr), "delete from srv_table where db_sat_para_id=%d",db_sat_id);
						sqlite3_exec(hdb, sqlstr, NULL, NULL, NULL);
						AM_DEBUG(1, "Remove this sat para record...");
						snprintf(sqlstr, sizeof(sqlstr), "delete from sat_para_table where db_id=%d",db_sat_id);
						sqlite3_exec(hdb, sqlstr, NULL, NULL, NULL);
					}
				}
				else if (GET_MODE(result->start_para->dtv_para.mode) != AM_SCAN_DTVMODE_MANUAL)
				{
					/*自动搜索和全频段搜索时删除该源下的所有信息*/
					am_scan_clear_source(hdb, result->start_para->dtv_para.source);
				}
				
				has_dtv = AM_TRUE;
			}
			if (result->start_para->dtv_para.standard != AM_SCAN_DTV_STD_ATSC)
				store_dvb_ts(stmts, result, ts, &srv_tab);
			else
				store_atsc_ts(stmts, result, ts);
		}
	AM_SI_LIST_END()

	/* Sort the ATV Programs */
	if (has_atv)
	{
		am_scan_atv_default_sort_by_freq(result, stmts);
	}
	/* Sort the DTV Programs */
	if (has_dtv)
	{
		/* 生成lcn排序结果 */
		am_scan_lcn_proc(result, stmts, &srv_tab);
		
		if (result->start_para->dtv_para.sort_method == AM_SCAN_SORT_BY_FREQ_SRV_ID)
		{
			/* 生成按service_id大小顺序排序结果 */
			am_scan_dtv_default_sort_by_service_id(result, stmts, &srv_tab);
		}
		else if (result->start_para->dtv_para.sort_method == AM_SCAN_SORT_BY_LCN)
		{
			am_scan_dtv_default_sort_by_lcn(result, stmts, &srv_tab);
		}
		else
		{
			/* 生成按搜索先后顺序排序结果 */
			am_scan_dtv_default_sort_by_scan_order(result, stmts, &srv_tab);
		}
	}
	
	/* 生成最终 chan_num */
	AM_DEBUG(1, "Updating chan_num & chan_order from default_chan_num ...");
	sqlite3_exec(hdb, "update srv_table set chan_num=default_chan_num, \
		chan_order=default_chan_num where default_chan_num>0", NULL, NULL, NULL);
	
store_end:
	for (i=0; i<MAX_STMT; i++)
	{
		if (stmts[i] != NULL)
			sqlite3_finalize(stmts[i]);
	}

	am_scan_rec_tab_release(&srv_tab);
}

/**\brief 清空一个表控制标志*/
static void am_scan_tablectl_clear(AM_SCAN_TableCtl_t * scl)
{
	scl->data_arrive_time = 0;
	
	if (scl->subs && scl->subctl)
	{
		int i;

		memset(scl->subctl, 0, sizeof(AM_SCAN_SubCtl_t) * scl->subs);
		for (i=0; i<scl->subs; i++)
		{
			scl->subctl[i].ver = 0xff;
		}
	}
}

/**\brief 初始化一个表控制结构*/
static AM_ErrorCode_t am_scan_tablectl_init(AM_SCAN_TableCtl_t * scl, int recv_flag, int evt_flag,
											int timeout, uint16_t pid, uint8_t tid, const char *name,
											uint16_t sub_cnt, void (*done)(struct AM_SCAN_Scanner_s *),
											int distance)
{
	memset(scl, 0, sizeof(AM_SCAN_TableCtl_t));\
	scl->fid = -1;
	scl->recv_flag = recv_flag;
	scl->evt_flag = evt_flag;
	scl->timeout = timeout;
	scl->pid = pid;
	scl->tid = tid;
	scl->done = done;
	scl->repeat_distance = distance;
	strcpy(scl->tname, name);

	scl->subs = sub_cnt;
	if (scl->subs)
	{
		scl->subctl = (AM_SCAN_SubCtl_t*)malloc(sizeof(AM_SCAN_SubCtl_t) * scl->subs);
		if (!scl->subctl)
		{
			scl->subs = 0;
			AM_DEBUG(1, "Cannot init tablectl, no enough memory");
			return AM_SCAN_ERR_NO_MEM;
		}

		am_scan_tablectl_clear(scl);
	}

	return AM_SUCCESS;
}

/**\brief 反初始化一个表控制结构*/
static void am_scan_tablectl_deinit(AM_SCAN_TableCtl_t * scl)
{
	if (scl->subctl)
	{
		free(scl->subctl);
		scl->subctl = NULL;
	}
}

/**\brief 判断一个表的所有section是否收齐*/
static AM_Bool_t am_scan_tablectl_test_complete(AM_SCAN_TableCtl_t * scl)
{
	static uint8_t test_array[32] = {0};
	int i;

	for (i=0; i<scl->subs; i++)
	{
		if ((scl->subctl[i].ver != 0xff) &&
			memcmp(scl->subctl[i].mask, test_array, sizeof(test_array)))
			return AM_FALSE;
	}

	return AM_TRUE;
}

/**\brief 判断一个表的指定section是否已经接收*/
static AM_Bool_t am_scan_tablectl_test_recved(AM_SCAN_TableCtl_t * scl, AM_SI_SectionHeader_t *header)
{
	int i;
	
	if (!scl->subctl)
		return AM_TRUE;

	for (i=0; i<scl->subs; i++)
	{
		if ((scl->subctl[i].ext == header->extension) && 
			(scl->subctl[i].ver == header->version) && 
			(scl->subctl[i].last == header->last_sec_num) && 
			!BIT_TEST(scl->subctl[i].mask, header->sec_num))
			return AM_TRUE;
	}
	
	return AM_FALSE;
}

/**\brief 在一个表中增加一个section已接收标识*/
static AM_ErrorCode_t am_scan_tablectl_mark_section(AM_SCAN_TableCtl_t * scl, AM_SI_SectionHeader_t *header)
{
	int i;
	AM_SCAN_SubCtl_t *sub, *fsub;

	if (!scl->subctl)
		return AM_SUCCESS;

	sub = fsub = NULL;
	for (i=0; i<scl->subs; i++)
	{
		if (scl->subctl[i].ext == header->extension)
		{
			sub = &scl->subctl[i];
			break;
		}
		/*记录一个空闲的结构*/
		if ((scl->subctl[i].ver == 0xff) && !fsub)
			fsub = &scl->subctl[i];
	}
	
	if (!sub && !fsub)
	{
		AM_DEBUG(1, "No more subctl for adding new %s subtable", scl->tname);
		return AM_FAILURE;
	}
	if (!sub)
		sub = fsub;
	
	/*发现新版本，重新设置接收控制*/
	if (sub->ver != 0xff && (sub->ver != header->version ||\
		sub->ext != header->extension || sub->last != header->last_sec_num))
		SUBCTL_CLEAR(sub);

	if (sub->ver == 0xff)
	{
		int i;
		
		/*接收到的第一个section*/
		sub->last = header->last_sec_num;
		sub->ver = header->version;	
		sub->ext = header->extension;
		
		for (i=0; i<(sub->last+1); i++)
			BIT_SET(sub->mask, i);
	}

	/*设置已接收标识*/
	BIT_CLEAR(sub->mask, header->sec_num);

	if (scl->data_arrive_time == 0)
		AM_TIME_GetClock(&scl->data_arrive_time);

	return AM_SUCCESS;
}

/**\brief 释放过滤器，并保证在此之后不会再有无效数据*/
static void am_scan_free_filter(AM_SCAN_Scanner_t *scanner, int *fid)
{
	if (*fid == -1)
		return;
		
	AM_DMX_FreeFilter(dtv_start_para.dmx_dev_id, *fid);
	*fid = -1;
	pthread_mutex_unlock(&scanner->lock);
	/*等待无效数据处理完毕*/
	AM_DMX_Sync(dtv_start_para.dmx_dev_id);
	pthread_mutex_lock(&scanner->lock);
}

/**\brief NIT搜索完毕(包括超时)处理*/
static void am_scan_nit_done(AM_SCAN_Scanner_t *scanner)
{
	dvbpsi_nit_t *nit;
	dvbpsi_nit_ts_t *ts;
	dvbpsi_descriptor_t *descr;
	struct dvb_frontend_parameters *param;
	AM_SCAN_FrontEndPara_t *old_para;
	int old_para_cnt;
	int i;

	am_scan_free_filter(scanner, &scanner->dtvctl.nitctl.fid);
	/*清除事件标志*/
	//scanner->evt_flag &= ~scanner->dtvctl.nitctl.evt_flag;
	
	if (scanner->stage == AM_SCAN_STAGE_TS)
	{
		/*清除搜索标识*/
		scanner->recv_status &= ~scanner->dtvctl.nitctl.recv_flag;
		AM_DEBUG(1, "NIT Done in TS stage!");
		return;
	}
	
	if (! scanner->result.nits)
	{
		AM_DEBUG(1, "No NIT found ,try next frequency");
		am_scan_try_nit(scanner);
		return;
	}

	/*清除搜索标识*/
	scanner->recv_status &= ~scanner->dtvctl.nitctl.recv_flag;
	
	old_para = scanner->start_freqs;
	old_para_cnt = scanner->start_freqs_cnt;
	if (scanner->start_freqs)
		free(scanner->start_freqs);
	scanner->start_freqs = NULL;
	scanner->start_freqs_cnt = 0;
	scanner->curr_freq = -1;
	
	/*首先统计NIT中描述的TS个数*/
	AM_SI_LIST_BEGIN(scanner->result.nits, nit)
		AM_SI_LIST_BEGIN(nit->p_first_ts, ts)
		scanner->start_freqs_cnt++;
		AM_SI_LIST_END()
	AM_SI_LIST_END()
	
	if (scanner->start_freqs_cnt == 0)
	{
		AM_DEBUG(1, "No TS in NIT");
		goto NIT_END;
	}
	scanner->start_freqs_cnt = AM_MAX(scanner->start_freqs_cnt, old_para_cnt);
	scanner->start_freqs = (AM_SCAN_FrontEndPara_t*)\
							malloc(sizeof(AM_SCAN_FrontEndPara_t) * scanner->start_freqs_cnt);
	if (!scanner->start_freqs)
	{
		AM_DEBUG(1, "No enough memory for building ts list");
		scanner->start_freqs_cnt = 0;
		goto NIT_END;
	}
	memset(scanner->start_freqs, 0, sizeof(AM_SCAN_FrontEndPara_t) * scanner->start_freqs_cnt);
	/*包含模拟频点*/
	for (i=0; i<old_para_cnt; i++)
	{
		if (old_para[i].atv_freq > 0)
			scanner->start_freqs[i].atv_freq = old_para[i].atv_freq;
	}
	
	scanner->start_freqs_cnt = 0;
	
	/*从NIT搜索结果中取出频点列表存到start_freqs中*/
	AM_SI_LIST_BEGIN(scanner->result.nits, nit)
		/*遍历每个TS*/
		AM_SI_LIST_BEGIN(nit->p_first_ts, ts)
			AM_SI_LIST_BEGIN(ts->p_first_descriptor, descr)
			/*取DVBC频点信息*/
			if (descr->p_decoded && descr->i_tag == AM_SI_DESCR_CABLE_DELIVERY)
			{
				dvbpsi_cable_delivery_dr_t *pcd = (dvbpsi_cable_delivery_dr_t*)descr->p_decoded;

				param = &scanner->start_freqs[scanner->start_freqs_cnt].dtv_para.cable.para;
				param->frequency = pcd->i_frequency/1000;
				param->u.qam.modulation = pcd->i_modulation_type;
				param->u.qam.symbol_rate = pcd->i_symbol_rate;
				scanner->start_freqs_cnt++;
				AM_DEBUG(1, "Add frequency %u, symbol_rate %u, modulation %u, onid %d, ts_id %d", param->frequency,
						param->u.qam.symbol_rate, param->u.qam.modulation, ts->i_orig_network_id, ts->i_ts_id);
				break;
			}
			if (descr->p_decoded && descr->i_tag == AM_SI_DESCR_TERRESTRIAL_DELIVERY)
			{
				dvbpsi_terr_deliv_sys_dr_t *pcd = (dvbpsi_terr_deliv_sys_dr_t*)descr->p_decoded;

				param = &scanner->start_freqs[scanner->start_freqs_cnt].dtv_para.terrestrial.para;
				param->frequency = pcd->i_centre_frequency/1000;
				param->u.ofdm.bandwidth = pcd->i_bandwidth;
				scanner->start_freqs_cnt++;
				AM_DEBUG(1, "Add frequency %u, bw %u, onid %d, ts_id %d", param->frequency,
						param->u.ofdm.bandwidth, ts->i_orig_network_id, ts->i_ts_id);
				break;
			}
			AM_SI_LIST_END()
		AM_SI_LIST_END()
	AM_SI_LIST_END()

NIT_END:
	AM_DEBUG(1, "Total found %d frequencies in NIT", scanner->start_freqs_cnt);
	if (scanner->start_freqs_cnt == 0)
	{
		AM_DEBUG(1, "No Delivery system descriptor in NIT");
	}
	scanner->start_freqs_cnt = AM_MAX(scanner->start_freqs_cnt, old_para_cnt);
	if (scanner->recv_status == AM_SCAN_RECVING_COMPLETE)
	{
		/*开始搜索TS*/
		SET_PROGRESS_EVT(AM_SCAN_PROGRESS_TS_END, NULL);
		SET_PROGRESS_EVT(AM_SCAN_PROGRESS_NIT_END, NULL);
		scanner->stage = AM_SCAN_STAGE_TS;
		am_scan_start_next_ts(scanner);
	}
}

/**\brief NIT搜索完毕(包括超时)处理 目的在于manual与allband模式需要使用NIT的描述子信息*/
static void am_scan_nit_done_nousefreqinfo(AM_SCAN_Scanner_t *scanner)
{
	am_scan_free_filter(scanner, &scanner->dtvctl.nitctl.fid);
	/*清除事件标志*/
	//scanner->evt_flag &= ~scanner->dtvctl.nitctl.evt_flag;
	AM_DEBUG(1, "$$$$$$$$$$$$$$ am_scan_nit_done_nousefreqinfo %p", scanner->result.nits);
	/*清除搜索标识*/
	scanner->recv_status &= ~scanner->dtvctl.nitctl.recv_flag;
}

/**\brief BAT搜索完毕(包括超时)处理*/
static void am_scan_bat_done(AM_SCAN_Scanner_t *scanner)
{
	am_scan_free_filter(scanner, &scanner->dtvctl.batctl.fid);
	/*清除事件标志*/
	//scanner->evt_flag &= ~scanner->dtvctl.batctl.evt_flag;
	/*清除搜索标识*/
	scanner->recv_status &= ~scanner->dtvctl.batctl.recv_flag;
	
	if (scanner->recv_status == AM_SCAN_RECVING_COMPLETE)
	{
		/*开始搜索TS*/
		SET_PROGRESS_EVT(AM_SCAN_PROGRESS_TS_END, NULL);
		SET_PROGRESS_EVT(AM_SCAN_PROGRESS_NIT_END, NULL);
		scanner->stage = AM_SCAN_STAGE_TS;
		am_scan_start_next_ts(scanner);
	}
}

/**\brief PAT搜索完毕(包括超时)处理*/
static void am_scan_pat_done(AM_SCAN_Scanner_t *scanner)
{
	am_scan_free_filter(scanner, &scanner->dtvctl.patctl.fid);
	/*清除事件标志*/
	//scanner->evt_flag &= ~scanner->dtvctl.patctl.evt_flag;
	/*清除搜索标识*/
	scanner->recv_status &= ~scanner->dtvctl.patctl.recv_flag;

	SET_PROGRESS_EVT(AM_SCAN_PROGRESS_PAT_DONE, (void*)scanner->curr_ts->digital.pats);
	
	/* Set the current PAT section */
	scanner->dtvctl.cur_pat = scanner->curr_ts->digital.pats;
	
	/*开始搜索PMT表*/
	am_scan_request_pmts(scanner);
}

/**\brief PMT搜索完毕(包括超时)处理*/
static void am_scan_pmt_done(AM_SCAN_Scanner_t *scanner)
{
	int i;
	
	for (i=0; i<(int)AM_ARRAY_SIZE(scanner->dtvctl.pmtctl); i++)
	{
		if (scanner->dtvctl.pmtctl[i].fid >= 0 && 
			am_scan_tablectl_test_complete(&scanner->dtvctl.pmtctl[i]) &&
			scanner->dtvctl.pmtctl[i].data_arrive_time > 0)
		{
			AM_DEBUG(1, "Stop filter for PMT, program %d", scanner->dtvctl.pmtctl[i].ext);
			am_scan_free_filter(scanner, &scanner->dtvctl.pmtctl[i].fid);
		}
	}
	
	/*清除事件标志*/
	//scanner->evt_flag &= ~scanner->dtvctl.pmtctl.evt_flag;
	
	/*开始搜索尚未接收的PMT表*/
	am_scan_request_pmts(scanner);
}

/**\brief CAT搜索完毕(包括超时)处理*/
static void am_scan_cat_done(AM_SCAN_Scanner_t *scanner)
{
	am_scan_free_filter(scanner, &scanner->dtvctl.catctl.fid);
	/*清除事件标志*/
	//scanner->evt_flag &= ~scanner->dtvctl.catctl.evt_flag;
	/*清除搜索标识*/
	scanner->recv_status &= ~scanner->dtvctl.catctl.recv_flag;

	SET_PROGRESS_EVT(AM_SCAN_PROGRESS_CAT_DONE, (void*)scanner->curr_ts->digital.cats);
}

/**\brief SDT搜索完毕(包括超时)处理*/
static void am_scan_sdt_done(AM_SCAN_Scanner_t *scanner)
{
	am_scan_free_filter(scanner, &scanner->dtvctl.sdtctl.fid);
	/*清除事件标志*/
	//scanner->evt_flag &= ~scanner->dtvctl.sdtctl.evt_flag;
	/*清除搜索标识*/
	scanner->recv_status &= ~scanner->dtvctl.sdtctl.recv_flag;

	SET_PROGRESS_EVT(AM_SCAN_PROGRESS_SDT_DONE, (void*)scanner->curr_ts->digital.sdts);
}

/**\brief MGT搜索完毕(包括超时)处理*/
static void am_scan_mgt_done(AM_SCAN_Scanner_t *scanner)
{
	am_scan_free_filter(scanner, &scanner->dtvctl.mgtctl.fid);
	/*清除事件标志*/
	//scanner->evt_flag &= ~scanner->dtvctl.mgtctl.evt_flag;
	/*清除搜索标识*/
	scanner->recv_status &= ~scanner->dtvctl.mgtctl.recv_flag;

	SET_PROGRESS_EVT(AM_SCAN_PROGRESS_MGT_DONE, (void*)scanner->curr_ts->digital.mgts);
	
	/*开始搜索VCT表*/
	if (scanner->curr_ts->digital.mgts)
	{
		if (! scanner->curr_ts->digital.mgts->is_cable)
			scanner->dtvctl.vctctl.tid = AM_SI_TID_PSIP_TVCT;
		else
			scanner->dtvctl.vctctl.tid = AM_SI_TID_PSIP_CVCT;
		am_scan_request_section(scanner, &scanner->dtvctl.vctctl);
	}
}

/**\brief VCT搜索完毕(包括超时)处理*/
static void am_scan_vct_done(AM_SCAN_Scanner_t *scanner)
{
	am_scan_free_filter(scanner, &scanner->dtvctl.vctctl.fid);
	/*清除事件标志*/
	//scanner->evt_flag &= ~scanner->dtvctl.vctctl.evt_flag;
	/*清除搜索标识*/
	scanner->recv_status &= ~scanner->dtvctl.vctctl.recv_flag;

	if (scanner->dtvctl.vctctl.tid == AM_SI_TID_PSIP_CVCT)
	{
		atsc_descriptor_t *descr;
		cvct_section_info_t *cvct;
		cvct_channel_info_t *chan;
		
		SET_PROGRESS_EVT(AM_SCAN_PROGRESS_CVCT_DONE, (void*)scanner->curr_ts->digital.cvcts);
		
		/*是否需要搜索PAT/PMT*/
		if (scanner->curr_ts->digital.cvcts)
		{
			/*find service location desc first*/
			AM_SI_LIST_BEGIN(scanner->curr_ts->digital.cvcts, cvct)
			AM_SI_LIST_BEGIN(cvct->vct_chan_info, chan);
			if (chan->channel_TSID == cvct->transport_stream_id)
			{
				AM_SI_LIST_BEGIN(chan->desc, descr)			
					if (descr->p_decoded && descr->i_tag == AM_SI_DESCR_SERVICE_LOCATION)
					{
						atsc_service_location_dr_t *asld = (atsc_service_location_dr_t*)descr->p_decoded;
						
						if (asld->i_elem_count > 0)
						{
							AM_DEBUG(1, "Found ServiceLocationDescr in CVCT, will not scan PAT/PMT");
							return;
						}
					}
				}
				AM_SI_LIST_END()
			AM_SI_LIST_END()
			AM_SI_LIST_END()
		}
	}
	else
	{
		atsc_descriptor_t *descr;
		tvct_section_info_t *tvct;
		tvct_channel_info_t *chan;

		SET_PROGRESS_EVT(AM_SCAN_PROGRESS_TVCT_DONE, (void*)scanner->curr_ts->digital.tvcts);
		
		/*是否需要搜索PAT/PMT*/
		if (scanner->curr_ts->digital.tvcts)
		{
			/*find service location desc first*/
			AM_SI_LIST_BEGIN(scanner->curr_ts->digital.tvcts, tvct)
			AM_SI_LIST_BEGIN(tvct->vct_chan_info, chan);
			if (chan->channel_TSID == tvct->transport_stream_id)
			{
				AM_SI_LIST_BEGIN(chan->desc, descr)			
					if (descr->p_decoded && descr->i_tag == AM_SI_DESCR_SERVICE_LOCATION)
					{
						atsc_service_location_dr_t *asld = (atsc_service_location_dr_t*)descr->p_decoded;
						
						if (asld->i_elem_count > 0)
						{
							AM_DEBUG(1, "Found ServiceLocationDescr in TVCT, will not scan PAT/PMT");
							return;
						}
					}
				}
				AM_SI_LIST_END()
			AM_SI_LIST_END()
			AM_SI_LIST_END()
		}
	}
		
	/*VCT中未发现有效service location descr, 尝试搜索PAT/PMT*/
	am_scan_request_section(scanner, &scanner->dtvctl.patctl);
}

/**\brief 根据过滤器号取得相应控制数据*/
static AM_SCAN_TableCtl_t *am_scan_get_section_ctrl_by_fid(AM_SCAN_Scanner_t *scanner, int fid)
{
	AM_SCAN_TableCtl_t *scl = NULL;
	int i;
	
	if (scanner->dtvctl.patctl.fid == fid)
		scl = &scanner->dtvctl.patctl;
	else if (dtv_start_para.standard == AM_SCAN_DTV_STD_ATSC)
	{
		if (scanner->dtvctl.mgtctl.fid == fid)
			scl = &scanner->dtvctl.mgtctl;
		else if (scanner->dtvctl.vctctl.fid == fid)
			scl = &scanner->dtvctl.vctctl;
	}
	else
	{
		if (scanner->dtvctl.catctl.fid == fid)
			scl = &scanner->dtvctl.catctl;
		else if (scanner->dtvctl.sdtctl.fid == fid)
			scl = &scanner->dtvctl.sdtctl;
		else if (scanner->dtvctl.nitctl.fid == fid)
			scl = &scanner->dtvctl.nitctl;
		else if (scanner->dtvctl.batctl.fid == fid)
			scl = &scanner->dtvctl.batctl;
	}
	
	if (scl == NULL)
	{
		for (i=0; i<(int)AM_ARRAY_SIZE(scanner->dtvctl.pmtctl); i++)
		{
			if (scanner->dtvctl.pmtctl[i].fid == fid)
			{
				scl = &scanner->dtvctl.pmtctl[i];
				break;
			}
		}
	}
	
	return scl;
}

/**\brief 从一个TVCT中添加虚拟频道*/
static void am_scan_add_vc_from_tvct(AM_SCAN_Scanner_t *scanner, tvct_section_info_t *vct)
{
	tvct_channel_info_t *tmp, *new;
	AM_Bool_t found;
	
	AM_SI_LIST_BEGIN(vct->vct_chan_info, tmp)
		new = (tvct_channel_info_t *)malloc(sizeof(tvct_channel_info_t));
		if (new == NULL)
		{
			AM_DEBUG(1, "Error, no enough memory for adding a new VC");
			continue;
		}
		/*here we share the desc pointer*/
		*new = *tmp;
		new->p_next = NULL;
		
		found = AM_FALSE;
		/*Is this vc already added?*/
		AM_SI_LIST_BEGIN(scanner->result.tvcs, tmp)
			if (tmp->channel_TSID == new->channel_TSID && 
				tmp->program_number == new->program_number)
			{
				found = AM_TRUE;
				break;
			}
		AM_SI_LIST_END()
		
		if (! found)		
		{
			/*Add this vc to result.vcs*/
			ADD_TO_LIST(new, scanner->result.tvcs);
		}
	AM_SI_LIST_END()
}

/**\brief 从一个CVCT中添加虚拟频道*/
static void am_scan_add_vc_from_cvct(AM_SCAN_Scanner_t *scanner, cvct_section_info_t *vct)
{
	cvct_channel_info_t *tmp, *new;
	AM_Bool_t found;
	
	AM_SI_LIST_BEGIN(vct->vct_chan_info, tmp)
		new = (cvct_channel_info_t *)malloc(sizeof(cvct_channel_info_t));
		if (new == NULL)
		{
			AM_DEBUG(1, "Error, no enough memory for adding a new VC");
			continue;
		}
		/*here we share the desc pointer*/
		*new = *tmp;
		new->p_next = NULL;
		
		found = AM_FALSE;
		/*Is this vc already added?*/
		AM_SI_LIST_BEGIN(scanner->result.cvcs, tmp)
			if (tmp->channel_TSID == new->channel_TSID && 
				tmp->program_number == new->program_number)
			{
				found = AM_TRUE;
				break;
			}
		AM_SI_LIST_END()
		
		if (! found)		
		{
			/*Add this vc to result.vcs*/
			ADD_TO_LIST(new, scanner->result.cvcs);
		}
	AM_SI_LIST_END()
}


/**\brief 数据处理函数*/
static void am_scan_section_handler(int dev_no, int fid, const uint8_t *data, int len, void *user_data)
{
	AM_SCAN_Scanner_t *scanner = (AM_SCAN_Scanner_t*)user_data;
	AM_SCAN_TableCtl_t * sec_ctrl;
	AM_SI_SectionHeader_t header;
	
	if (scanner == NULL)
	{
		AM_DEBUG(1, "Scan: Invalid param user_data in dmx callback");
		return;
	}
	if (data == NULL)
		return;
		
	pthread_mutex_lock(&scanner->lock);
	/*获取接收控制数据*/
	sec_ctrl = am_scan_get_section_ctrl_by_fid(scanner, fid);
	if (sec_ctrl)
	{
		/* the section_syntax_indicator bit must be 1 */
		if ((data[1]&0x80) == 0)
		{
			AM_DEBUG(1, "Scan: section_syntax_indicator is 0, skip this section");
			goto parse_end;
		}
		
		if (AM_SI_GetSectionHeader(scanner->dtvctl.hsi, (uint8_t*)data, len, &header) != AM_SUCCESS)
		{
			AM_DEBUG(1, "Scan: section header error");
			goto parse_end;
		}
		
		/*该section是否已经接收过*/
		if (am_scan_tablectl_test_recved(sec_ctrl, &header))
		{
			AM_DEBUG(1,"%s section %d repeat!", sec_ctrl->tname, header.sec_num);
			/*当有多个子表时，判断收齐的条件为 收到重复section + 所有子表收齐 + 重复section间隔时间大于某个值*/
			if (sec_ctrl->subs > 1)
			{
				int now;
				
				AM_TIME_GetClock(&now);
				if (am_scan_tablectl_test_complete(sec_ctrl) && 
					((now - sec_ctrl->data_arrive_time) > sec_ctrl->repeat_distance))
				{
					AM_DEBUG(1, "%s Done!", sec_ctrl->tname);
					scanner->evt_flag |= sec_ctrl->evt_flag;
					pthread_cond_signal(&scanner->cond);
				}
			}
			
			goto parse_end;
		}
		/*数据处理*/
		switch (header.table_id)
		{
			case AM_SI_TID_PAT:
				AM_DEBUG(1, "PAT section %d/%d arrived", header.sec_num, header.last_sec_num);
				if (scanner->curr_ts)
					COLLECT_SECTION(dvbpsi_pat_t, scanner->curr_ts->digital.pats);
				break;
			case AM_SI_TID_PMT:
				if (scanner->curr_ts)
				{
					AM_DEBUG(1, "PMT %d arrived", header.extension);
					COLLECT_SECTION(dvbpsi_pmt_t, scanner->curr_ts->digital.pmts);
				}
				break;
			case AM_SI_TID_SDT_ACT:
				AM_DEBUG(1, "SDT actual section %d/%d arrived", header.sec_num, header.last_sec_num);
				if (scanner->curr_ts)
					COLLECT_SECTION(dvbpsi_sdt_t, scanner->curr_ts->digital.sdts);
				break;
			case AM_SI_TID_CAT:
				if (scanner->curr_ts)
					COLLECT_SECTION(dvbpsi_cat_t, scanner->curr_ts->digital.cats);
				break;
			case AM_SI_TID_NIT_ACT:
				if (scanner->stage != AM_SCAN_STAGE_TS)
				{
					COLLECT_SECTION(dvbpsi_nit_t, scanner->result.nits);
				}
				else
				{
					COLLECT_SECTION(dvbpsi_nit_t, scanner->curr_ts->digital.nits);
				}
				break;
			case AM_SI_TID_BAT:
				AM_DEBUG(1, "BAT ext 0x%x, sec %d, last %d", header.extension, header.sec_num, header.last_sec_num);
				COLLECT_SECTION(dvbpsi_bat_t, scanner->result.bats);
				break;
			case AM_SI_TID_PSIP_MGT:
				if (scanner->curr_ts)
					COLLECT_SECTION(mgt_section_info_t, scanner->curr_ts->digital.mgts);
				break;
			case AM_SI_TID_PSIP_TVCT:
				if (scanner->curr_ts)
				{
					COLLECT_SECTION(tvct_section_info_t, scanner->curr_ts->digital.tvcts);
					//if (scanner->curr_ts->digital.tvcts != NULL)
					//	am_scan_add_vc_from_tvct(scanner, scanner->curr_ts->digital.tvcts);
				}
				break;
			case AM_SI_TID_PSIP_CVCT:
				if (scanner->curr_ts)
				{
					COLLECT_SECTION(cvct_section_info_t, scanner->curr_ts->digital.cvcts);
					//if (scanner->curr_ts->digital.tvcts != NULL)
					//	am_scan_add_vc_from_cvct(scanner, scanner->curr_ts->digital.cvcts);
				}
				break;
			default:
				AM_DEBUG(1, "Scan: Unkown section data, table_id 0x%x", data[0]);
				goto parse_end;
				break;
		}
			
		/*数据处理完毕，查看该表是否已接收完毕所有section*/
		if (am_scan_tablectl_test_complete(sec_ctrl) && sec_ctrl->subs == 1)
		{
			/*该表接收完毕*/
			AM_DEBUG(1, "%s Done!", sec_ctrl->tname);
			scanner->evt_flag |= sec_ctrl->evt_flag;
			pthread_cond_signal(&scanner->cond);
		}
	}
	else
	{
		AM_DEBUG(1, "Scan: Unknown filter id %d in dmx callback", fid);
	}

parse_end:
	pthread_mutex_unlock(&scanner->lock);

}

/**\brief 请求一个表的section数据*/
static AM_ErrorCode_t am_scan_request_section(AM_SCAN_Scanner_t *scanner, AM_SCAN_TableCtl_t *scl)
{
	struct dmx_sct_filter_params param;
	
	if (scl->fid != -1)
	{
		am_scan_free_filter(scanner, &scl->fid);
	}

	/*分配过滤器*/
	if (AM_DMX_AllocateFilter(dtv_start_para.dmx_dev_id, &scl->fid) != AM_SUCCESS)
		return AM_DMX_ERR_NO_FREE_FILTER;
	/*设置处理函数*/
	AM_TRY(AM_DMX_SetCallback(dtv_start_para.dmx_dev_id, scl->fid, am_scan_section_handler, (void*)scanner));

	/*设置过滤器参数*/
	memset(&param, 0, sizeof(param));
	param.pid = scl->pid;
	param.filter.filter[0] = scl->tid;
	param.filter.mask[0] = 0xff;
	
	/*For PMT, we must filter its extension*/
	if (scl->tid == AM_SI_TID_PMT)
	{
		param.filter.filter[1] = (uint8_t)((scl->ext&0xff00)>>8);
		param.filter.mask[1] = 0xff;
		param.filter.filter[2] = (uint8_t)(scl->ext);
		param.filter.mask[2] = 0xff;
	}

	/*Current next indicator must be 1*/
	param.filter.filter[3] = 0x01;
	param.filter.mask[3] = 0x01;
	
	param.flags = DMX_CHECK_CRC;
	/*设置超时时间*/
	AM_TIME_GetTimeSpecTimeout(scl->timeout, &scl->end_time);

	AM_TRY(AM_DMX_SetSecFilter(dtv_start_para.dmx_dev_id, scl->fid, &param));
	AM_TRY(AM_DMX_SetBufferSize(dtv_start_para.dmx_dev_id, scl->fid, 16*1024));
	AM_TRY(AM_DMX_StartFilter(dtv_start_para.dmx_dev_id, scl->fid));

	/*设置接收状态*/
	scanner->recv_status |= scl->recv_flag; 

	return AM_SUCCESS;
}

static int am_scan_get_recving_pmt_count(AM_SCAN_Scanner_t *scanner)
{
	int i, count = 0;
	
	for (i=0; i<(int)AM_ARRAY_SIZE(scanner->dtvctl.pmtctl); i++)
	{
		if (scanner->dtvctl.pmtctl[i].fid >= 0)
			count++;
	}
	
	AM_DEBUG(1, "%d PMTs are receiving...", count);
	return count;
}

static AM_ErrorCode_t am_scan_request_pmts(AM_SCAN_Scanner_t *scanner)
{
	int i, recv_count;
	AM_ErrorCode_t ret = AM_SUCCESS;
	dvbpsi_pat_program_t *cur_prog = scanner->dtvctl.cur_prog;
	
	if (! scanner->curr_ts)
	{
		/*A serious error*/
		AM_DEBUG(1, "Error, no current ts selected");
		return AM_SCAN_ERROR_BASE;
	}
	if (! scanner->curr_ts->digital.pats)
		return AM_SCAN_ERROR_BASE;
		
	for (i=0; i<(int)AM_ARRAY_SIZE(scanner->dtvctl.pmtctl); i++)
	{
		if (scanner->dtvctl.pmtctl[i].fid < 0)
		{
			if (cur_prog)
			{
				/* Start next */
				cur_prog = cur_prog->p_next;
			}
			if (! cur_prog)
			{
				/* find a valid PAT section */
				while (scanner->dtvctl.cur_pat && ! scanner->dtvctl.cur_pat->p_first_program)
				{
					AM_DEBUG(1,"Scan: no PMT found in this PAT section, try next PAT section.");
					scanner->dtvctl.cur_pat = scanner->dtvctl.cur_pat->p_next;
				}
				/* Start from its first program */
				if (scanner->dtvctl.cur_pat) 
				{
					AM_DEBUG(1, "Start PMT from a new PAT section %p...", scanner->dtvctl.cur_pat);
					cur_prog = scanner->dtvctl.cur_pat->p_first_program;
					scanner->dtvctl.cur_pat = scanner->dtvctl.cur_pat->p_next;
				}
			}
			
			AM_DEBUG(1, "current program %p", cur_prog);
			while (cur_prog && cur_prog->i_number == 0)
			{
				AM_DEBUG(1, "Skip for program number = 0");
				cur_prog = cur_prog->p_next;
			}
			
			recv_count = am_scan_get_recving_pmt_count(scanner);
			
			if (! cur_prog)
			{
				if (recv_count == 0)
				{
					AM_DEBUG(1,"All PMTs Done!");
					
					/*清除搜索标识*/
					scanner->recv_status &= ~scanner->dtvctl.pmtctl[0].recv_flag;
					SET_PROGRESS_EVT(AM_SCAN_PROGRESS_PMT_DONE, (void*)scanner->curr_ts->digital.pmts);
				}
				scanner->dtvctl.cur_prog = NULL;
				return AM_SUCCESS;
			}
			
			AM_DEBUG(1, "Start PMT for program %d, pmt_pid 0x%x", cur_prog->i_number, cur_prog->i_pid);
			am_scan_tablectl_clear(&scanner->dtvctl.pmtctl[i]);
			scanner->dtvctl.pmtctl[i].pid = cur_prog->i_pid;
			scanner->dtvctl.pmtctl[i].ext = cur_prog->i_number;
			
			ret = am_scan_request_section(scanner, &scanner->dtvctl.pmtctl[i]);
			if (ret == AM_DMX_ERR_NO_FREE_FILTER)
			{
				if (recv_count <= 0)
				{
					/* Can this case be true ? */
					AM_DEBUG(1, "No more filter for recving PMT, skip program %d", cur_prog->i_number);
					scanner->dtvctl.cur_prog = cur_prog;
				}
				else
				{
					AM_DEBUG(1, "No more filter for recving PMT, will start when getting a free filter");	
				}
				
				return ret;
			}
			else if (ret != AM_SUCCESS)
			{
				AM_DEBUG(1, "Start PMT for program %d failed", cur_prog->i_number);
			}
			
			scanner->dtvctl.cur_prog = cur_prog;
		}
	}
	
	return ret;
}

/**\brief 从下一个主频点中搜索NIT表*/
static AM_ErrorCode_t am_scan_try_nit(AM_SCAN_Scanner_t *scanner)
{
	AM_ErrorCode_t ret;
	AM_SCAN_TSProgress_t tp;
	
	if (scanner->stage != AM_SCAN_STAGE_NIT)
	{
		scanner->stage = AM_SCAN_STAGE_NIT;
		/*开始搜索NIT*/
		SET_PROGRESS_EVT(AM_SCAN_PROGRESS_NIT_BEGIN, NULL);
	}

	if (scanner->curr_freq >= 0)
		SET_PROGRESS_EVT(AM_SCAN_PROGRESS_TS_END, NULL);
		
	do
	{
		scanner->curr_freq++;
		if (scanner->curr_freq < 0 || scanner->curr_freq >= scanner->start_freqs_cnt || 
			dvb_fend_para(cur_fe_para)->frequency <= 0)
		{
			AM_DEBUG(1, "Cannot get nit after all trings!");
			if (scanner->start_freqs)
			{
				free(scanner->start_freqs);
				scanner->start_freqs = NULL;
				scanner->start_freqs_cnt = 0;
				scanner->curr_freq = -1;
			}
			/*搜索结束*/
			scanner->stage = AM_SCAN_STAGE_DONE;
			SET_PROGRESS_EVT(AM_SCAN_PROGRESS_SCAN_END, (void*)scanner->end_code);
			
			return AM_SCAN_ERR_CANNOT_GET_NIT;
		}

		AM_DEBUG(1, "Tring to recv NIT in frequency %u ...", 
			dvb_fend_para(cur_fe_para)->frequency);

		tp.index = scanner->curr_freq;
		tp.total = scanner->start_freqs_cnt;
		tp.fend_para = cur_fe_para;
		SET_PROGRESS_EVT(AM_SCAN_PROGRESS_TS_BEGIN, (void*)&tp);
		
		ret = AM_FENDCTRL_SetPara(scanner->start_para.fend_dev_id, &cur_fe_para);
		if (ret == AM_SUCCESS)
		{
			scanner->recv_status |= AM_SCAN_RECVING_WAIT_FEND;
		}
		else
		{
			SET_PROGRESS_EVT(AM_SCAN_PROGRESS_TS_END, NULL);
			AM_DEBUG(1, "AM_FENDCTRL_SetPara Failed, try next frequency");
		}
			
	}while(ret != AM_SUCCESS);
	
	return ret;
}

/**\brief 检查一个ATV频率的范围，超出范围后做相应调整*/
void am_scan_atv_check_freq_range(AM_SCAN_Scanner_t *scanner, int check_type, uint32_t *check_freq) 
{
    uint32_t min_freq, max_freq;

    min_freq = (uint32_t)scanner->atvctl.min_freq;
    max_freq = (uint32_t)scanner->atvctl.max_freq;

    if (check_type == AM_SCAN_ATV_FREQ_RANGE_CHECK) {
        if (*check_freq > max_freq) 
        {
            *check_freq = max_freq;
        } 
        else if (*check_freq < min_freq) 
        {
            *check_freq = min_freq;
        }
    } else if (check_type == AM_SCAN_ATV_FREQ_LOOP_CHECK) {
        if (*check_freq > max_freq) 
        {
            *check_freq = min_freq;
        } 
        else if (*check_freq < min_freq) 
        {
            *check_freq = max_freq;
        }
    }
}

/**\brief 所有ATV/DTV搜索均已完成*/
static AM_ErrorCode_t am_scan_all_done(AM_SCAN_Scanner_t *scanner)
{
	AM_DEBUG(1, "All ATV & DTV Done !");
	
	if (scanner->start_freqs)
	{
		free(scanner->start_freqs);
		scanner->start_freqs = NULL;
		scanner->start_freqs_cnt = 0;
		scanner->curr_freq = -1;
	}

	/*搜索结束*/
	scanner->stage = AM_SCAN_STAGE_DONE;
	SET_PROGRESS_EVT(AM_SCAN_PROGRESS_SCAN_END, (void*)scanner->end_code);
	
	return AM_SUCCESS;
}

/**\brief ATV搜索下一个频率*/
static AM_ErrorCode_t am_scan_atv_step_tune(AM_SCAN_Scanner_t *scanner)
{
	AM_ErrorCode_t ret = AM_FAILURE;
	
	if (cur_fe_para.m_type == FE_ANALOG)
	{
		/* step to next */
		do
		{
			/* Auto scan reach the max freq ? */
			if ((int)dvb_fend_para(cur_fe_para)->frequency >= scanner->atvctl.max_freq &&
				atv_start_para.mode == AM_SCAN_ATVMODE_AUTO)
			{
				AM_DEBUG(1, "ATV auto scan reach max freq.");
				return am_scan_start_next_ts(scanner);
			}
			
			/* try next frequency */
			dvb_fend_para(cur_fe_para)->frequency += scanner->atvctl.step * scanner->atvctl.direction;
			/* frequency range check */
			am_scan_atv_check_freq_range(scanner, scanner->atvctl.range_check, &dvb_fend_para(cur_fe_para)->frequency);
			AM_DEBUG(1, "ATV step tune: %s %dHz", scanner->atvctl.direction>0?"+":"-", scanner->atvctl.step);

			SET_PROGRESS_EVT(AM_SCAN_PROGRESS_ATV_TUNING, (void*)(dvb_fend_para(cur_fe_para)->frequency));
		
			/* Set frontend */
			AM_DEBUG(1, "Trying to tune atv frequency %uHz (range:%d ~ %d)...", 
				dvb_fend_para(cur_fe_para)->frequency,
				scanner->atvctl.min_freq, scanner->atvctl.max_freq);
			ret = AM_FENDCTRL_SetPara(scanner->start_para.fend_dev_id, &cur_fe_para);
			if (ret == AM_SUCCESS)
			{
				scanner->recv_status |= AM_SCAN_RECVING_WAIT_FEND;
			}
			else
			{
				AM_DEBUG(1, "Set frontend failed, try next frequency...");
			}
		}while(ret != AM_SUCCESS);
	}
	
	return ret;
}

/**\brief 从频率列表中选择下一个频点开始搜索*/
static AM_ErrorCode_t am_scan_start_next_ts(AM_SCAN_Scanner_t *scanner)
{
	AM_ErrorCode_t ret = AM_FAILURE;
	AM_SCAN_TSProgress_t tp;
	int frequency;

	if (scanner->stage != AM_SCAN_STAGE_TS)
		scanner->stage = AM_SCAN_STAGE_TS;

	if (scanner->curr_freq >= 0)
		SET_PROGRESS_EVT(AM_SCAN_PROGRESS_TS_END, scanner->curr_ts);
	
	scanner->curr_ts = NULL;
	do
	{
		scanner->curr_freq++;
		if (scanner->curr_freq < 0 || scanner->curr_freq >= scanner->start_freqs_cnt)
		{
			AM_DEBUG(1, "All TSes Complete!");
			return am_scan_all_done(scanner);
		}

		AM_DEBUG(1, "curr_freq %d, %d, %d, %d, %d", scanner->curr_freq, 
			scanner->atvctl.start_idx, atv_start_para.fe_cnt,
			scanner->dtvctl.start_idx, dtv_start_para.fe_cnt);
		if (scanner->curr_freq >= (scanner->atvctl.start_idx + atv_start_para.fe_cnt) && 
			! scanner->dtvctl.start)
		{
			return am_scan_start_dtv(scanner);
		}
		else if (scanner->curr_freq >= (scanner->dtvctl.start_idx + dtv_start_para.fe_cnt) && 
			! scanner->atvctl.start)
		{
			return am_scan_start_atv(scanner);
		}
		
		/* now start a new freq*/
		if (atv_start_para.mode == AM_SCAN_ATVMODE_FREQ)
		{
			/* adjust the min, max, start freq */
			scanner->atvctl.min_freq = dvb_fend_para(cur_fe_para)->frequency - ATV_1_5MHZ;
			scanner->atvctl.max_freq = dvb_fend_para(cur_fe_para)->frequency + ATV_1_5MHZ;
			scanner->atvctl.start_freq = scanner->atvctl.min_freq;
			
			/* start from min freq */
			dvb_fend_para(cur_fe_para)->frequency = scanner->atvctl.start_freq;
		}
		tp.index = scanner->curr_freq;
		tp.total = scanner->start_freqs_cnt;
		tp.fend_para = cur_fe_para;
		SET_PROGRESS_EVT(AM_SCAN_PROGRESS_TS_BEGIN, (void*)&tp);
		
		AM_DEBUG(1, "Start scanning %s frequency %u ...", 
			(cur_fe_para.m_type==FE_ANALOG)?"atv":"dtv", 
			dvb_fend_para(cur_fe_para)->frequency);
		ret = AM_FENDCTRL_SetPara(scanner->start_para.fend_dev_id, &cur_fe_para);
		if (ret == AM_SUCCESS)
		{
			scanner->recv_status |= AM_SCAN_RECVING_WAIT_FEND;
		}
		else
		{
			AM_DEBUG(1, "Set frontend failed, try next ts...");
		}
	}while(ret != AM_SUCCESS);

	return ret;
}

static void am_scan_fend_callback(int dev_no, int event_type, void *param, void *user_data)
{
	struct dvb_frontend_event *evt = (struct dvb_frontend_event*)param;
	AM_SCAN_Scanner_t *scanner = (AM_SCAN_Scanner_t*)user_data;

	if (!scanner || !evt || (evt->status == 0))
		return;

	pthread_mutex_lock(&scanner->lock);
	scanner->fe_evt = *evt;
	scanner->evt_flag |= AM_SCAN_EVT_FEND;
	pthread_cond_signal(&scanner->cond);
	pthread_mutex_unlock(&scanner->lock);
}

/**\brief 卫星BlindScan回调*/
static void am_scan_blind_scan_callback(int dev_no, AM_FEND_BlindEvent_t *evt, void *user_data)
{
	AM_SCAN_Scanner_t *scanner = (AM_SCAN_Scanner_t*)user_data;
	
	if (!scanner)
		return;
	
	AM_DEBUG(1, "bs callback wait lock, scanner %p", scanner);
	pthread_mutex_lock(&scanner->lock);
	AM_DEBUG(1, "bs callback get lock");
	if (evt->status == AM_FEND_BLIND_UPDATE)
	{
		int cnt = AM_SCAN_MAX_BS_TP_CNT;
		int i;
		struct dvb_frontend_parameters tps[AM_SCAN_MAX_BS_TP_CNT];
		struct dvb_frontend_parameters *new_tp_start;

		AM_FEND_BlindGetTPInfo(scanner->start_para.fend_dev_id, tps, (unsigned int*)&cnt);

		if (cnt > scanner->dtvctl.bs_ctl.searched_tp_cnt)
		{
			scanner->dtvctl.bs_ctl.progress.new_tp_cnt = cnt - scanner->dtvctl.bs_ctl.searched_tp_cnt;
			scanner->dtvctl.bs_ctl.progress.new_tps = tps + scanner->dtvctl.bs_ctl.searched_tp_cnt;
			new_tp_start = &scanner->dtvctl.bs_ctl.searched_tps[scanner->dtvctl.bs_ctl.searched_tp_cnt];
			
			/* Center freq -> Transponder freq */
			for (i=0; i<scanner->dtvctl.bs_ctl.progress.new_tp_cnt; i++)
			{
				AM_SEC_FreqConvert(scanner->start_para.fend_dev_id, 
					scanner->dtvctl.bs_ctl.progress.new_tps[i].frequency, 
					&scanner->dtvctl.bs_ctl.progress.new_tps[i].frequency);
				new_tp_start[i] = scanner->dtvctl.bs_ctl.progress.new_tps[i];
			}
			SET_PROGRESS_EVT(AM_SCAN_PROGRESS_BLIND_SCAN, (void*)&scanner->dtvctl.bs_ctl.progress);
			
			scanner->dtvctl.bs_ctl.searched_tp_cnt = cnt;
			scanner->dtvctl.bs_ctl.progress.new_tp_cnt = 0;
			scanner->dtvctl.bs_ctl.progress.new_tps = NULL;
		}
		
		scanner->dtvctl.bs_ctl.progress.progress = (scanner->dtvctl.bs_ctl.stage - AM_SCAN_BS_STAGE_HL)*25 + evt->process/4;
		SET_PROGRESS_EVT(AM_SCAN_PROGRESS_BLIND_SCAN, (void*)&scanner->dtvctl.bs_ctl.progress);
	
		if (evt->process >= 100)
		{
			/* Current blind scan stage done */
			scanner->evt_flag |= AM_SCAN_EVT_BLIND_SCAN_DONE;
			pthread_cond_signal(&scanner->cond);
		}
	}
	else if (evt->status == AM_FEND_BLIND_START)
	{
		scanner->dtvctl.bs_ctl.progress.freq = evt->freq;
		AM_SEC_FreqConvert(scanner->start_para.fend_dev_id, 
					scanner->dtvctl.bs_ctl.progress.freq, 
					(unsigned int*)&scanner->dtvctl.bs_ctl.progress.freq);
		AM_DEBUG(1, "bs callback evt: BLIND_START");
		SET_PROGRESS_EVT(AM_SCAN_PROGRESS_BLIND_SCAN, (void*)&scanner->dtvctl.bs_ctl.progress);
		AM_DEBUG(1, "bs callback evt: BLIND_START end");
	}
	
	
	pthread_mutex_unlock(&scanner->lock);
}

/**\brief 启动一次卫星盲扫*/
static AM_ErrorCode_t am_scan_start_blind_scan(AM_SCAN_Scanner_t *scanner)
{
	AM_FEND_Polarisation_t p;
	AM_FEND_Localoscollatorfreq_t l;
	AM_SEC_DVBSatelliteEquipmentControl_t *sec;
	AM_ErrorCode_t ret = AM_FAILURE;
	AM_SEC_DVBSatelliteEquipmentControl_t setting;
	
	AM_DEBUG(1, "stage %d", scanner->dtvctl.bs_ctl.stage);
	if (scanner->dtvctl.bs_ctl.stage > AM_SCAN_BS_STAGE_VH)
	{
		AM_DEBUG(1, "Currently is not in BlindScan mode");
		return AM_FAILURE;
	}
	
	sec = &dtv_start_para.sat_para.sec;
	
	while (scanner->dtvctl.bs_ctl.stage <= AM_SCAN_BS_STAGE_VH && ret != AM_SUCCESS)
	{
		AM_DEBUG(1, "Start blind scan at stage %d", scanner->dtvctl.bs_ctl.stage);
		if (scanner->dtvctl.bs_ctl.stage == AM_SCAN_BS_STAGE_HL ||
			scanner->dtvctl.bs_ctl.stage == AM_SCAN_BS_STAGE_HH)
			p = AM_FEND_POLARISATION_H;
		else
			p = AM_FEND_POLARISATION_V;
		
		if (scanner->dtvctl.bs_ctl.stage == AM_SCAN_BS_STAGE_VL ||
			scanner->dtvctl.bs_ctl.stage == AM_SCAN_BS_STAGE_HL)
			l = AM_FEND_LOCALOSCILLATORFREQ_L;
		else
			l = AM_FEND_LOCALOSCILLATORFREQ_H;

	
		scanner->dtvctl.bs_ctl.progress.polar = p;
		scanner->dtvctl.bs_ctl.progress.lo = l;
		scanner->dtvctl.bs_ctl.searched_tp_cnt = 0;

#if 1
		if (dtv_start_para.sat_para.sec.m_lnbs.m_lof_hi == dtv_start_para.sat_para.sec.m_lnbs.m_lof_lo && 
			l == AM_FEND_LOCALOSCILLATORFREQ_H)
#else
		if (scanner->dtvctl.bs_ctl.stage != AM_SCAN_BS_STAGE_VL)
#endif
		{
			AM_DEBUG(1, "Skip blind scan stage %d", scanner->dtvctl.bs_ctl.stage);
		}
		else
		{
			AM_SEC_GetSetting(scanner->start_para.fend_dev_id, &setting);
			setting.m_lnbs.b_para.polarisation = p;
			setting.m_lnbs.b_para.ocaloscollatorfreq = l;
			AM_SEC_SetSetting(scanner->start_para.fend_dev_id, &setting);
			AM_SEC_PrepareBlindScan(scanner->start_para.fend_dev_id);
			
			AM_DEBUG(1, "start blind scan, scanner %p", scanner);
			/* start blind scan */
			ret = AM_FEND_BlindScan(scanner->start_para.fend_dev_id, am_scan_blind_scan_callback, 
				(void*)scanner, scanner->dtvctl.bs_ctl.start_freq, scanner->dtvctl.bs_ctl.stop_freq);	
		}
	
		if (ret != AM_SUCCESS)
		{
			scanner->dtvctl.bs_ctl.progress.progress += 25;
			SET_PROGRESS_EVT(AM_SCAN_PROGRESS_BLIND_SCAN, (void*)&scanner->dtvctl.bs_ctl.progress);
			scanner->dtvctl.bs_ctl.stage++;
		}
	}
	
	if (ret != AM_SUCCESS && scanner->dtvctl.bs_ctl.stage >= AM_SCAN_BS_STAGE_VH)
	{
		AM_DEBUG(1, "All blind scan done! Now scan for each TP...");
		/* Change the scan mode to All band */
		dtv_start_para.mode = AM_SCAN_DTVMODE_ALLBAND | (dtv_start_para.mode & 0xf1);
		return am_scan_start_next_ts(scanner);
	}
	
	AM_DEBUG(1,"start blind scan return ret %d", ret);
	return ret;
}

/**\brief 启动DTV搜索*/
static AM_ErrorCode_t am_scan_start_dtv(AM_SCAN_Scanner_t *scanner)
{
	int i;
	
	if (scanner->dtvctl.start)
	{
		AM_DEBUG(1, "dtv scan already start");
		return AM_SUCCESS;
	}
	
	AM_DEBUG(1, "@@@ Start dtv scan use standard: %s @@@", (dtv_start_para.standard==AM_SCAN_DTV_STD_DVB)?"DVB" :
		(dtv_start_para.standard==AM_SCAN_DTV_STD_ISDB)?"ISDB" : "ATSC");

	/*创建SI解析器*/
	AM_TRY(AM_SI_Create(&scanner->dtvctl.hsi));

	/*接收控制数据初始化*/								
	am_scan_tablectl_init(&scanner->dtvctl.patctl, AM_SCAN_RECVING_PAT, AM_SCAN_EVT_PAT_DONE, PAT_TIMEOUT, 
						AM_SI_PID_PAT, AM_SI_TID_PAT, "PAT", 1, am_scan_pat_done, 0);
						
	for (i=0; i<(int)AM_ARRAY_SIZE(scanner->dtvctl.pmtctl); i++)
	{
		am_scan_tablectl_init(&scanner->dtvctl.pmtctl[i], AM_SCAN_RECVING_PMT, AM_SCAN_EVT_PMT_DONE, PMT_TIMEOUT, 
						0x1fff, AM_SI_TID_PMT, "PMT", 1, am_scan_pmt_done, 0);
	}
	am_scan_tablectl_init(&scanner->dtvctl.catctl, AM_SCAN_RECVING_CAT, AM_SCAN_EVT_CAT_DONE, CAT_TIMEOUT, 
							AM_SI_PID_CAT, AM_SI_TID_CAT, "CAT", 1, am_scan_cat_done, 0);
	am_scan_tablectl_init(&scanner->dtvctl.mgtctl, AM_SCAN_RECVING_MGT, AM_SCAN_EVT_MGT_DONE, MGT_TIMEOUT, 
					AM_SI_ATSC_BASE_PID, AM_SI_TID_PSIP_MGT, "MGT", 1, am_scan_mgt_done, 0);
	am_scan_tablectl_init(&scanner->dtvctl.vctctl, AM_SCAN_RECVING_VCT, AM_SCAN_EVT_VCT_DONE, VCT_TIMEOUT, 
					AM_SI_ATSC_BASE_PID, AM_SI_TID_PSIP_CVCT, "VCT", 1, am_scan_vct_done, 0);
	am_scan_tablectl_init(&scanner->dtvctl.sdtctl, AM_SCAN_RECVING_SDT, AM_SCAN_EVT_SDT_DONE, SDT_TIMEOUT, 
						AM_SI_PID_SDT, AM_SI_TID_SDT_ACT, "SDT", 1, am_scan_sdt_done, 0);
	am_scan_tablectl_init(&scanner->dtvctl.nitctl, AM_SCAN_RECVING_NIT, AM_SCAN_EVT_NIT_DONE, NIT_TIMEOUT, 
						AM_SI_PID_NIT, AM_SI_TID_NIT_ACT, "NIT", 1, am_scan_nit_done, 0);
	am_scan_tablectl_init(&scanner->dtvctl.batctl, AM_SCAN_RECVING_BAT, AM_SCAN_EVT_BAT_DONE, BAT_TIMEOUT, 
						AM_SI_PID_BAT, AM_SI_TID_BAT, "BAT", MAX_BAT_SUBTABLE_CNT, 
						am_scan_bat_done, BAT_REPEAT_DISTANCE);
						
	scanner->curr_freq = scanner->dtvctl.start_idx - 1;
	scanner->dtvctl.start = 1;
	scanner->end_code = AM_SCAN_RESULT_UNLOCKED;
	
	if (dtv_start_para.source == FE_QPSK)
	{
		AM_DEBUG(1, "Set SEC settings...");
		
		AM_SEC_SetSetting(scanner->start_para.fend_dev_id, &dtv_start_para.sat_para.sec);
	}

	/* 启动搜索 */
	if (dtv_start_para.standard == AM_SCAN_DTV_STD_DVB)
	{
		if (GET_MODE(dtv_start_para.mode) == AM_SCAN_DTVMODE_AUTO)
		{
			/*自动搜索模式时按指定主频点列表开始NIT表请求*/
			return am_scan_try_nit(scanner);
		}
		else if (GET_MODE(dtv_start_para.mode) == AM_SCAN_DTVMODE_SAT_BLIND)
		{
			/* 启动卫星盲扫 */
			scanner->dtvctl.bs_ctl.stage = AM_SCAN_BS_STAGE_HL;
			scanner->dtvctl.bs_ctl.progress.progress = 0;
			scanner->dtvctl.bs_ctl.start_freq = DEFAULT_DVBS_BS_FREQ_START;
			scanner->dtvctl.bs_ctl.stop_freq = DEFAULT_DVBS_BS_FREQ_STOP;
			scanner->start_freqs_cnt = 0;
			
			AM_SEC_PrepareBlindScan(scanner->start_para.fend_dev_id);

			return am_scan_start_blind_scan(scanner);
		}
	}
	
	/*其他模式直接按指定频点开始搜索*/
	return am_scan_start_next_ts(scanner);
}

/**\brief 停止DTV搜索*/
static AM_ErrorCode_t am_scan_stop_dtv(AM_SCAN_Scanner_t *scanner)
{
	if (scanner->dtvctl.start)
	{
		int i;
		AM_SCAN_TS_t *ts, *tnext;
		
		AM_DEBUG(1, "Stopping dtv ...");
		
		/* If in satellite blind scan mode, stop it first */
		if (GET_MODE(dtv_start_para.mode) == AM_SCAN_DTVMODE_SAT_BLIND)
		{
			/* we must unlock, as BlindExit will wait the callback done */
			pthread_mutex_unlock(&scanner->lock);
			/* Stop blind scan */
			AM_FEND_BlindExit(scanner->start_para.fend_dev_id);
			pthread_mutex_lock(&scanner->lock);
		}
		
		/*DMX释放*/
		if (scanner->dtvctl.patctl.fid != -1)
			AM_DMX_FreeFilter(dtv_start_para.dmx_dev_id, scanner->dtvctl.patctl.fid);
		for (i=0; i<(int)AM_ARRAY_SIZE(scanner->dtvctl.pmtctl); i++)
		{
			if (scanner->dtvctl.pmtctl[i].fid != -1)
			AM_DMX_FreeFilter(dtv_start_para.dmx_dev_id, scanner->dtvctl.pmtctl[i].fid);
		}
		if (scanner->dtvctl.catctl.fid != -1)
			AM_DMX_FreeFilter(dtv_start_para.dmx_dev_id, scanner->dtvctl.catctl.fid);
		if (scanner->dtvctl.nitctl.fid != -1)
			AM_DMX_FreeFilter(dtv_start_para.dmx_dev_id, scanner->dtvctl.nitctl.fid);
		if (scanner->dtvctl.sdtctl.fid != -1)
			AM_DMX_FreeFilter(dtv_start_para.dmx_dev_id, scanner->dtvctl.sdtctl.fid);
		if (scanner->dtvctl.batctl.fid != -1)
			AM_DMX_FreeFilter(dtv_start_para.dmx_dev_id, scanner->dtvctl.batctl.fid);
		if (scanner->dtvctl.mgtctl.fid != -1)
			AM_DMX_FreeFilter(dtv_start_para.dmx_dev_id, scanner->dtvctl.mgtctl.fid);
		if (scanner->dtvctl.vctctl.fid != -1)
			AM_DMX_FreeFilter(dtv_start_para.dmx_dev_id, scanner->dtvctl.vctctl.fid);
			
		/*等待回调函数执行完毕*/
		pthread_mutex_unlock(&scanner->lock);
		AM_DEBUG(1, "Waiting for dmx callback...");
		AM_DMX_Sync(dtv_start_para.dmx_dev_id);
		AM_DEBUG(1, "OK, hsi is %d", scanner->dtvctl.hsi);
		pthread_mutex_lock(&scanner->lock);
	
		/*表控制释放*/
		am_scan_tablectl_deinit(&scanner->dtvctl.patctl);
		for (i=0; i<(int)AM_ARRAY_SIZE(scanner->dtvctl.pmtctl); i++)
		{
			am_scan_tablectl_deinit(&scanner->dtvctl.pmtctl[i]);
		}
		am_scan_tablectl_deinit(&scanner->dtvctl.catctl);
		am_scan_tablectl_deinit(&scanner->dtvctl.nitctl);
		am_scan_tablectl_deinit(&scanner->dtvctl.sdtctl);
		am_scan_tablectl_deinit(&scanner->dtvctl.batctl);
		am_scan_tablectl_deinit(&scanner->dtvctl.mgtctl);
		am_scan_tablectl_deinit(&scanner->dtvctl.vctctl);
		
		if (scanner->start_freqs != NULL)
			free(scanner->start_freqs);
		/*释放SI资源*/
		RELEASE_TABLE_FROM_LIST(dvbpsi_nit_t, scanner->result.nits);
		RELEASE_TABLE_FROM_LIST(dvbpsi_bat_t, scanner->result.bats);
		ts = scanner->result.tses;
		while (ts)
		{
			tnext = ts->p_next;
			//AM_DEBUG(1, "hsi is %d, ts->digital.pats is %p", scanner->dtvctl.hsi, ts->digital.pats);
			RELEASE_TABLE_FROM_LIST(dvbpsi_pat_t, ts->digital.pats);
			RELEASE_TABLE_FROM_LIST(dvbpsi_pmt_t, ts->digital.pmts);
			RELEASE_TABLE_FROM_LIST(dvbpsi_cat_t, ts->digital.cats);
			RELEASE_TABLE_FROM_LIST(dvbpsi_sdt_t, ts->digital.sdts);
			free(ts);

			ts = tnext;
		}

		AM_SI_Destroy(scanner->dtvctl.hsi);
		scanner->dtvctl.start = 0;	
		AM_DEBUG(1, "dtv stopped !");
	}
	
	return AM_SUCCESS;
}

/**\brief 启动ATV搜索*/
static AM_ErrorCode_t am_scan_start_atv(AM_SCAN_Scanner_t *scanner)
{
	if (scanner->atvctl.start)
	{
		AM_DEBUG(1, "atv scan already start = %d", scanner->atvctl.start);
		return AM_SUCCESS;
	}
	AM_DEBUG(1, "@@@ Start atv scan use mode: %d @@@", atv_start_para.mode);
	if (atv_start_para.mode == AM_SCAN_ATVMODE_FREQ)
	{
		scanner->atvctl.direction = 1;
		scanner->atvctl.range_check = AM_SCAN_ATV_FREQ_RANGE_CHECK;
		scanner->atvctl.step = 0;
		scanner->atvctl.min_freq = 0;
		scanner->atvctl.max_freq = 0;
		scanner->atvctl.start_freq = dvb_fend_para(scanner->start_freqs[scanner->atvctl.start_idx].dtv_para)->frequency;
		scanner->curr_freq = scanner->atvctl.start_idx - 1;
	}
	else if (atv_start_para.mode == AM_SCAN_ATVMODE_MANUAL)
	{
		scanner->atvctl.direction = (atv_start_para.direction>0) ? 1 : -1;
		scanner->atvctl.range_check = AM_SCAN_ATV_FREQ_LOOP_CHECK;
		scanner->atvctl.step = 0;
		scanner->atvctl.min_freq = dvb_fend_para(scanner->start_freqs[scanner->atvctl.start_idx].dtv_para)->frequency;
		scanner->atvctl.max_freq = dvb_fend_para(scanner->start_freqs[scanner->atvctl.start_idx+1].dtv_para)->frequency;
		scanner->atvctl.start_freq = dvb_fend_para(scanner->start_freqs[scanner->atvctl.start_idx+2].dtv_para)->frequency;
		/* start from start freq */
		scanner->curr_freq = scanner->atvctl.start_idx - 1 + 2; 
	}
	else
	{
		scanner->atvctl.direction = 1;
		scanner->atvctl.range_check = AM_SCAN_ATV_FREQ_RANGE_CHECK;
		scanner->atvctl.step = 0;
		scanner->atvctl.min_freq = (int)dvb_fend_para(scanner->start_freqs[scanner->atvctl.start_idx].dtv_para)->frequency;
		scanner->atvctl.max_freq = (int)dvb_fend_para(scanner->start_freqs[scanner->atvctl.start_idx+1].dtv_para)->frequency;
		scanner->atvctl.start_freq = scanner->atvctl.min_freq;
		dvb_fend_para(scanner->start_freqs[scanner->atvctl.start_idx+2].dtv_para)->frequency = scanner->atvctl.min_freq;
		/* start from start freq */
		scanner->curr_freq = scanner->atvctl.start_idx - 1 + 2; 
	}
	
	scanner->atvctl.start = 1;
	am_scan_open_vdin(scanner);
	am_scan_open_afe(scanner);
	
	return am_scan_start_next_ts(scanner);
}

/**\brief 停止ATV搜索*/
static AM_ErrorCode_t am_scan_stop_atv(AM_SCAN_Scanner_t *scanner)
{
	if (scanner->atvctl.start)
	{
		AM_DEBUG(1, "Stopping atv ...");
		AM_DEBUG(1, "Closing AFE device ...");
		am_scan_close_afe(scanner);
		am_scan_close_vdin(scanner);
		
		scanner->atvctl.start = 0;
		AM_DEBUG(1, "atv stopped !");
	}
	
	return AM_SUCCESS;
}

/**\brief 启动搜索*/
static AM_ErrorCode_t am_scan_start(AM_SCAN_Scanner_t *scanner)
{
	/*注册前端事件*/
	AM_EVT_Subscribe(scanner->start_para.fend_dev_id, AM_FEND_EVT_STATUS_CHANGED, am_scan_fend_callback, (void*)scanner);
	
	if (scanner->start_para.mode == AM_SCAN_MODE_ATV_DTV)
	{
		return am_scan_start_atv(scanner);
	}
	else
	{
		return am_scan_start_dtv(scanner);
	}
}

/**\brief 完成一次卫星盲扫*/
static void am_scan_solve_blind_scan_done_evt(AM_SCAN_Scanner_t *scanner)
{
	AM_ErrorCode_t ret = AM_FAILURE;
	int i;
	
	/*Copy 本次搜索到的新TP到start_freqs*/
	for (i=0; i<scanner->dtvctl.bs_ctl.searched_tp_cnt && scanner->start_freqs_cnt < AM_SCAN_MAX_BS_TP_CNT; i++)
	{
		scanner->start_freqs[scanner->start_freqs_cnt].dtv_para.m_type = dtv_start_para.source;
		scanner->start_freqs[scanner->start_freqs_cnt].dtv_para.sat.polarisation = scanner->dtvctl.bs_ctl.progress.polar;
		*dvb_fend_para(scanner->start_freqs[scanner->start_freqs_cnt].dtv_para) = scanner->dtvctl.bs_ctl.searched_tps[i];
		scanner->start_freqs_cnt++;
	}
	
	/* 退出本次盲扫 */
	AM_FEND_BlindExit(scanner->start_para.fend_dev_id);
	
	scanner->dtvctl.bs_ctl.stage++;
	am_scan_start_blind_scan(scanner);
}

/**\brief 当前频点锁定后的处理*/
static int am_scan_new_ts_locked_proc(AM_SCAN_Scanner_t *scanner)
{
	if (scanner->end_code == AM_SCAN_RESULT_UNLOCKED)
		scanner->end_code = AM_SCAN_RESULT_OK;

	if (scanner->stage == AM_SCAN_STAGE_TS)
	{
		/* For analog ts, we must check the tv decoder */
		if (cur_fe_para.m_type == FE_ANALOG && !am_scan_atv_cvbs_lock(scanner))
		{
			AM_DEBUG(1, "cvbs unlock !");
			scanner->atvctl.step = atv_start_para.cvbs_unlocked_step;
			am_scan_atv_step_tune(scanner);
			return 0;
		}
		
		/*频点锁定，新申请内存以添加该频点信息*/
		scanner->curr_ts = (AM_SCAN_TS_t*)malloc(sizeof(AM_SCAN_TS_t));
		if (scanner->curr_ts == NULL)
		{
			AM_DEBUG(1, "Error, no enough memory for adding a new ts");\
			return -1;
		}
		memset(scanner->curr_ts, 0, sizeof(AM_SCAN_TS_t));
		
		if (cur_fe_para.m_type == FE_ANALOG)
		{
			AM_SCAN_DTVSignalInfo_t si;
			
			AM_DEBUG(1, "cvbs lock !");
			memset(&si, 0, sizeof(si));
			si.frequency = dvb_fend_para(cur_fe_para)->frequency;
			si.locked = AM_TRUE;
			SIGNAL_EVENT(AM_SCAN_EVT_SIGNAL, (void*)&si);
			
			/* Analog processing */
			scanner->curr_ts->type = AM_SCAN_TS_ANALOG;
			scanner->curr_ts->analog.freq = am_scan_format_atv_freq(scanner->atvctl.afc_locked_freq);
			scanner->curr_ts->analog.audio_std = dvb_fend_para(cur_fe_para)->u.analog.soundsys;
			scanner->curr_ts->analog.video_std = dvb_fend_para(cur_fe_para)->u.analog.std;
			/*添加到搜索结果列表*/
			APPEND_TO_LIST(AM_SCAN_TS_t, scanner->curr_ts, scanner->result.tses);
			if (atv_start_para.mode == AM_SCAN_ATVMODE_MANUAL)
			{
				AM_DEBUG(1, "ATV manual scan done!");
				/* this will cause start dtv or complete scan */
				am_scan_start_next_ts(scanner);
			}
			else
			{
				scanner->atvctl.step =  atv_start_para.cvbs_locked_step;
				am_scan_atv_step_tune(scanner);
			}
		}
		else
		{
			int i;
			
			/* Digital processing */
			scanner->curr_ts->type = AM_SCAN_TS_DIGITAL;
	
			scanner->curr_ts->digital.snr = 0;
			scanner->curr_ts->digital.ber = 0;
			scanner->curr_ts->digital.strength = 0;
		
			if (dtv_start_para.standard == AM_SCAN_DTV_STD_ATSC)
			{
				am_scan_tablectl_clear(&scanner->dtvctl.patctl);
				for (i=0; i<(int)AM_ARRAY_SIZE(scanner->dtvctl.pmtctl); i++)
				{
					am_scan_tablectl_clear(&scanner->dtvctl.pmtctl[i]);
				}
				am_scan_tablectl_clear(&scanner->dtvctl.catctl);
				am_scan_tablectl_clear(&scanner->dtvctl.mgtctl);
			
				/*请求数据*/
				//am_scan_request_section(scanner, &scanner->dtvctl.patctl);
				am_scan_request_section(scanner, &scanner->dtvctl.catctl);
				am_scan_request_section(scanner, &scanner->dtvctl.mgtctl);
			}
			else
			{
				am_scan_tablectl_clear(&scanner->dtvctl.patctl);
				for (i=0; i<(int)AM_ARRAY_SIZE(scanner->dtvctl.pmtctl); i++)
				{
					am_scan_tablectl_clear(&scanner->dtvctl.pmtctl[i]);
				}
				am_scan_tablectl_clear(&scanner->dtvctl.catctl);
				am_scan_tablectl_clear(&scanner->dtvctl.sdtctl);

				/*请求数据*/
				am_scan_request_section(scanner, &scanner->dtvctl.patctl);
				am_scan_request_section(scanner, &scanner->dtvctl.catctl);
				am_scan_request_section(scanner, &scanner->dtvctl.sdtctl);

				/*In order to support lcn, we need scan NIT for each TS */
				am_scan_tablectl_clear(&scanner->dtvctl.nitctl);
				am_scan_request_section(scanner, &scanner->dtvctl.nitctl);
			}
		

			scanner->curr_ts->digital.fend_para = cur_fe_para;
			scanner->start_freqs[scanner->curr_freq].dtv_locked = AM_TRUE;
			/*添加到搜索结果列表*/
			APPEND_TO_LIST(AM_SCAN_TS_t, scanner->curr_ts, scanner->result.tses);
		}
	}
	else if (scanner->stage == AM_SCAN_STAGE_NIT)
	{
		am_scan_tablectl_clear(&scanner->dtvctl.nitctl);
		am_scan_tablectl_clear(&scanner->dtvctl.batctl);
		
		/*请求NIT, BAT数据*/
		am_scan_request_section(scanner, &scanner->dtvctl.nitctl);
		if (dtv_start_para.mode & AM_SCAN_DTVMODE_SEARCHBAT)
			am_scan_request_section(scanner, &scanner->dtvctl.batctl);
	}
	else
	{
		AM_DEBUG(1, "FEND event arrive at unexpected scan stage %d", scanner->stage);
	}
	
	return 0;
}

/**\brief SCAN前端事件处理*/
static void am_scan_solve_fend_evt(AM_SCAN_Scanner_t *scanner)
{
	int i;
	unsigned int freq;
	unsigned int cur_drift, max_drift;
	AM_SCAN_DTVSignalInfo_t si;

	if (scanner->curr_freq < 0 || scanner->curr_freq >= scanner->start_freqs_cnt)
	{
		AM_DEBUG(1, "Unexpected fend_evt arrived: curr_freq = %d", scanner->curr_freq);
		return;
	}
	if ( ! (scanner->recv_status & AM_SCAN_RECVING_WAIT_FEND))
	{
		AM_DEBUG(1, "Unexpected fend_evt arrived: donot wait for any fend event");
		return;
	}

	if (cur_fe_para.m_type == FE_ANALOG)
	{
		freq = scanner->fe_evt.parameters.frequency;
		if (abs(dvb_fend_para(cur_fe_para)->frequency - freq) > atv_start_para.afc_range)
		{
			AM_DEBUG(1, "Unexpected fend_evt arrived, expect frequency %u,  got %u, exceeds afc_range", 
				dvb_fend_para(cur_fe_para)->frequency, freq);
			return;
		}
	}
	else
	{
		/* For DVBS, we must do some extra control */
		if (dtv_start_para.source == FE_QPSK)
		{
			int tmp_drift;
			
			AM_ErrorCode_t ret = AM_SEC_FreqConvert(scanner->start_para.fend_dev_id, scanner->fe_evt.parameters.frequency, &freq);
			if(ret != AM_SUCCESS)
			{
				AM_DEBUG(1, "error sec freq convert, try next\n");
				scanner->recv_status &= ~AM_SCAN_RECVING_WAIT_FEND;
				goto try_next;
			}
			
			AM_DEBUG(1, "%d %d %d, max_drift %d, unicable %d", scanner->fe_evt.parameters.frequency, freq, 
				dvb_fend_para(cur_fe_para)->frequency, cur_fe_para.sat.para.u.qpsk.symbol_rate / 2000,
				(dtv_start_para.mode&AM_SCAN_DTVMODE_SAT_UNICABLE)?1:0);
				
			tmp_drift = dvb_fend_para(cur_fe_para)->frequency - freq;
			cur_drift = AM_ABS(tmp_drift);
			/*algorithm from kernel*/
			max_drift = cur_fe_para.sat.para.u.qpsk.symbol_rate / 2000;
			
			if ((cur_drift > max_drift) && !(dtv_start_para.mode&AM_SCAN_DTVMODE_SAT_UNICABLE))
			{
				AM_DEBUG(1, "Unexpected fend_evt arrived dvbs %d %d", cur_drift, max_drift);
				return;
			}	
		}
		else
		{
			freq = scanner->fe_evt.parameters.frequency;
			
			if (dvb_fend_para(cur_fe_para)->frequency != freq)
			{
				AM_DEBUG(1, "Unexpected fend_evt arrived, expect frequency %u, but got %u", dvb_fend_para(cur_fe_para)->frequency, freq);
				return;
			}
		}
	}
	
	
	AM_DEBUG(1, "Scan get fend event: %u %s!", scanner->fe_evt.parameters.frequency, \
			(scanner->fe_evt.status&FE_HAS_LOCK) ? "Locked" : "Unlocked");
	memset(&si, 0, sizeof(si));
	/*获取前端信号质量等信息并通知*/
	if (cur_fe_para.m_type != FE_ANALOG && 
		GET_MODE(dtv_start_para.mode) != AM_SCAN_DTVMODE_SAT_BLIND)
	{
		AM_FEND_GetSNR(scanner->start_para.fend_dev_id, &si.snr);
		AM_FEND_GetBER(scanner->start_para.fend_dev_id, &si.ber);
		AM_FEND_GetStrength(scanner->start_para.fend_dev_id, &si.strength);
		si.locked = (scanner->fe_evt.status&FE_HAS_LOCK) ? AM_TRUE : AM_FALSE;
	}
	si.frequency = scanner->fe_evt.parameters.frequency;
	SIGNAL_EVENT(AM_SCAN_EVT_SIGNAL, (void*)&si);
		
	scanner->recv_status &= ~AM_SCAN_RECVING_WAIT_FEND;

	if (scanner->fe_evt.status & FE_HAS_LOCK)
	{	
		if (cur_fe_para.m_type == FE_ANALOG)
		{
			/* Update the frequency */
			scanner->atvctl.afc_locked_freq = scanner->fe_evt.parameters.frequency;
		}
		/* Get locked, process it */
		if (am_scan_new_ts_locked_proc(scanner) < 0) {
			goto try_next;
		} else {
			return;
		}
	}

try_next:
	/*尝试下一频点*/	
	if (cur_fe_para.m_type == FE_ANALOG)
	{
		scanner->atvctl.step = atv_start_para.afc_unlocked_step;
		am_scan_atv_step_tune(scanner);
	}
	else if (scanner->stage == AM_SCAN_STAGE_NIT)
	{
		am_scan_try_nit(scanner);
	}
	else if (scanner->stage == AM_SCAN_STAGE_TS)
	{
		scanner->start_freqs[scanner->curr_freq].dtv_locked = AM_FALSE;
		am_scan_start_next_ts(scanner);
	}

}

/**\brief 比较2个timespec*/
static int am_scan_compare_timespec(const struct timespec *ts1, const struct timespec *ts2)
{
	assert(ts1 && ts2);

	if ((ts1->tv_sec > ts2->tv_sec) ||
		((ts1->tv_sec == ts2->tv_sec)&&(ts1->tv_nsec > ts2->tv_nsec)))
		return 1;
		
	if ((ts1->tv_sec == ts2->tv_sec) && (ts1->tv_nsec == ts2->tv_nsec))
		return 0;
	
	return -1;
}

/**\brief 计算下一等待超时时间，存储到ts所指结构中*/
static void am_scan_get_wait_timespec(AM_SCAN_Scanner_t *scanner, struct timespec *ts)
{
	int rel, i;
	struct timespec now;

#define TIMEOUT_CHECK(table)\
	AM_MACRO_BEGIN\
		struct timespec end;\
		if (scanner->recv_status & scanner->dtvctl.table.recv_flag){\
			end = scanner->dtvctl.table.end_time;\
			rel = am_scan_compare_timespec(&scanner->dtvctl.table.end_time, &now);\
			if (rel <= 0){\
				AM_DEBUG(1, "%s timeout", scanner->dtvctl.table.tname);\
				scanner->dtvctl.table.data_arrive_time = 1;/*just mark it*/\
				scanner->dtvctl.table.done(scanner);\
			}\
			if (rel > 0 || memcmp(&end, &scanner->dtvctl.table.end_time, sizeof(struct timespec))){\
				if ((ts->tv_sec == 0 && ts->tv_nsec == 0) || \
					(am_scan_compare_timespec(&scanner->dtvctl.table.end_time, ts) < 0))\
					*ts = scanner->dtvctl.table.end_time;\
			}\
		}\
	AM_MACRO_END

	ts->tv_sec = 0;
	ts->tv_nsec = 0;
	AM_TIME_GetTimeSpec(&now);
	
	/*超时检查*/
	TIMEOUT_CHECK(patctl);
	for (i=0; i<(int)AM_ARRAY_SIZE(scanner->dtvctl.pmtctl); i++)
	{
		if (scanner->dtvctl.pmtctl[i].fid >= 0)
			TIMEOUT_CHECK(pmtctl[i]);
	}
	TIMEOUT_CHECK(catctl);
	if (dtv_start_para.standard == AM_SCAN_DTV_STD_ATSC)
	{
		TIMEOUT_CHECK(mgtctl);
		TIMEOUT_CHECK(vctctl);
	}
	else
	{
		TIMEOUT_CHECK(sdtctl);
		TIMEOUT_CHECK(nitctl);
		TIMEOUT_CHECK(batctl);
	}
	
	/* All tables complete, try next ts */
	if (scanner->stage == AM_SCAN_STAGE_TS && \
		scanner->recv_status == AM_SCAN_RECVING_COMPLETE)
		am_scan_start_next_ts(scanner);
	
}

/**\brief SCAN线程*/
static void *am_scan_thread(void *para)
{
	AM_SCAN_Scanner_t *scanner = (AM_SCAN_Scanner_t*)para;
	AM_SCAN_TS_t *ts, *tnext;
	struct timespec to;
	int ret, evt_flag, i;
	AM_Bool_t go = AM_TRUE;

	pthread_mutex_lock(&scanner->lock);
	
	while (go)
	{
		/*检查超时，并计算下一个超时时间*/
		am_scan_get_wait_timespec(scanner, &to);

		ret = 0;
		/*等待事件*/
		if(scanner->evt_flag == 0)
		{
			if (to.tv_sec == 0 && to.tv_nsec == 0)
				ret = pthread_cond_wait(&scanner->cond, &scanner->lock);
			else
				ret = pthread_cond_timedwait(&scanner->cond, &scanner->lock, &to);
		}
		
		if (ret != ETIMEDOUT)
		{
handle_events:
			evt_flag = scanner->evt_flag;
			AM_DEBUG(2, "Evt flag 0x%x", scanner->evt_flag);

			/*开始搜索事件*/
			if ((evt_flag&AM_SCAN_EVT_START) && (scanner->dtvctl.hsi == 0))
			{
				SET_PROGRESS_EVT(AM_SCAN_PROGRESS_SCAN_BEGIN, (void*)scanner);
				am_scan_start(scanner);
			}

			/*前端事件*/
			if (evt_flag & AM_SCAN_EVT_FEND)
				am_scan_solve_fend_evt(scanner);

			/*PAT表收齐事件*/
			if (evt_flag & AM_SCAN_EVT_PAT_DONE)
				scanner->dtvctl.patctl.done(scanner);
			/*PMT表收齐事件*/
			if (evt_flag & AM_SCAN_EVT_PMT_DONE)
				scanner->dtvctl.pmtctl[0].done(scanner);
			/*CAT表收齐事件*/
			if (evt_flag & AM_SCAN_EVT_CAT_DONE)
				scanner->dtvctl.catctl.done(scanner);
			/*SDT表收齐事件*/
			if (evt_flag & AM_SCAN_EVT_SDT_DONE)
				scanner->dtvctl.sdtctl.done(scanner);
			/*NIT表收齐事件*/
			if (evt_flag & AM_SCAN_EVT_NIT_DONE)
				scanner->dtvctl.nitctl.done(scanner);
			/*BAT表收齐事件*/
			if (evt_flag & AM_SCAN_EVT_BAT_DONE)
				scanner->dtvctl.batctl.done(scanner);
			/*MGT表收齐事件*/
			if (evt_flag & AM_SCAN_EVT_MGT_DONE)
				scanner->dtvctl.mgtctl.done(scanner);
			/*VCT表收齐事件*/
			if (evt_flag & AM_SCAN_EVT_VCT_DONE)
				scanner->dtvctl.vctctl.done(scanner);
			/*完成一次卫星盲扫*/
			if (evt_flag & AM_SCAN_EVT_BLIND_SCAN_DONE)
				am_scan_solve_blind_scan_done_evt(scanner);
				
			/*退出事件*/
			if (evt_flag & AM_SCAN_EVT_QUIT)
			{
				go = AM_FALSE;	
				continue;
			}
			
			/*在调用am_scan_free_filter时可能会产生新事件*/
			scanner->evt_flag &= ~evt_flag;
			if (scanner->evt_flag)
			{
				goto handle_events;
			}
		}
	}
	
	/* need store ? */
	if (scanner->result.tses/* && scanner->stage == AM_SCAN_STAGE_DONE*/ && scanner->store)
	{
		AM_DEBUG(1, "Call store proc");
		SET_PROGRESS_EVT(AM_SCAN_PROGRESS_STORE_BEGIN, NULL);
		if (scanner->store_cb)
			scanner->store_cb(&scanner->result);
		SET_PROGRESS_EVT(AM_SCAN_PROGRESS_STORE_END, NULL);
	}
	
	am_scan_stop_atv(scanner);
	am_scan_stop_dtv(scanner);
				
	/*反注册前端事件*/
	AM_EVT_Unsubscribe(scanner->start_para.fend_dev_id, AM_FEND_EVT_STATUS_CHANGED, am_scan_fend_callback, (void*)scanner);

	pthread_mutex_unlock(&scanner->lock);
	pthread_mutex_destroy(&scanner->lock);
	pthread_cond_destroy(&scanner->cond);
	free(scanner);		
	
	return NULL;
}


/**\brief 拷贝数字前端参数*/
static void am_scan_copy_dtv_feparas(AM_SCAN_DTVCreatePara_t *para, AM_Bool_t use_default, AM_SCAN_FrontEndPara_t *fe_start)
{
	int i;
	AM_SCAN_FrontEndPara_t *start = fe_start;
	
	if (((GET_MODE(para->mode) == AM_SCAN_DTVMODE_ALLBAND) 
		||(GET_MODE(para->mode) == AM_SCAN_DTVMODE_AUTO))
		&& use_default )
	{
		struct dvb_frontend_parameters tpara;
		
		/* User not specify the fe_paras, using default */
		if(para->source==FE_QAM) 
		{
			tpara.u.qam.modulation = DEFAULT_DVBC_MODULATION;
			tpara.u.qam.symbol_rate = DEFAULT_DVBC_SYMBOLRATE;
			for (i=0; i<para->fe_cnt; i++,fe_start++)
			{
				tpara.frequency = dvbc_std_freqs[i]*1000;
				*dvb_fend_para(fe_start->dtv_para) = tpara;
			}
		} 
		else if(para->source==FE_OFDM) 
		{
			tpara.u.ofdm.bandwidth = DEFAULT_DVBT_BW;
			for (i=0; i<para->fe_cnt; i++,fe_start++)
			{
				tpara.frequency = (DEFAULT_DVBT_FREQ_START+(8000*i))*1000;
				*dvb_fend_para(fe_start->dtv_para) = tpara;
			}
		}
	}
	else if (GET_MODE(para->mode) != AM_SCAN_DTVMODE_SAT_BLIND)
	{
		for (i=0; i<para->fe_cnt; i++,fe_start++)
		{
			fe_start->dtv_para = para->fe_paras[i];
		}
	}
	
	AM_DEBUG(1, "DTV fe_paras count %d", fe_start - start);
}

/**\brief 拷贝数字前端参数*/
static void am_scan_copy_atv_feparas(AM_SCAN_ATVCreatePara_t *para, AM_SCAN_FrontEndPara_t *fe_start)
{
	int i;
	
	for (i=0; i<para->fe_cnt; i++,fe_start++)
	{
		fe_start->dtv_para = para->fe_paras[i];
		AM_DEBUG(1, "ATV fe_para mode %d, freq %u", fe_start->dtv_para.m_type, dvb_fend_para(fe_start->dtv_para)->frequency);
		if (para->mode != AM_SCAN_ATVMODE_FREQ)
		{
			dvb_fend_para(fe_start->dtv_para)->u.analog.soundsys = para->default_aud_std;
			dvb_fend_para(fe_start->dtv_para)->u.analog.std = para->default_vid_std;
		}
		dvb_fend_para(fe_start->dtv_para)->u.analog.flag |= ANALOG_FLAG_ENABLE_AFC;
		if (para->afc_range <= 0)
			para->afc_range = ATV_2MHZ;
		dvb_fend_para(fe_start->dtv_para)->u.analog.afc_range = para->afc_range;
	}
	
	AM_DEBUG(1, "ATV fe_paras count %d", para->fe_cnt);
}
 
/****************************************************************************
 * API functions
 ***************************************************************************/

 /**\brief 创建节目搜索
 * \param [in] para 创建参数
 * \param [out] handle 返回SCAN句柄
 * \return
 *   - AM_SUCCESS 成功
 *   - 其他值 错误代码(见am_scan.h)
 */
AM_ErrorCode_t AM_SCAN_Create(AM_SCAN_CreatePara_t *para, int *handle)
{
	AM_SCAN_Scanner_t *scanner;
	int rc;
	pthread_mutexattr_t mta;
	int use_default=0;
	int i;
	
	if (! para || ! handle)
		return AM_SCAN_ERR_INVALID_PARAM;
	*handle = 0;	
	if (para->atv_para.mode < AM_SCAN_ATVMODE_NONE)
	{
		if (para->atv_para.fe_cnt < 0 || !para->atv_para.fe_paras)
			return AM_SCAN_ERR_INVALID_PARAM;
		/* need to scan ATV */
		if (para->atv_para.mode != AM_SCAN_ATVMODE_FREQ && para->atv_para.fe_cnt < 3)
		{
			AM_DEBUG(1, "Invalid ATV fe count, must >= 3 to specify the min,max and start freq");
			return AM_SCAN_ERR_INVALID_PARAM;
		}
	}
	else
	{
		para->atv_para.fe_cnt = 0;
	}
	
	if ((GET_MODE(para->dtv_para.mode) < AM_SCAN_DTVMODE_NONE))
	{
		/* need to scan DTV */
		if (para->dtv_para.fe_cnt < 0 ||
			para->dtv_para.standard >= AM_SCAN_DTV_STD_MAX ||
			((GET_MODE(para->dtv_para.mode) == AM_SCAN_DTVMODE_MANUAL) && (para->dtv_para.fe_cnt == 0 || !para->dtv_para.fe_paras)) ||
			(para->dtv_para.fe_cnt > 0 && !para->dtv_para.fe_paras))
			return AM_SCAN_ERR_INVALID_PARAM;
			
		if (GET_MODE(para->mode) == AM_SCAN_DTVMODE_MANUAL)
		{
			para->dtv_para.fe_cnt = 1;
		}
		else if (((GET_MODE(para->dtv_para.mode) ==  AM_SCAN_DTVMODE_ALLBAND) || 
				(GET_MODE(para->dtv_para.mode) ==  AM_SCAN_DTVMODE_AUTO)) && 
				para->dtv_para.fe_cnt < 0)
		{
			use_default = 1;
			para->dtv_para.fe_cnt = (para->dtv_para.source==FE_QAM) ? AM_ARRAY_SIZE(dvbc_std_freqs) 
				: ((para->dtv_para.source==FE_OFDM) ? (DEFAULT_DVBT_FREQ_STOP-DEFAULT_DVBT_FREQ_START)/8000+1
				: 0/* Otherwise, user must specify the fe_paras */);
		}
	}
	else
	{
		para->dtv_para.fe_cnt = 0;
	}
	
	/*Create a scanner*/
	scanner = (AM_SCAN_Scanner_t*)malloc(sizeof(AM_SCAN_Scanner_t));
	if (scanner == NULL)
	{
		AM_DEBUG(1, "Cannot create scan, no enough memory");
		return AM_SCAN_ERR_NO_MEM;
	}
	/*数据初始化*/
	memset(scanner, 0, sizeof(AM_SCAN_Scanner_t));
	
	/*配置起始频点*/
	if (para->mode == AM_SCAN_MODE_ADTV)
	{
		scanner->start_freqs_cnt = (para->atv_para.fe_cnt > 0) ? para->atv_para.fe_cnt : para->dtv_para.fe_cnt;
	}
	else
	{
		if (GET_MODE(para->dtv_para.mode) == AM_SCAN_DTVMODE_SAT_BLIND)
			scanner->start_freqs_cnt = AM_SCAN_MAX_BS_TP_CNT;
		else
			scanner->start_freqs_cnt = para->dtv_para.fe_cnt;
		/* atv fe + dtv fe */
		scanner->start_freqs_cnt += para->atv_para.fe_cnt;
	}
	
	if (scanner->start_freqs_cnt > 0)
	{
		scanner->start_freqs = (AM_SCAN_FrontEndPara_t*)malloc(sizeof(AM_SCAN_FrontEndPara_t)*scanner->start_freqs_cnt);
		if (!scanner->start_freqs)
		{
			AM_DEBUG(1, "Cannot create scan, no enough memory");
			free(scanner);
			return AM_SCAN_ERR_NO_MEM;
		}
		memset(scanner->start_freqs, 0, sizeof(AM_SCAN_FrontEndPara_t) * scanner->start_freqs_cnt);
	}
	else
	{
		AM_DEBUG(1, "Donot sepcify any atv/dtv fe_paras");
		free(scanner);
		return AM_SCAN_ERR_INVALID_PARAM; 
	}
	
	if (para->mode == AM_SCAN_MODE_ADTV)
	{
		AM_FENDCTRL_DVBFrontendParameters_t *fep;
		
		fep = (scanner->start_freqs_cnt == para->atv_para.fe_cnt) ? para->atv_para.fe_paras : para->dtv_para.fe_paras;
		for (i=0; i<scanner->start_freqs_cnt; i++)
		{
			scanner->start_freqs[i].dtv_para = *fep++;
		}
		scanner->atvctl.start_idx = 0;
		scanner->dtvctl.start_idx = 0;
	}
	else if (para->mode == AM_SCAN_MODE_ATV_DTV)
	{
		am_scan_copy_atv_feparas(&para->atv_para, scanner->start_freqs);
		am_scan_copy_dtv_feparas(&para->dtv_para, use_default, scanner->start_freqs + para->atv_para.fe_cnt);
		scanner->atvctl.start_idx = 0;
		scanner->dtvctl.start_idx = para->atv_para.fe_cnt;
	}
	else
	{
		am_scan_copy_dtv_feparas(&para->dtv_para, use_default, scanner->start_freqs);
		am_scan_copy_atv_feparas(&para->atv_para, scanner->start_freqs + para->dtv_para.fe_cnt);
		scanner->atvctl.start_idx = para->dtv_para.fe_cnt;
		scanner->dtvctl.start_idx = 0;
	}
	
	AM_DEBUG(1, "Total fe_paras count %d", scanner->start_freqs_cnt);
	
	scanner->start_para = *para;
	dtv_start_para.fe_paras = NULL;
	atv_start_para.fe_paras = NULL;
	if (atv_start_para.afc_unlocked_step <= 0)
		atv_start_para.afc_unlocked_step = DEFAULT_AFC_UNLOCK_STEP;
	if (atv_start_para.cvbs_unlocked_step <= 0)
		atv_start_para.cvbs_unlocked_step = DEFAULT_CVBS_UNLOCK_STEP;
	if (atv_start_para.cvbs_locked_step <= 0)
		atv_start_para.cvbs_locked_step = DEFAULT_CVBS_LOCK_STEP;
	scanner->result.start_para = &scanner->start_para;
	scanner->atvctl.afe_fd = -1;
	scanner->atvctl.vdin_fd = -1;
	
	if (! para->store_cb)
		scanner->store_cb = am_scan_default_store;
	else
		scanner->store_cb = para->store_cb;
	
	pthread_mutexattr_init(&mta);
	pthread_mutexattr_settype(&mta, PTHREAD_MUTEX_RECURSIVE_NP);
	pthread_mutex_init(&scanner->lock, &mta);
	pthread_cond_init(&scanner->cond, NULL);
	pthread_mutexattr_destroy(&mta);
	
	rc = pthread_create(&scanner->thread, NULL, am_scan_thread, (void*)scanner);
	if(rc)
	{
		AM_DEBUG(1, "%s", strerror(rc));
		pthread_mutex_destroy(&scanner->lock);
		pthread_cond_destroy(&scanner->cond);
		free(scanner->start_freqs);
		free(scanner);
		return AM_SCAN_ERR_CANNOT_CREATE_THREAD;
	}
	
	*handle = (int)scanner;
	return AM_SUCCESS;
}

/**\brief 启动节目搜索
 * \param handle Scan句柄
 * \return
 *   - AM_SUCCESS 成功
 *   - 其他值 错误代码(见am_scan.h)
 */
AM_ErrorCode_t AM_SCAN_Start(int handle)
{
	AM_SCAN_Scanner_t *scanner = (AM_SCAN_Scanner_t*)handle;

	if (scanner)
	{
		pthread_mutex_lock(&scanner->lock);
		/*启动搜索*/
		scanner->evt_flag |= AM_SCAN_EVT_START;
		pthread_cond_signal(&scanner->cond);
		pthread_mutex_unlock(&scanner->lock);
	}

	return AM_SUCCESS;
}

/**\brief 销毀节目搜索
 * \param handle Scan句柄
 * \param store 是否存储
 * \return
 *   - AM_SUCCESS 成功
 *   - 其他值 错误代码(见am_scan.h)
 */
AM_ErrorCode_t AM_SCAN_Destroy(int handle, AM_Bool_t store)
{
	AM_SCAN_Scanner_t *scanner = (AM_SCAN_Scanner_t*)handle;

	if (scanner)
	{
		pthread_t t;
		
		pthread_mutex_lock(&scanner->lock);
		/*等待搜索线程退出*/
		scanner->evt_flag |= AM_SCAN_EVT_QUIT;
		scanner->store = store;
		t = scanner->thread;
		pthread_cond_signal(&scanner->cond);
		pthread_mutex_unlock(&scanner->lock);

		if (t != pthread_self())
			pthread_join(t, NULL);
	}

	return AM_SUCCESS;
}

/**\brief 设置用户数据
 * \param handle Scan句柄
 * \param [in] user_data 用户数据
 * \return
 *   - AM_SUCCESS 成功
 *   - 其他值 错误代码(见am_scan.h)
 */
AM_ErrorCode_t AM_SCAN_SetUserData(int handle, void *user_data)
{
	AM_SCAN_Scanner_t *scanner = (AM_SCAN_Scanner_t*)handle;

	if (scanner)
	{
		pthread_mutex_lock(&scanner->lock);
		scanner->user_data = user_data;
		pthread_mutex_unlock(&scanner->lock);
	}

	return AM_SUCCESS;
}

/**\brief 取得用户数据
 * \param handle Scan句柄
 * \param [in] user_data 用户数据
 * \return
 *   - AM_SUCCESS 成功
 *   - 其他值 错误代码(见am_scan.h)
 */
AM_ErrorCode_t AM_SCAN_GetUserData(int handle, void **user_data)
{
	AM_SCAN_Scanner_t *scanner = (AM_SCAN_Scanner_t*)handle;

	assert(user_data);
	
	if (scanner)
	{
		pthread_mutex_lock(&scanner->lock);
		*user_data = scanner->user_data;
		pthread_mutex_unlock(&scanner->lock);
	}

	return AM_SUCCESS;
}

