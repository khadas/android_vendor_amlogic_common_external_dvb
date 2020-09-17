#include <am_debug.h>
#include <am_fend.h>
#include <am_vlfend.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>

#define VLFEND_DEV_NO (0)

#define TVAFE_DEV_NEED (1)
#define VDIN_DEV_NEED  (1)

#define V4L2_COLOR_STD_PAL      (0x04000000)
#define V4L2_COLOR_STD_NTSC     (0x08000000)
#define V4L2_COLOR_STD_SECAM    (0x10000000)

#define V4L2_STD_PAL_B          (0x00000001)
#define V4L2_STD_PAL_B1         (0x00000002)
#define V4L2_STD_PAL_G          (0x00000004)
#define V4L2_STD_PAL_H          (0x00000008)
#define V4L2_STD_PAL_I          (0x00000010)
#define V4L2_STD_PAL_D          (0x00000020)
#define V4L2_STD_PAL_D1         (0x00000040)
#define V4L2_STD_PAL_K          (0x00000080)

#define V4L2_STD_PAL_M          (0x00000100)
#define V4L2_STD_PAL_N          (0x00000200)
#define V4L2_STD_PAL_Nc         (0x00000400)
#define V4L2_STD_PAL_60         (0x00000800)

#define V4L2_STD_NTSC_M         (0x00001000)	/* BTSC */
#define V4L2_STD_NTSC_M_JP      (0x00002000)	/* EIA-J */
#define V4L2_STD_NTSC_443       (0x00004000)
#define V4L2_STD_NTSC_M_KR      (0x00008000)	/* FM A2 */

#define V4L2_STD_SECAM_B        (0x00010000)
#define V4L2_STD_SECAM_D        (0x00020000)
#define V4L2_STD_SECAM_G        (0x00040000)
#define V4L2_STD_SECAM_H        (0x00080000)
#define V4L2_STD_SECAM_K        (0x00100000)
#define V4L2_STD_SECAM_K1       (0x00200000)
#define V4L2_STD_SECAM_L        (0x00400000)
#define V4L2_STD_SECAM_LC       (0x00800000)

#define V4L2_STD_PAL_BG         (V4L2_STD_PAL_B | V4L2_STD_PAL_B1 | V4L2_STD_PAL_G)
#define V4L2_STD_PAL_DK         (V4L2_STD_PAL_D | V4L2_STD_PAL_D1 | V4L2_STD_PAL_K)

#define TVIN_IOC_MAGIC 'T'
#define TVIN_IOC_OPEN               _IOW(TVIN_IOC_MAGIC, 0x01, struct tvin_parm_s)
#define TVIN_IOC_START_DEC          _IOW(TVIN_IOC_MAGIC, 0x02, struct tvin_parm_s)
#define TVIN_IOC_STOP_DEC           _IO(TVIN_IOC_MAGIC, 0x03)
#define TVIN_IOC_CLOSE              _IO(TVIN_IOC_MAGIC, 0x04)
#define TVIN_IOC_G_SIG_INFO         _IOR(TVIN_IOC_MAGIC, 0x07, struct tvin_info_s)
#define TVIN_IOC_S_AFE_CVBS_STD     _IOW(TVIN_IOC_MAGIC, 0x1b, enum tvin_sig_fmt_e)


enum tvin_port_e {
	TVIN_PORT_CVBS3 = 0x00001003,
};

enum tvin_sig_fmt_e {
    TVIN_SIG_FMT_NULL = 0,
    //Video Formats
    TVIN_SIG_FMT_CVBS_NTSC_M                        = 0x601,
    TVIN_SIG_FMT_CVBS_NTSC_443                      = 0x602,
    TVIN_SIG_FMT_CVBS_PAL_I                         = 0x603,
    TVIN_SIG_FMT_CVBS_PAL_M                         = 0x604,
    TVIN_SIG_FMT_CVBS_PAL_60                        = 0x605,
    TVIN_SIG_FMT_CVBS_PAL_CN                        = 0x606,
    TVIN_SIG_FMT_CVBS_SECAM                         = 0x607,
    TVIN_SIG_FMT_CVBS_NTSC_50                       = 0x608,
    TVIN_SIG_FMT_CVBS_MAX                           = 0x609,
    TVIN_SIG_FMT_CVBS_THRESHOLD                     = 0x800,
    TVIN_SIG_FMT_MAX,
};

