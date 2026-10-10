#include "ls_test.h"
#include "../../components/apps/tui/screens/ext/scr_tiles.c"
static int switches;
static bool accepted;
static const ls_app_t map_app={0};
bool ls_tiles_action(int action,const ls_tile_region *r) {
    (void)r;LS_EQ_INT(2,action);return accepted;
}
const ls_app_t *ls_app_by_id(const char *id) { LS_CHECK(!strcmp(id,"map"));return &map_app; }
int ls_tui_screen_index_of(const ls_tui_screen_t *screen) { (void)screen;return 1; }
void ls_tui_screen_show(int index) { LS_EQ_INT(1,index);switches++; }
LS_CASE(open_waits_for_worker_and_keeps_failures_visible) {
    catalog.count=1;accepted=true;opening=false;switches=0;page=1;
    map_open();LS_CHECK(opening && page==0 && switches==0);
    catalog.open_state=1;LS_CHECK(!map_open_done(true) && opening);
    catalog.open_state=3;strcpy(catalog.message,"Cannot open: invalid map / insufficient PSRAM");
    LS_CHECK(!map_open_done(false) && opening);
    LS_CHECK(!map_open_done(true) && !opening && switches==0);
    LS_CHECK(strstr(catalog.message,"Cannot open"));
    map_open();catalog.open_state=2;
    LS_CHECK(map_open_done(true) && switches==1 && !opening);
    LS_CHECK(!map_open_done(true) && switches==1);
    accepted=false;map_open();LS_CHECK(!opening && switches==1);
}
