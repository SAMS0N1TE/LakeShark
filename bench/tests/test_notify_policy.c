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