enum tvin_trans_fmt {
	TVIN_TFMT_2D = 0,
};

enum tvin_aspect_ratio_e {
    TVIN_ASPECT_NULL = 0,
    TVIN_ASPECT_1x1,
    TVIN_ASPECT_4x3_FULL,
    TVIN_ASPECT_14x9_FULL,
    TVIN_ASPECT_14x9_LB_CENTER,
    TVIN_ASPECT_14x9_LB_TOP,
    TVIN_ASPECT_16x9_FULL,
    TVIN_ASPECT_16x9_LB_CENTER,
    TVIN_ASPECT_16x9_LB_TOP,
    TVIN_ASPECT_MAX,
};

enum tvin_sig_status_e {
    TVIN_SIG_STATUS_NULL = 0, // processing status from init to the finding of the 1st confirmed status
    TVIN_SIG_STATUS_NOSIG,    // no signal - physically no signal
    TVIN_SIG_STATUS_UNSTABLE, // unstable - physically bad signal
    TVIN_SIG_STATUS_NOTSUP,   // not supported - physically good signal & not supported
    TVIN_SIG_STATUS_STABLE,   // stable - physically good signal & supported
};

enum color_fmt_e {
    RGB444 = 0,
    YUV422, // 1
    YUV444, // 2
    YUYV422,// 3
    YVYU422,// 4
    UYVY422,// 5
    VYUY422,// 6
    NV12,   // 7
    NV21,   // 8
    BGGR,   // 9  raw data
    RGGB,   // 10 raw data
    GBRG,   // 11 raw data
    GRBG,   // 12 raw data
    COLOR_FMT_MAX,
};

struct tvin_info_s
{
	enum tvin_trans_fmt trans_fmt;
	enum tvin_sig_fmt_e fmt;
	enum tvin_sig_status_e status;
	enum color_fmt_e cfmt;
	unsigned int fps;
	unsigned int is_dvi;
	unsigned int hdr_info;
	enum tvin_aspect_ratio_e aspect_ratio;
};

struct tvin_parm_s
{
	int index;                      // index of frontend for vdin
	enum tvin_port_e port;          // must set port in IOCTL
	struct tvin_info_s info;
	unsigned int hist_pow;
	unsigned int luma_sum;
	unsigned int pixel_sum;
	unsigned short histgram[64];
	unsigned int flag;
	unsigned short dest_width;      //for vdin horizontal scale down
	unsigned short dest_height;     //for vdin vertical scale down
	_Bool h_reverse;                 //for vdin horizontal reverse
	_Bool v_reverse;                 //for vdin vertical reverse
	unsigned int reserved;
};

#if TVAFE_DEV_NEED
static int fd_tvafe = -1;
#endif
#if VDIN_DEV_NEED
static int fd_vdin = -1;
#endif

static char *str_vstd[] = {
	"PAL",
	"NTSC",
	"SECAM",
	"UNKNOWN",
};

static char *str_astd[] = {
	"DK",
	"I",
	"BG",
	"M",
	"L",
	"LC",
	"UNKNOWN",
};

static char *str_cvbs[] = {
	"PAL_STD",
	"PAL_M",
	"PAL_N",
	"PAL_60",
	"NTSC_M",
	"NTSC_443",
	"SECAM",
	"UNKNOWN",
};

static enum tvin_sig_fmt_e fmt_cvbs[] = {
	TVIN_SIG_FMT_CVBS_PAL_I,
	TVIN_SIG_FMT_CVBS_PAL_M,
	TVIN_SIG_FMT_CVBS_PAL_CN,
	TVIN_SIG_FMT_CVBS_PAL_60,
	TVIN_SIG_FMT_CVBS_NTSC_M,
	TVIN_SIG_FMT_CVBS_NTSC_443,
	TVIN_SIG_FMT_CVBS_SECAM,
	TVIN_SIG_FMT_MAX
};

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
static int status = 0;

