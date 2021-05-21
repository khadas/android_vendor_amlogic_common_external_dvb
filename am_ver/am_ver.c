/*
* Copyright (c) 2014 Amlogic, Inc. All rights reserved.
*
* This source code is subject to the terms and conditions defined in the
* file 'LICENSE' which is part of this source code package. *
* Description:
*/
#include <stdio.h>
#include <string.h>

#include <am_ver.h>

static char gitversionstr[256] = "N/A";

static int version_info_init(void) {
    static int info_is_inited = 0;
    int dirty_num = 0;

    if (info_is_inited > 0) {
        return 0;
    }
    info_is_inited++;
    return 0;
}

const char *dvb_get_git_version_info(void) {
    version_info_init();
    return gitversionstr;
}

const char *dvb_get_last_chaned_time_info(void) {

    return " Unknow ";
}

const char *dvb_get_git_branch_info(void) {

    return " Unknow ";
}

const char *dvb_get_build_time_info(void) {

    return " Unknow ";
}

const char *dvb_get_build_name_info(void) {

    return " Unknow ";
}
