#include "ls_test.h"
#include "../../components/apps/tui/screens/ext/scr_map.c"
static bool armed;
static int sends;
static char picker_note[96];
static bool picker_open;
static char sent[LS_SHARE_PARTS][LS_SHARE_TEXT+1];
static char sent_id[17];
static int sent_chan;
static double latitude,longitude;
static ls_notice_t last_notice;
bool ls_mesh_tx_enabled(void) { return armed; }
esp_err_t ls_mesh_send_dm_id(const char *id,const char *text) {
    strcpy(sent_id,id);snprintf(sent[sends++],sizeof(sent[0]),"%s",text);return ESP_OK;
}
esp_err_t ls_mesh_send_text_on(int chan,const char *text) {
    sent_chan=chan;snprintf(sent[sends++],sizeof(sent[0]),"%s",text);return ESP_OK;
}
void ls_notify_post(const ls_notice_t *n) { last_notice=*n; }
void ls_tui_invalidate(void) {}
void ls_tui_screen_set_bare(bool on) { (void)on; }
void ls_map_get_center(double *lat,double *lon) { *lat=latitude;*lon=longitude; }
void ls_map_center(double lat,double lon) { latitude=lat;longitude=lon; }
void ls_map_follow_set(bool b) { (void)b; }
void settings_set_map_layers(uint32_t bits) { (void)bits; }
int ls_tui_screen_index_of(const ls_tui_screen_t *s) { (void)s;return 5; }
void ls_picker_open(const char *s,ls_picker_done_t cb) { (void)s;(void)cb;picker_open=true; }
bool ls_picker_add(const char *a,const char *b) { (void)a;(void)b;return true; }
void ls_picker_back(void (*cb)(void)) { (void)cb; }
void ls_picker_navigation(void (*cb)(int,int)) { (void)cb; }
void ls_picker_note(const char *s) { snprintf(picker_note,sizeof(picker_note),"%s",s); }
void ls_picker_keep_open(void) {}
void ls_picker_close(void) { picker_open=false; }
void ls_picker_empty_reason(const char *s) { (void)s; }
bool ls_mesh_peer_at(int i,ls_mesh_peer_t *p) {
    if(i) return false;
    memset(p,0,sizeof(*p));strcpy(p->id,"1234567890abcdef");strcpy(p->name,"KB1QWE");return true;
}
int ls_mesh_channels(ls_mesh_chan_t *p,int max) { (void)max;memset(p,0,sizeof(*p));p->fixed=true;strcpy(p->name,"Public");return 1; }
static void reset(void) {
    remove("share_ui.txt");remove("share_ui.txt.bak");remove("share_ui.txt.new");
    ls_marks_use_file("share_ui.txt");memset(&s_share_rx,0,sizeof(s_share_rx));
    select_none();ls_sketch_cancel();picker_note[0]=0;picker_open=false;
    sends=0;s_send_part=0;armed=false;memset(&last_notice,0,sizeof(last_notice));
}
LS_CASE(map_send_requires_destination_and_rechecks_disarm_between_parts) {
    reset();int i=ls_marks_add(43.12345,-71.54321,0,"Camp");select_mark(SEL_MARK,i);
    send_selected();LS_EQ_INT(s_send_part,0);LS_EQ_INT(sends,0);
    LS_CHECK(picker_open);LS_CHECK(strstr(picker_note,"disarmed"));
    destination_pick(0);LS_CHECK(picker_open);LS_EQ_INT(sends,0);
    armed=true;send_selected();LS_EQ_INT(sends,0);LS_EQ_INT(s_ndest,2);
    destination_pick(0);ls_scr_map_share_poll();LS_EQ_INT(sends,1);
    LS_EQ_STR(sent_id,"1234567890abcdef");LS_CHECK(strstr(sent[0],"WPT Camp"));
    ls_sketch_begin();for(int k=0;k<12;k++) ls_sketch_add_point(43+k*.00001,-71);
    i=ls_sketch_finish();select_mark(SEL_LINE,i);send_selected();destination_pick(1);
    ls_scr_map_share_poll();LS_EQ_INT(sends,2);LS_EQ_INT(sent_chan,0);
    LS_CHECK(strstr(sent[1],"1/3]"));armed=false;s_send_after=0;ls_scr_map_share_poll();
    LS_EQ_INT(sends,2);LS_EQ_INT(s_send_part,0);LS_CHECK(strstr(last_notice.body,"disarmed"));
    reset();
}
LS_CASE(map_channel_and_dm_receipts_open_exact_item_without_sending) {
    reset();ls_mesh_msg_t m={.direct=false,.chan=2};ls_notice_t notice={0};
    strcpy(m.text,"KB1QWE: WPT Camp [LS1 1234abcd 1/1] 43.12345,-71.54321");
    LS_CHECK(ls_scr_map_receive(&m,&notice));LS_EQ_INT(sends,0);LS_CHECK(notice.open!=NULL);
    LS_CHECK(strstr(notice.body,"KB1QWE sent a marker"));LS_EQ_INT(notice.screen,5);
    notice.open(notice.item);LS_NEAR(latitude,43.12345,.000001);LS_NEAR(longitude,-71.54321,.000001);
    LS_EQ_INT(s_sel,SEL_MARK);LS_CHECK(ls_marks_at(s_sel_index)->origin.received);
    LS_CHECK(!ls_scr_map_receive(&m,&notice));
    m.direct=true;strcpy(m.peer,"1234567890abcdef");strcpy(m.sender,"KB1ABC");
    strcpy(m.text,"WPT Other [LS1 1234abcd 1/1] 44.00000,-72.00000");
    LS_CHECK(ls_scr_map_receive(&m,&notice));notice.open(notice.item);LS_NEAR(latitude,44,0);
    LS_EQ_STR(ls_marks_at(s_sel_index)->origin.sender,"KB1ABC");LS_EQ_INT(sends,0);
    reset();
}