struct channel {
	unsigned int freq;
	unsigned int std;
	unsigned int vstd;
	unsigned int astd;
	unsigned int cvbs;
};

static int number = 0;
struct channel ch[64];

static int vlfend_play(unsigned int freq, unsigned int video_std,
		unsigned int audio_std, unsigned int cvbs)
{
	int ret = 0, num = 0, i = 0;
	struct dvb_frontend_parameters set_para, get_para;
	unsigned int std = 0;
	fe_status_t st;

	printf(">>> Tune Frequency = %d Hz, system: %s-%s (cvbs %s).\n",
			freq, str_vstd[video_std], str_astd[audio_std], str_cvbs[cvbs]);

	if (video_std == 0) {
		std = V4L2_COLOR_STD_PAL;
		if (audio_std == 0) {
			std |= V4L2_STD_PAL_DK;
		} else if (audio_std == 1) {
			std |= V4L2_STD_PAL_I;
		} else if (audio_std == 2) {
			std |= V4L2_STD_PAL_BG;
		} else if (audio_std == 3) {
			std |= V4L2_STD_PAL_M;
		}
	} else if (video_std == 1) {
		std = V4L2_COLOR_STD_NTSC;
		if (audio_std == 0) {
			std |= V4L2_STD_PAL_DK;
		} else if (audio_std == 1) {
			std |= V4L2_STD_PAL_I;
		} else if (audio_std == 2) {
			std |= V4L2_STD_PAL_BG;
		} else if (audio_std == 3) {
			std |= V4L2_STD_NTSC_M;
		}
	} else if (video_std == 2) {
		std = V4L2_COLOR_STD_SECAM;
		if (audio_std == 0) {
			std |= V4L2_STD_SECAM_DK;
		} else if (audio_std == 1) {
			std |= V4L2_STD_PAL_I;
		} else if (audio_std == 2) {
			std |= (V4L2_STD_SECAM_B | V4L2_STD_SECAM_G);
		} else if (audio_std == 3) {
			std |= V4L2_STD_NTSC_M;
		} else if (audio_std == 4) {
			std |= V4L2_STD_SECAM_L;
		} else if (audio_std == 5) {
			std |= V4L2_STD_SECAM_LC;
		}
	}

#if VDIN_DEV_NEED
	ret = ioctl(fd_vdin, TVIN_IOC_STOP_DEC);
#else
	printf("!!! Waring: Vdin device is also required for ATV playback.\n");
#endif

#if TVAFE_DEV_NEED
	enum tvin_sig_fmt_e fmt = fmt_cvbs[cvbs];
	ret = ioctl(fd_tvafe, TVIN_IOC_S_AFE_CVBS_STD, &fmt);
	if (ret) {
		printf("!!! Tvafe TVIN_IOC_S_AFE_CVBS_STD, error (%s).\n", strerror(errno));
		return -1;
	}
#else
	printf("!!! Waring: Tvafe device is also required for ATV playback.\n");
#endif

	set_para.frequency = freq;
	set_para.u.analog.audmode = std & 0x00FFFFFF;
	set_para.u.analog.soundsys = 0xFF;
	set_para.u.analog.std = std;
	set_para.u.analog.flag = 0;
	set_para.u.analog.afc_range = 0;

	if ((ret = AM_VLFEND_SetPara(VLFEND_DEV_NO, &set_para)) != 0) {
		printf("!!! Error: AM_VLFEND_SetPara error [ret = %d, %s].\n", ret, strerror(errno));
		return -1;
	}

	AM_VLFEND_GetStatus(VLFEND_DEV_NO, &st);

	if (st & FE_HAS_LOCK)
		printf("$$$ Status: Locked.\n");
	else
		printf("@@@ Status: Unlocked.\n");

	sleep(1);

#if VDIN_DEV_NEED
	struct tvin_info_s tvin_info;
	ret = ioctl(fd_vdin, TVIN_IOC_G_SIG_INFO, &tvin_info);
	if (ret) {
		printf("!!! Vdin TVIN_IOC_G_SIG_INFO, error (%s).\n", strerror(errno));
		return -1;
	}

	struct tvin_parm_s tvin_parm;
	tvin_parm.info = tvin_info;
	ret = ioctl(fd_vdin, TVIN_IOC_START_DEC, &tvin_info);
	if (ret) {
		printf("!!! Vdin TVIN_IOC_START_DEC, error (%s).\n", strerror(errno));
		return -1;
	}
#else
	printf("!!! Waring: Vdin device is also required for  ATV playback.\n");
#endif

	return 0;
}

