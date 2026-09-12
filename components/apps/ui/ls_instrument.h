#pragma once
#include "lvgl.h"

/* Fixed-cell instrument surfaces. Owned by root; no application state here. */
struct ls_instrument_t {
    lv_obj_t *root,*heading,*index,*left,*right,*footer;
    int portrait_left,portrait_footer,landscape_footer;
};
ls_instrument_t *ls_instrument_create(lv_obj_t *parent,const char *title,
    const char *index,int portrait_left,int portrait_footer=40,int landscape_footer=40);
lv_obj_t *ls_instrument_text(lv_obj_t *parent,const char *text,bool large=false);
lv_obj_t *ls_instrument_button(lv_obj_t *parent,const char *text,
    lv_event_cb_t callback,void *context,lv_obj_t **label=nullptr);
void ls_instrument_list(lv_obj_t *parent,int columns,int row_height=44);
void ls_instrument_flat(lv_obj_t *object);
lv_color_t ls_instrument_accent(void);
lv_color_t ls_instrument_ink(void);
void ls_instrument_select(lv_obj_t *button,bool selected);