static int cross_x,cross_y,cross_count,target_cells;
static uint8_t target_centre_attr;
uint8_t ls_ink_tinted(uint8_t a) { return a; }
void ls_ink_text(tui_surface *sf, tui_rect clip, int x, int y, const char *s, int n, uint8_t attr)
{
    (void)sf;(void)clip;
    for(int i=0;i<n;i++) {
        target_cells++;
        if(s[i]=='+') { cross_x=x+i;cross_y=y;cross_count++;target_centre_attr=attr; }
    }
}
bool ls_globe_flat(tui_surface *sf, tui_rect a, double lat, double lon, unsigned z, double ppx, double ppy,
                   ls_globe_fill_fn fill, void *ctx)
{ (void)sf; (void)a; (void)lat; (void)lon; (void)z; (void)ppx; (void)ppy; (void)fill; (void)ctx; return false; }
bool ls_globe_nearest(double lat, double lon, ls_globe_fill_fn fill, void *ctx, char code[8], float *rlat, float *rlon,
                      bool *inside)
{ (void)lat; (void)lon; (void)fill; (void)ctx; (void)code; (void)rlat; (void)rlon; (void)inside; return false; }
void tui_put_char(tui_surface *sf,tui_rect clip,int x,int y,char ch,uint8_t attr) {
    (void)sf;(void)clip;(void)attr;
    target_cells++;
    if(ch=='+') { cross_x=x;cross_y=y;cross_count++;target_centre_attr=attr; }
}
LS_CASE(marker_target_is_visible_with_controls_hidden_and_delete_confirms) {
    reset();latitude=43.12345;longitude=-71.54321;
    s_controls_hidden=true;cross_count=0;target_cells=0;
    centre_reticle(NULL,(tui_rect){2,3,52,60});
    LS_EQ_INT(target_cells,7);LS_EQ_INT(target_centre_attr,TUI_ATTR(TUI_YELLOW|TUI_BRIGHT,TUI_BLACK));
    LS_EQ_INT(cross_count,1);LS_EQ_INT(cross_x,28);LS_EQ_INT(cross_y,33);
    ls_val_t result;a_map_mark(NULL,&result);LS_EQ_INT(ls_marks_count(),1);
    LS_NEAR(ls_marks_at(0)->lat,latitude,0);LS_NEAR(ls_marks_at(0)->lon,longitude,0);
    s_edit_kind=SEL_MARK;s_edit_index=0;rename_done("CAMP");
    ls_marks_reload();LS_EQ_STR(ls_marks_at(0)->name,"CAMP");
    confirm_delete(false);LS_EQ_INT(ls_marks_count(),1);
    delete_confirm(-1);LS_EQ_INT(ls_marks_count(),1);
    delete_confirm(0);ls_marks_reload();LS_EQ_INT(ls_marks_count(),0);
    reset();
}
LS_CASE(three_part_route_queues_in_order_and_received_route_opens_start) {
    reset();armed=true;
    ls_sketch_begin();for(int k=0;k<9;k++) ls_sketch_add_point(43+k*.00001,-71-k*.00001);
    int i=ls_sketch_finish();select_mark(SEL_LINE,i);send_selected();destination_pick(1);
    for(int p=0;p<3;p++) { s_send_after=0;ls_scr_map_share_poll(); }
    LS_EQ_INT(sends,3);LS_EQ_INT(s_send_part,0);
    LS_CHECK(strstr(last_notice.body,"Queued 3/3"));
    ls_mesh_msg_t m={.direct=true};strcpy(m.peer,"1234567890abcdef");strcpy(m.sender,"KB1QWE");
    ls_notice_t n={0};
    strcpy(m.text,sent[2]);LS_CHECK(!ls_scr_map_receive(&m,&n));
    strcpy(m.text,sent[0]);LS_CHECK(!ls_scr_map_receive(&m,&n));
    strcpy(m.text,sent[1]);LS_CHECK(ls_scr_map_receive(&m,&n));
    LS_CHECK(strstr(n.body,"sent a route"));n.open(n.item);
    LS_EQ_INT(s_sel,SEL_LINE);LS_EQ_INT(ls_sketch_at(s_sel_index)->n,9);
    LS_NEAR(latitude,43,0);LS_NEAR(longitude,-71,0);LS_EQ_INT(sends,3);
    reset();
}