static int vlfend_format_frequency(int freq)
{
	int tmp_val = 0;

	tmp_val = (freq % 1000000) / 10000;

	if (tmp_val >= 0 && tmp_val <= 30) {
		tmp_val = 25;
	} else if (tmp_val >= 70 && tmp_val <= 80) {
		tmp_val = 75;
	}

	freq = (freq / 1000000) * 1000000 + tmp_val * 10000;

	return freq;
}

static void vlfend_callback(int dev_no, struct dvb_frontend_event *evt, void *user_data)
{
	//printf("!!! Lock status: %s.\n", evt->status & FE_HAS_LOCK ? "Locked" : "Unlocked");
	pthread_mutex_lock(&lock);
	status = evt->status;
	pthread_cond_signal(&cond);
	pthread_mutex_unlock(&lock);
}

static int vlfend_lock(void)
{
	int ret = 0, num = 0, i = 0;
	struct dvb_frontend_parameters set_para, get_para;
	unsigned int freq = 0, video_std = 0, audio_std = 0, cvbs = 0;
	unsigned int std = 0, afc = 0, blind_scan = 0;
	fe_status_t st;

	printf("blind scan? [0 - No, 1 - Yes]: ");
	scanf("%d", &blind_scan);

	if (!blind_scan) {
		printf("Input vlfrontend parameters [frequency = 0 quit].\n");
		printf("Frequency(Hz): ");
		scanf("%d", &freq);

		if (!freq) {
			printf("!!! Error: input frequency error.\n");
			return -1;
		}

		printf("Color standard [0 - PAL, 1 - NTSC, 3 - SECAM]: ");
		scanf("%d", &video_std);
		printf("Audio standard [0 - DK, 1 - I, 2 - BG, 3 = M, 4 = L, 5 = LC]: ");
		scanf("%d", &audio_std);
		printf("Cvbs format [0 - PAL_I, 1 - PAL_M, 2 - PAL_N, 3 - NTSC-M, 4 - NTSC_443, 5 - SECAM]: ");
		scanf("%d", &cvbs);

		if (video_std > 2 || audio_std > 5 || cvbs > 6) {
			printf("!!! Error: input video/audio/cvbs error.\n");
			return -1;
		}

		vlfend_play(freq, video_std, audio_std, cvbs);
	} else {
		AM_TRY(AM_VLFEND_SetCallback(VLFEND_DEV_NO, vlfend_callback, NULL));

		memset(ch, 0, sizeof(ch));
		num = 0;

		pthread_mutex_lock(&lock);

		for (freq = 44250000; freq <= 868250000;) {
			set_para.frequency = freq;
			set_para.u.analog.audmode = 0;
			set_para.u.analog.soundsys = 0xFF;
			set_para.u.analog.std = 0;
			set_para.u.analog.flag = 1;
			set_para.u.analog.afc_range = 1000000;

			printf(">>> Start scan freq: %d. ", freq);
			if ((ret = AM_VLFEND_SetPara(VLFEND_DEV_NO, &set_para)) != 0) {
				printf("!!! Error: AM_VLFEND_SetPara error [ret = %d, %s].\n", ret, strerror(errno));
				return -1;
			}

			pthread_cond_wait(&cond, &lock);

			if (status & FE_HAS_LOCK) {
				AM_VLFEND_GetPara(VLFEND_DEV_NO, &get_para);

				std = get_para.u.analog.std;

				printf("Get status: Locked.\n");
				printf("------ channel info ------\n");
				printf("### frequency: %d Hz.\n", vlfend_format_frequency(get_para.frequency));
				printf("### standard : 0x%x.\n", std);
				if ((std & V4L2_COLOR_STD_PAL) == V4L2_COLOR_STD_PAL) {
					video_std = 0;
				} else if ((std & V4L2_COLOR_STD_NTSC) == V4L2_COLOR_STD_NTSC) {
					video_std = 1;
				} else if ((std & V4L2_COLOR_STD_SECAM) == V4L2_COLOR_STD_SECAM) {
					video_std = 2;
				} else {
					video_std = 3;
					printf("!!! color std ERROR !!!.\n");
				}

				printf("### color std: %s.\n", str_vstd[video_std]);

				if (((std & V4L2_STD_PAL_DK) == V4L2_STD_PAL_DK)
						|| ((std & V4L2_STD_SECAM_DK) == V4L2_STD_SECAM_DK)) {
					audio_std = 0;
				} else if ((std & V4L2_STD_PAL_I) == V4L2_STD_PAL_I) {
					audio_std = 1;
				} else if ((std & V4L2_STD_PAL_BG) == V4L2_STD_PAL_BG) {
					audio_std = 2;
				} else if (((std & V4L2_STD_PAL_M) == V4L2_STD_PAL_M)
						|| ((std & V4L2_STD_NTSC_M) == V4L2_STD_NTSC_M)) {
					audio_std = 3;
				} else if ((std & V4L2_STD_SECAM_L) == V4L2_STD_SECAM_L) {
					audio_std = 4;
				} else if ((std & V4L2_STD_SECAM_LC) == V4L2_STD_SECAM_LC) {
					audio_std = 5;
				} else {
					audio_std = 6;
					printf("!!! audio std ERROR !!!\n");
				}

				printf("### audio std: %s.\n", str_astd[audio_std]);

				if ((std & V4L2_STD_PAL_I) == V4L2_STD_PAL_I) {
					cvbs = 0;
				} else if ((std & V4L2_STD_PAL_M) == V4L2_STD_PAL_M) {
					cvbs = 1;
				} else if ((std & V4L2_STD_PAL_Nc) == V4L2_STD_PAL_Nc) {
					cvbs = 2;
				} else if ((std & V4L2_STD_PAL_60) == V4L2_STD_PAL_60) {
					cvbs = 3;
				} else if ((std & V4L2_STD_NTSC_M) == V4L2_STD_NTSC_M) {
					cvbs = 4;
				} else if ((std & V4L2_STD_NTSC_443) == V4L2_STD_NTSC_443) {
					cvbs = 5;
				} else if ((std & V4L2_STD_SECAM_L) == V4L2_STD_SECAM_L) {
					cvbs = 6;
				} else {
					cvbs = 7;
					printf("# cvbs std ERROR !!!\n");
				}

				printf("### cvbs std : %s.\n", str_cvbs[cvbs]);

				ch[num].freq = vlfend_format_frequency(get_para.frequency);
				ch[num].std = std;
				ch[num].vstd = video_std;
				ch[num].astd = audio_std;
				ch[num].cvbs = cvbs;
				num++;

				freq += 6000000;
			} else {
				printf("!!! Get status: Unlocked.\n");
				freq += 3000000;
			}
		}

		pthread_mutex_unlock(&lock);

		printf("=== Find all channels:\n");

		number = num;
		for (i = 0; i < num; ++i) {
			printf("[channel %d] frequency: %d, system: %s-%s (cvbs %s).\n",
					i + 1, ch[i].freq,
					str_vstd[ch[i].vstd],
					str_astd[ch[i].astd],
					str_cvbs[ch[i].cvbs]);
		}
	}

	return 0;
}

