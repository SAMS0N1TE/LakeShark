/* SPDX-License-Identifier: GPL-3.0-or-later
   LakeShark original. Not librtlsdr - see UPSTREAM.md in this
   directory for which files here are third-party and which are ours. */

#ifndef USB_HOST_H
#define USB_HOST_H

#ifdef __cplusplus
extern "C" {
#endif

void class_driver_task(void *arg);
void class_driver_client_deregister(void);

#ifdef __cplusplus
}
#endif

#endif