void ls_keyboard_open(const char *title,const char *initial,int max,void (*done)(const char *)) {
    (void)title;(void)initial;(void)max;(void)done;
}
const adsb_aircraft_t *adsb_state_get(int slot) { (void)slot;return NULL; }
static void command(const char *cmd) {
    ls_args_t args={.n=1,.v={{.kind=LS_VAL_TEXT,.s=cmd}}};ls_val_t out={0};
    a_map_context(&args,&out);
}
static const ls_quick_t *button(const char *label) {
    int n;const ls_quick_t *q=quick_table(&n);
    for(int i=0;i<n;i++) if(!strcmp(q[i].label,label)) return &q[i];
    return NULL;
}
LS_CASE(context_bar_tracks_selection_draw_and_received_states) {
    reset();LS_CHECK(button("MARK"));LS_CHECK(button("SELECT"));
    int a=ls_marks_add(43,-71,0,"A"),b=ls_marks_add(44,-72,0,"B");
    select_mark(SEL_MARK,a);LS_CHECK(button("MOVE HERE"));LS_CHECK(button("SEND"));
    command("v");LS_CHECK(button("SURE?")->danger);LS_EQ_INT(ls_marks_count(),2);
    command("j");LS_EQ_INT(s_sel_index,b);LS_CHECK(!s_delete_armed);
    command("p");LS_EQ_INT(s_sel_index,a);
    command("v");command("v");LS_EQ_INT(ls_marks_count(),1);LS_EQ_INT(s_sel,SEL_NONE);
    ls_sketch_begin();LS_CHECK(button("ADD POINT"));LS_CHECK(button("UNDO"));
    LS_CHECK(button("FINISH"));LS_CHECK(button("CANCEL"));LS_CHECK(button("ZOOM+"));
    ls_sketch_add_point(43,-71);ls_sketch_add_point(44,-72);
    int line=ls_sketch_finish();select_mark(SEL_LINE,line);
    LS_CHECK(button("RENAME"));LS_CHECK(!button("MOVE HERE"));
    command("q");select_node("1234567890abcdef");
    LS_CHECK(button("CENTRE"));LS_CHECK(button("MESSAGE"));LS_CHECK(button("DONE"));
    ls_mesh_msg_t m={.direct=true};ls_notice_t notice={0};
    strcpy(m.peer,"1234567890abcdef");strcpy(m.text,"WPT CAMP [LS1 4321abcd 1/1] 43.12345,-71.54321");
    LS_CHECK(ls_scr_map_receive(&m,&notice));notice.open(notice.item);
    LS_CHECK(button("SAVE"));LS_CHECK(button("DELETE"));LS_CHECK(button("DONE"));
    LS_CHECK(!button("SEND"));command("w");LS_CHECK(button("SEND"));LS_CHECK(!button("SAVE"));
    reset();
}