static int vlfend_stat(void)
{
	printf("!!! Not implemented !!!\n");
	return 0;
}

static int vlfend_info(void)
{
	printf("!!! Not implemented !!!\n");
	return 0;
}

static int vlfend_tune(void)
{
	int i = 0;
	int num = 0;

	if (number > 0) {
		for (i = 0; i < number; ++i) {
			printf("[channel %d] freq: %d, system: %s-%s (%s).\n",
					i + 1, ch[i].freq,
					str_vstd[ch[i].vstd],
					str_astd[ch[i].astd],
					str_cvbs[ch[i].cvbs]);
		}

		printf("Input channel number to tune.\n");
		printf("Number: ");
		scanf("%d", &num);

		if (num > number || num < 1) {
			printf("!!! Input channel number ERROR !!!\n");
			return -1;
		}

		vlfend_play(ch[num - 1].freq, ch[num - 1].vstd,
				ch[num - 1].astd, ch[num - 1].cvbs);
	} else {
		printf("!!! No channel, please scan the channel first !!!\n");
	}

	return 0;
}

static void show_cmds(void)
{
	printf("\nCommands:\n");
	printf("\tlock\n");
	printf("\tstat\n");
	printf("\tinfo\n");
	printf("\ttune\n");
	printf("\tquit\n");
	printf(">");
	fflush(stdout);
}

