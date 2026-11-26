/* SPDX-License-Identifier: GPL-3.0-or-later
 * 'tui cast': the panel framebuffer as JPEG frames over Wi-Fi, for
 * recording the screen on a PC (a receiver script on the PC side). The serial recorder
 * cannot carry pixel-heavy screens; this can.
 */
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The console's 'tui cast ...' (argv[0] is "tui", argv[1] is "cast"). */
int ls_cast_cmd(int argc, char **argv);

/* The UI task, right after a present: the framebuffer holds a whole frame
   now. clean: nothing changed on the glass this frame. Cheap when off. */
void ls_cast_presented(bool clean);

/* The panel driver: framebuffer rows [y0, y1) were just presented. */
void ls_cast_rows(int y0, int y1);

/* A client is connected and frames are going out. */
bool ls_cast_active(void);

#ifdef __cplusplus
}
#endif
