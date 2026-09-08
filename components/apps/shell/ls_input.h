#ifndef LS_INPUT_H
#define LS_INPUT_H

#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"
#include "shell/ls_input_gesture.h"

#ifdef __cplusplus
extern "C" {
#endif

void ls_input_init(void);
void ls_input_set_screen(lv_obj_t *screen);
void ls_input_refresh_focus(void);
void ls_input_focus_modal(lv_obj_t *modal);
void ls_input_focus_screen(void);

/* Board drivers submit ls_key_code_t or printable ASCII events from task
   context.  False means unsupported, unmapped, or queue full. */
bool ls_input_key_event(uint32_t key, bool pressed);


#ifdef __cplusplus
}
#endif

#endif
