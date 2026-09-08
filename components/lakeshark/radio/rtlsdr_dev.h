/* SPDX-License-Identifier: GPL-3.0-or-later
   LakeShark original. Not librtlsdr - see UPSTREAM.md in this
   directory for which files here are third-party and which are ours. */

#ifndef RTLSDR_DEV_H
#define RTLSDR_DEV_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*LS-407*/
void rtlsdr_dev_teardown(void);

#ifdef __cplusplus
}
#endif

#endif
