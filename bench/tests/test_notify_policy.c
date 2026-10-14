#include "ls_test.h"
#define LS_NOTIFY_EXTERNAL_ALERT_HW 1
#include "../../components/apps/tui/ls_notify.c"

static int alerts, rings, vibrations;
void ls_tui_invalidate(void) {}
void ls_notify_alert_hw(bool ring, bool vibe)
{ alerts++; rings += ring; vibrations += vibe; }

LS_CASE(route_quiet_posts_and_same_flags_share_the_alert_policy)
{
    ls_notice_t n = { .screen = -1, .accent = TUI_CYAN };
    alerts = rings = vibrations = 0;
    ls_notify_clear();
    ls_notify_set_alerts(true, true);
    ls_notify_post_quiet(&n);
    LS_CHECK(ls_notify_showing());
    LS_EQ_INT(0, alerts);
    LS_EQ_INT(0, ls_notify_unread());
    n.quiet = true;
    ls_notify_post(&n);
    LS_EQ_INT(0, alerts);
    n.quiet = false;
    n.haptic_only = true;
    ls_notify_post(&n);
    LS_EQ_INT(0, rings);
    LS_EQ_INT(1, vibrations);
    n.haptic_only = false;
    n.accent = 0;
    ls_notify_post(&n);
    LS_EQ_INT(1, rings);
    LS_EQ_INT(2, vibrations);
    LS_EQ_INT(1, ls_notify_unread());
    ls_notify_set_alerts(false, false);
    ls_notify_post(&n);
    LS_EQ_INT(1, rings);
    LS_EQ_INT(2, vibrations);
    ls_notify_clear();
}

LS_CASE(sensor_haptics_respect_quiet_posts_and_alert_preferences)
{
    ls_notice_t n = { .screen = 3, .accent = TUI_YELLOW, .haptic_only = true };
    alerts = rings = vibrations = 0;
    ls_notify_clear();
    ls_notify_set_alerts(true, true);
    ls_notify_post_quiet(&n);
    LS_CHECK(ls_notify_showing());
    LS_EQ_INT(0, alerts);
    n.quiet = true;
    ls_notify_post(&n);
    LS_EQ_INT(0, alerts);
    n.quiet = false;
    ls_notify_post(&n);
    LS_EQ_INT(0, rings);
    LS_EQ_INT(1, vibrations);
    LS_EQ_INT(0, ls_notify_unread());
    ls_notify_set_alerts(true, false);
    ls_notify_post(&n);
    LS_EQ_INT(0, rings);
    LS_EQ_INT(1, vibrations);
    ls_notify_set_alerts(false, true);
    ls_notify_post(&n);
    LS_EQ_INT(0, rings);
    LS_EQ_INT(2, vibrations);
    ls_notify_set_alerts(true, true);
    ls_notify_clear();
}

static bool probe_pending;
static ls_notice_t probe_notice;
static bool policy_probe(ls_notice_t *out)
{
    if (!probe_pending) return false;
    *out = probe_notice;
    probe_pending = false;
    return true;
}

LS_CASE(visible_source_override_preserves_haptic_and_quiet_policy)
{
    alerts = rings = vibrations = 0;
    ls_notify_clear();
    ls_notify_set_alerts(true, true);
    ls_notify_add_probe(policy_probe);
    probe_notice = (ls_notice_t){ .screen = 3, .accent = TUI_YELLOW,
        .haptic_only = true };
    probe_pending = true;
    ls_notify_poll(3);
    LS_CHECK(!ls_notify_showing());
    LS_EQ_INT(0, alerts);
    probe_notice.notify_visible = true;
    probe_pending = true;
    ls_notify_poll(3);
    LS_CHECK(ls_notify_showing());
    LS_EQ_INT(0, rings);
    LS_EQ_INT(1, vibrations);
    probe_notice.quiet = true;
    probe_pending = true;
    ls_notify_poll(3);
    LS_EQ_INT(1, alerts);
    LS_EQ_INT(0, ls_notify_unread());
    ls_notify_clear();
}

static unsigned probed;
static bool probe0(ls_notice_t *out) { (void)out;probed|=1;return false; }
static bool probe1(ls_notice_t *out) { (void)out;probed|=2;return false; }
static bool probe2(ls_notice_t *out) { (void)out;probed|=4;return false; }
static bool probe3(ls_notice_t *out) { (void)out;probed|=8;return false; }
static bool probe4(ls_notice_t *out) { (void)out;probed|=16;return false; }
static bool map_save_probe(ls_notice_t *out) {
    probed|=32;snprintf(out->title,sizeof(out->title),"MAP");
    snprintf(out->body,sizeof(out->body),"NOT SAVED: retry in OPTIONS");
    out->screen=7;out->quiet=true;out->accent=TUI_YELLOW;return true;
}
LS_CASE(map_save_notice_survives_other_registered_sources_and_leaving_map) {
    s_probes=0;probed=0;alerts=0;ls_notify_clear();
    ls_notify_add_probe(probe0);ls_notify_add_probe(probe1);ls_notify_add_probe(probe2);
    ls_notify_add_probe(probe3);ls_notify_add_probe(probe4);ls_notify_add_probe(map_save_probe);
    ls_notify_poll(3);
    LS_EQ_UINT(63,probed);LS_CHECK(ls_notify_showing());LS_EQ_INT(7,s_now.screen);
    LS_CHECK(strstr(s_now.body,"NOT SAVED"));LS_EQ_INT(0,alerts);
    s_probes=0;ls_notify_clear();
}

static int opened_screen;
static uint32_t opened_item;
int ls_tui_screen_count(void) { return 10; }
void ls_tui_screen_show(int i) { opened_screen=i; }
static void open_received(uint32_t item) { LS_EQ_INT(opened_screen,5);opened_item=item; }
LS_CASE(map_notice_opens_exact_item_only_when_banner_is_accepted) {
    ls_notice_t n={.screen=5,.open=open_received,.item=123,.quiet=true};
    s_rect=(tui_rect){0,0,40,4};s_close=(tui_rect){38,0,2,4};
    opened_item=0;ls_notify_post(&n);LS_CHECK(ls_notify_touch(38,1));LS_EQ_UINT(opened_item,0);
    ls_notify_post(&n);LS_CHECK(ls_notify_key(LS_TK_ESC));LS_EQ_UINT(opened_item,0);
    ls_notify_post(&n);LS_CHECK(ls_notify_touch(5,1));LS_EQ_UINT(opened_item,123);
}
