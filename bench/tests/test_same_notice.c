#include "ls_test.h"
#include "ls_notify.h"
#include "screens/main/fm_same_view.h"
#include "apps/fm/same_store.h"
#include <string.h>
#define LS_NOTIFY_EXTERNAL_ALERT_HW 1
#include "../../components/apps/tui/ls_notify.c"
#include "../../components/apps/tui/screens/main/fm_same_view.c"
const ls_tui_screen_t ls_scr_fm = {.name = "FM"};
static int rings, vibrations;
void ls_tui_invalidate(void) {}
int ls_tui_screen_index_of(const ls_tui_screen_t *s) { (void)s; return 2; }
void ls_notify_alert_hw(bool ring, bool vibe)
{ rings += ring; vibrations += vibe; }
static same_alert_t alert;
LS_CASE(same_notices_use_haptics_and_weekly_tests_are_quiet)
{
    same_options_t o = {.include_tests = true}; same_options_set(&o);
    ls_notify_set_alerts(true, true); fm_same_enter();
    LS_CHECK(same_parse("ZCZC-WXR-TOR-025017+0030-2801430-KBOX/NWS-", &alert));
    same_store_receive(&alert, NULL); ls_notify_poll(2);
    LS_CHECK(ls_notify_showing()); LS_EQ_INT(0, rings); LS_EQ_INT(1, vibrations);
    LS_CHECK(same_parse("ZCZC-WXR-RWT-025017+0015-2801430-KBOX/NWS-", &alert));
    same_store_receive(&alert, NULL); ls_notify_poll(-1);
    LS_EQ_INT(0, rings); LS_EQ_INT(1, vibrations);
    o.include_tests = false; same_options_set(&o); ls_notify_clear();
    same_store_receive(&alert, NULL); ls_notify_poll(-1);
    LS_EQ_INT(0, rings); LS_EQ_INT(1, vibrations);
}