static int open_close_device(int on)
{
	int ret = 0;
	const AM_FEND_OpenPara_t para = { .mode = FE_ANALOG };
	struct tvin_parm_s vdinParam;

	if (on) {
		if (AM_VLFEND_Open(VLFEND_DEV_NO, &para)) {
			printf("!!! Open atv module, error (%s).\n", strerror(errno));
			return -1;
		}
#if TVAFE_DEV_NEED
		fd_tvafe = open("/dev/tvafe0", O_RDWR);
		if (fd_tvafe < 0) {
			printf("!!! Open tvafe module, error (%s).\n", strerror(errno));
			return -1;
		}
#else
	printf("!!! Waring: Tvafe device is also required for ATV.\n");
#endif

#if VDIN_DEV_NEED
		fd_vdin = open("/dev/vdin0", O_RDWR);
		if (fd_vdin < 0) {
			printf("!!! Open vdin module, error (%s).\n", strerror(errno));
			close(fd_tvafe);
			fd_tvafe = -1;
			return -1;
		}

		vdinParam.port = 0x00001003;
		vdinParam.index = 0;

		ret = ioctl(fd_vdin, TVIN_IOC_STOP_DEC);
		ret = ioctl(fd_vdin, TVIN_IOC_OPEN, &vdinParam);
		if (ret) {
			printf("!!! Vdin TVIN_IOC_OPEN, error (%s).\n", strerror(errno));
			close(fd_tvafe);
			close(fd_vdin);
			return ret;
		}
#else
	printf("!!! Waring: Vdin device is also required for ATV.\n");
#endif
	} else {
#if VDIN_DEV_NEED
		ret = ioctl(fd_vdin, TVIN_IOC_STOP_DEC);
		ret = ioctl(fd_vdin, TVIN_IOC_CLOSE);
#endif

		AM_VLFEND_CloseEx(VLFEND_DEV_NO, AM_TRUE);

#if TVAFE_DEV_NEED
		if (fd_tvafe >= 0) {
			close(fd_tvafe);
			fd_tvafe = -1;
		}
#endif

#if VDIN_DEV_NEED
		if (fd_vdin >= 0) {
			close(fd_vdin);
			fd_vdin = -1;
		}
#endif
	}

	return 0;
}

int main(int argc, char **argv)
{
	char buf[64] = { 0 };
	char *cmd = NULL;

	if (open_close_device(1)) {
		printf("!!! Error: open_close_device error.\n");
		return -1;
	}

	while (1) {
		show_cmds();
		cmd = fgets(buf, sizeof(buf), stdin);

		if (!cmd) {
			printf("!!! Error: invalid input.\n");
			continue;
		}

		if (!strncmp(cmd, "lock", 4)) {
			vlfend_lock();
		} else if (!strncmp(cmd, "stat", 4)) {
			vlfend_stat();
		} else if (!strncmp(cmd, "info", 4)) {
			vlfend_info();
		} else if (!strncmp(cmd, "tune", 4)) {
			vlfend_tune();
		} else if (!strncmp(cmd, "quit", 4)) {
			break;
		} else {
			continue;
		}

		cmd = NULL;
	}

	open_close_device(0);

	return 0;
}