static bool finger;
static int finger_sc,finger_sr,finger_c,finger_r,panned_x,panned_y;
bool ls_tui_touch_held(int *sc,int *sr,int *c,int *r) {
    if(!finger) return false;
    *sc=finger_sc;*sr=finger_sr;*c=finger_c;*r=finger_r;return true;
}
bool ls_tui_touch_px(int *x,int *y,int *x0,int *y0,int64_t *since) {
    *x=finger_c*10+5;*y=finger_r*17+8;
    *x0=finger_sc*10+5;*y0=finger_sr*17+8;*since=1;return finger;
}
int ls_map_tile_px(void) { return 256; }
uint32_t ls_tui_touch_drags(int *a,int *b,int *c,int *d) { if(a)*a=0;if(b)*b=0;if(c)*c=0;if(d)*d=0;return 0; }
void ls_map_pan(double dx,double dy) { panned_x+=dx;panned_y+=dy; }
void ls_tui_geometry(int *cols,int *rows,int *cw,int *ch) {
    if(cols) *cols=52;
    if(rows) *rows=67;
    if(cw) *cw=10;
    if(ch) *ch=17;
}
LS_CASE(map_drag_pans_with_the_finger_and_never_selects) {
    reset();s_map_cells=(tui_rect){0,10,52,36};panned_x=panned_y=0;
    finger=true;finger_sc=20;finger_sr=20;finger_c=21;finger_r=21;
    drag_pan();LS_CHECK(!s_dragging);LS_EQ_INT(panned_x,0);      /* a wobble is still a tap */
    finger_c=24;finger_r=22;drag_pan();LS_CHECK(s_dragging);
    LS_EQ_INT(panned_x,-4*SUB_X);LS_EQ_INT(panned_y,-2*sub_y()); /* the map follows the finger */
    finger_c=23;drag_pan();LS_EQ_INT(panned_x,-3*SUB_X);
    finger=false;LS_CHECK(s_dragging);                           /* the release is swallowed */
    drag_pan();LS_CHECK(!s_dragging);
    finger=true;finger_sc=finger_c=5;finger_sr=finger_r=5;      /* above the map: not a drag */
    finger_c=15;drag_pan();LS_CHECK(!s_dragging);finger=false;drag_pan();
    reset();
}
LS_CASE(target_steps_around_a_symbol_and_hides_under_one) {
    reset();const tui_rect body={2,3,52,60};
    s_ntaken=0;box_take_air(24,24,30,1,0);target_cells=cross_count=0;
    centre_reticle(NULL,body);LS_EQ_INT(target_cells,6);LS_EQ_INT(cross_count,1);
    s_ntaken=0;box_take_air(25,27,30,1,0);target_cells=cross_count=0;
    centre_reticle(NULL,body);LS_EQ_INT(target_cells,0);LS_EQ_INT(cross_count,0);
    s_ntaken=0;
}
