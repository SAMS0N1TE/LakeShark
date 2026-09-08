#ifndef LS_TEXT_ENTRY_H
#define LS_TEXT_ENTRY_H

#include <stdbool.h>
#include <stddef.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ls_text_entry ls_text_entry_t;

typedef enum {
    LS_TEXT_ENTRY_TEXT,
    LS_TEXT_ENTRY_NUMBER,
} ls_text_entry_mode_t;

typedef struct {
    const char *title;
    const char *text;
    const char *placeholder;
    const char *accepted_chars;
    size_t max_length;
    lv_coord_t width;
    ls_text_entry_mode_t mode;
    bool large;
    bool password;
} ls_text_entry_config_t;

typedef void (*ls_text_entry_done_cb_t)(bool accepted, const char *text,
                                        void *user_data);

/* The only app-facing text entry point.  Touch-only boards get the existing
   on-screen LVGL keyboard; physical-keyboard boards type into the same text
   area through the keypad indev. */
ls_text_entry_t *ls_text_entry_open(const ls_text_entry_config_t *config,
                                    ls_text_entry_done_cb_t done,
                                    void *user_data);
void ls_text_entry_close(ls_text_entry_t *entry);

#ifdef __cplusplus
}
#endif

#endif
