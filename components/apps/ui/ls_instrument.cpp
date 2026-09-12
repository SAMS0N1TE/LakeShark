#include "ls_instrument.h"
#include "sdr_ui/sdr_ui.h"
#include "ui/ls_ui.h"
#include <cstdlib>
#include <algorithm>

static lv_color_t ink(){return lv_color_hex(0xD9DED9);}
static lv_color_t amber(){return lv_color_hex(0xDFB56B);}
lv_color_t ls_instrument_accent(){return amber();}
lv_color_t ls_instrument_ink(){return ink();}
void ls_instrument_select(lv_obj_t *button,bool selected)
{
    if(selected)lv_obj_add_state(button,LV_STATE_CHECKED);
    else lv_obj_clear_state(button,LV_STATE_CHECKED);
}
void ls_instrument_flat(lv_obj_t *o)
{
    lv_obj_remove_style_all(o);
    lv_obj_set_style_bg_color(o,lv_color_hex(0x080C10),0);
    lv_obj_set_style_bg_opa(o,LV_OPA_COVER,0);
    lv_obj_set_style_text_color(o,ink(),0);
    lv_obj_set_style_text_font(o,sdr_font_mono(),0);
    lv_obj_clear_flag(o,LV_OBJ_FLAG_SCROLLABLE);
}
static void place(lv_obj_t *o,int x,int y,int w,int h)
{
    lv_obj_set_pos(o,x,y);lv_obj_set_size(o,std::max(1,w),std::max(1,h));
}
static void fit(lv_event_t *e)
{
    auto *v=static_cast<ls_instrument_t *>(lv_event_get_user_data(e));
    if(lv_event_get_code(e)==LV_EVENT_DELETE){free(v);return;}
    int w=lv_obj_get_content_width(v->root),h=lv_obj_get_content_height(v->root);
    bool wide=w>=800;
    int footer=wide?v->landscape_footer:v->portrait_footer;
    int header=LS_HAS_COMPACT_UI?4:52;
    int body=std::max(80,h-header-4-footer);
    place(v->heading,12,4,w-200,40);place(v->index,w-180,18,164,24);
    if(wide){
        int split=(w*2/5)/20*20;
        place(v->left,12,header,split-24,body);
        place(v->right,split+12,header,w-split-24,body);
    }else{
        int top=std::min(v->portrait_left,body/2);
        place(v->left,12,header,w-24,top);
        place(v->right,12,header+8+top,w-24,body-top-8);
    }
    place(v->footer,12,h-footer,w-24,footer);
}
ls_instrument_t *ls_instrument_create(lv_obj_t *parent,const char *title,
    const char *index,int portrait_left,int portrait_footer,int landscape_footer)
{
    auto *v=static_cast<ls_instrument_t *>(calloc(1,sizeof(ls_instrument_t)));
    if(!v)return nullptr;
    lv_obj_set_style_pad_all(parent,0,0);
    v->root=lv_obj_create(parent);ls_instrument_flat(v->root);
    lv_obj_set_size(v->root,lv_pct(100),lv_pct(100));
    v->portrait_left=portrait_left;v->portrait_footer=portrait_footer;
    v->landscape_footer=landscape_footer;
    v->heading=ls_instrument_text(v->root,title,true);
    v->index=ls_instrument_text(v->root,index);
    if(LS_HAS_COMPACT_UI){lv_obj_add_flag(v->heading,LV_OBJ_FLAG_HIDDEN);lv_obj_add_flag(v->index,LV_OBJ_FLAG_HIDDEN);}
    lv_obj_set_style_text_color(v->index,amber(),0);
    v->left=lv_obj_create(v->root);ls_instrument_flat(v->left);
    v->right=lv_obj_create(v->root);ls_instrument_flat(v->right);
    v->footer=lv_obj_create(v->root);ls_instrument_flat(v->footer);
    lv_obj_add_event_cb(v->root,fit,LV_EVENT_SIZE_CHANGED,v);
    lv_obj_add_event_cb(v->root,fit,LV_EVENT_DELETE,v);
    lv_obj_update_layout(v->root);
    lv_event_send(v->root,LV_EVENT_SIZE_CHANGED,nullptr);
    return v;
}
lv_obj_t *ls_instrument_text(lv_obj_t *parent,const char *text,bool large)
{
    auto *o=lv_label_create(parent);
    lv_obj_set_style_text_font(o,large?&lv_font_montserrat_32:sdr_font_mono(),0);
    lv_obj_set_style_text_color(o,ink(),0);
    lv_label_set_long_mode(o,LV_LABEL_LONG_DOT);
    lv_label_set_text(o,text);
    return o;
}
lv_obj_t *ls_instrument_button(lv_obj_t *parent,const char *text,
    lv_event_cb_t callback,void *context,lv_obj_t **label)
{
    auto *b=lv_btn_create(parent);ls_instrument_flat(b);
    sdr_button_guard_swipe(b);
    lv_obj_set_height(b,40);
    lv_obj_set_style_border_side(b,LV_BORDER_SIDE_BOTTOM,0);
    lv_obj_set_style_border_width(b,1,0);
    lv_obj_set_style_border_color(b,lv_color_hex(0x28333B),0);
    lv_obj_set_style_bg_color(b,lv_color_hex(0x253038),LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(b,lv_color_hex(0x282418),LV_STATE_CHECKED);
    lv_obj_set_style_border_color(b,amber(),LV_STATE_CHECKED);
    lv_obj_set_style_outline_color(b,amber(),LV_STATE_FOCUSED);
    lv_obj_set_style_outline_width(b,2,LV_STATE_FOCUSED);
    auto *l=ls_instrument_text(b,text);lv_obj_set_width(l,lv_pct(100));
    lv_obj_align(l,LV_ALIGN_LEFT_MID,8,0);
    lv_obj_add_event_cb(b,[](lv_event_t *e){
        auto *p=lv_event_get_target(e);auto *label=lv_obj_get_child(p,0);
        lv_obj_set_width(label,std::max(1,(int)lv_obj_get_content_width(p)-16));
    },LV_EVENT_SIZE_CHANGED,nullptr);
    if(callback)lv_obj_add_event_cb(b,callback,LV_EVENT_CLICKED,context);
    if(label)*label=l;
    return b;
}
static void layout_list(lv_obj_t *p,uintptr_t config)
{
    int columns=config&255,row=config>>8,w=lv_obj_get_content_width(p);
    for(uint32_t i=0;i<lv_obj_get_child_cnt(p);i++)
        place(lv_obj_get_child(p,i),(i%columns)*(w/columns),(i/columns)*row,w/columns-8,row-4);
}
static void fit_list(lv_event_t *e)
{
    layout_list(lv_event_get_target(e),reinterpret_cast<uintptr_t>(lv_event_get_user_data(e)));
}
void ls_instrument_list(lv_obj_t *parent,int columns,int row_height)
{
    lv_obj_remove_event_cb(parent,fit_list);
    lv_obj_set_layout(parent,0);
    lv_obj_add_event_cb(parent,fit_list,LV_EVENT_SIZE_CHANGED,
        reinterpret_cast<void *>(static_cast<uintptr_t>((row_height<<8)|columns)));
    layout_list(parent,(row_height<<8)|columns);
}
