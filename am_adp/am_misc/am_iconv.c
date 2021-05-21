#ifdef _FORTIFY_SOURCE
#undef _FORTIFY_SOURCE
#endif
/***************************************************************************
 * Copyright (c) 2014 Amlogic, Inc. All rights reserved.
 *
 * This source code is subject to the terms and conditions defined in the
 * file 'LICENSE' which is part of this source code package.
 *
 * Description:
 */
/**\file
 * \brief iconv functions
 *
 * \author Gong Ke <ke.gong@amlogic.com>
 * \date 2014-03-18: create the document
 ***************************************************************************/

#define AM_DEBUG_LEVEL 1

#include <stdlib.h>
#include <am_iconv.h>
#include <am_debug.h>
#include <stdbool.h>


UConverter* (*am_ucnv_open_ptr)(const char *converterName, UErrorCode *err);
void (*am_ucnv_close_ptr)(UConverter * converter);
void (*am_ucnv_convertEx_ptr)(UConverter *targetCnv, UConverter *sourceCnv,
		char **target, const char *targetLimit,
		const char **source, const char *sourceLimit,
		UChar *pivotStart, UChar **pivotSource,
		UChar **pivotTarget, const UChar *pivotLimit,
		UBool reset, UBool flush,
		UErrorCode *pErrorCode);
void (*am_u_setDataDirectory_ptr)(const char *directory);
void (*am_u_init_ptr)(long *status);

#ifdef USE_VENDOR_ICU
bool actionFlag = false;
void am_first_action(void)
{
	if (!actionFlag) {
		setenv("ICU_DATA", "/vendor/usr/icu", 1);
		u_setDataDirectory("/vendor/usr/icu");
		long status = 0;
		u_init(&status);
		if (status > 0)
			AM_DEBUG(1, "icu init fail. [%ld]", status);
		actionFlag = true;
	}
}
#endif

void
am_ucnv_dlink(void)
{

}

