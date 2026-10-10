/* LS_TEST_SOURCES: none; it runs the lssim this build made on MESH */
/* MESH taps and keys driven through the real screen, router and keyboard.
   The simulator's mesh stub prints every send and save on stderr as
   "mesh-stub ...", so a case can see what a tap would have put on the air. */
#include "ls_test.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define popen  _popen
#define pclose _pclose
#endif

static char s_out[64 * 1024];

/* Runs lssim on MESH with these options, the grid dumped, and keeps what it
   printed. `last` ends the stub's feed with a private message ("dm" or
   "chan"), or NULL for the plain fixture. */
static void run(const char *opts, const char *last)
{
    s_out[0] = 0;
#ifdef _WIN32
    char env[64];
    snprintf(env, sizeof(env), "LSSIM_MESH_LAST=%s", last ? last : "");
    _putenv(env);
#else
    if (last) setenv("LSSIM_MESH_LAST", last, 1);
    else      unsetenv("LSSIM_MESH_LAST");
#endif
    char cmd[700];
    snprintf(cmd, sizeof(cmd), "%s mesh -d %s 2>&1",
             LSSIM_EXE, opts);
#ifdef _WIN32
    for (char *c = cmd; *c && *c != ' '; c++) if (*c == '/') *c = '\\';
#endif
    FILE *p = popen(cmd, "r");
    LS_CHECK_MSG(p != NULL, "could not run %s", cmd);
    if (!p) return;
    size_t n = 0;
    char line[512];
    while (fgets(line, sizeof(line), p)) {
        const size_t l = strlen(line);
        if (n + l + 1 < sizeof(s_out)) { memcpy(s_out + n, line, l); n += l; }
    }
    s_out[n] = 0;
    const int rc = pclose(p);
    LS_CHECK_MSG(rc == 0, "lssim failed (%d) for: %s", rc, opts);
}

static bool said(const char *what) { return strstr(s_out, what) != NULL; }

LS_CASE(save_hands_the_backend_the_whole_key)
{
    /* NODES, the second node (KB1QWE, the one with a key), its page, SAVE. */
    run("-x ~2,@right,@down,@enter,+s", NULL);
    LS_CHECK_MSG(said("mesh-stub add_contact 17BC5E90A3D46F82114C09E75A30D86B"
                      "F2289140CD773EA506BB1F9462E08D53 ok"),
                 "SAVE did not pass the 64-character key:\n%s", s_out);
}

LS_CASE(a_tap_on_the_portrait_feed_never_sends_the_draft)
{
    run("-x ~2,+hello,20:40,~2", NULL);
    LS_CHECK_MSG(!said("mesh-stub send"), "a stray tap sent:\n%s", s_out);
}

LS_CASE(a_tap_on_the_landscape_feed_never_sends_the_draft)
{
    run("-l -x ~2,+hello,40:15,~2", NULL);
    LS_CHECK_MSG(!said("mesh-stub send"), "a stray tap sent:\n%s", s_out);
}

LS_CASE(landscape_tabs_answer_only_on_their_own_row)
{
    /* NODES, then a tap in the CHAT tab's columns high in the node list. */
    run("-l -x ~2,@right,3:20,~2", NULL);
    LS_CHECK_MSG(said("NODES IN RANGE"), "the tap changed page:\n%s", s_out);
}

LS_CASE(landscape_tap_on_the_name_leaves_the_dm)
{
    /* Armed, so the chip is drawn: lock the chat to KB1QWE, tap its chip,
       then send. */
    run("-l -x ~2,@mic,@right,@down,@enter,+d,~2,2:27,~2,+hi,@enter", NULL);
    LS_CHECK_MSG(said("mesh-stub send_on 0 hi"),
                 "the chip tap did not return to the channel:\n%s", s_out);
}

LS_CASE(a_reply_to_a_dm_goes_back_to_its_sender_and_the_row_says_dm)
{
    run("-x ~2,@mic,+ok,@enter", "dm");
    LS_CHECK_MSG(said("mesh-stub send_dm 17BC5E90A3D46F82 ok"),
                 "the reply did not go to the DM's sender:\n%s", s_out);
    LS_CHECK_MSG(said("<@KB1QWE"), "the DM row is not marked:\n%s", s_out);
}

LS_CASE(a_reply_on_a_private_channel_stays_on_it_and_the_row_says_so)
{
    run("-x ~2,@mic,+ok,@enter", "chan");
    LS_CHECK_MSG(said("mesh-stub send_on 1 ok"),
                 "the reply did not stay on the private channel:\n%s", s_out);
    LS_CHECK_MSG(said("<#TILTON ROOM"), "the channel row is not marked:\n%s", s_out);
    LS_CHECK_MSG(said("LAKE CHANNEL"), "the box does not name the channel:\n%s", s_out);
}

LS_CASE(the_public_fixture_still_replies_in_public)
{
    run("-x ~2,@mic,+ok,@enter", NULL);
    LS_CHECK_MSG(said("mesh-stub send_on 0 ok"), "public reply moved:\n%s", s_out);
    LS_CHECK_MSG(said("PUBLIC CHANNEL"), "the box lost its title:\n%s", s_out);
}

LS_CASE(portrait_send_refusal_is_visible_and_draft_survives_offline)
{
    run("-x ~2,+hello,@enter", NULL);
    LS_CHECK_MSG(said("disarmed - type /arm"), "refusal invisible:\n%s", s_out);
    run("-l -x ~2,@mic,+/stop,@enter,+lostdraft,@enter,+/start", NULL);
    LS_CHECK(!said("mesh-stub send"));
    LS_CHECK_MSG(said("draft kept"), "offline refusal invisible:\n%s", s_out);
    /* Start via the node shortcut, return and send the retained draft. */
    run("-x ~2,@mic,+/stop,@enter,+lostdraft,@enter,@right,+s,+t,@left,@enter", NULL);
    LS_CHECK_MSG(said("mesh-stub send_on 0 lostdraft"), "draft lost:\n%s", s_out);
}

LS_CASE(help_lists_commands_without_flash_truncation)
{
    run("-x ~2,+/help,@enter", NULL);
    LS_CHECK(said("MESH HELP")); LS_CHECK(said("/name X")); LS_CHECK(said("/pub"));
}

LS_CASE(node_advert_refusal_and_stop_failure_are_honest)
{
    run("-x ~2,@right,+a", NULL);
    LS_CHECK_MSG(said("advert refused"), "advert refusal invisible:\n%s", s_out);
    LS_CHECK(!said("advert queued"));
#ifdef _WIN32
    _putenv("LSSIM_MESH_STOP_FAIL=1");
#else
    setenv("LSSIM_MESH_STOP_FAIL", "1", 1);
#endif
    run("-x ~2,+/stop,@enter", NULL);
    LS_CHECK_MSG(said("stop failed"), "false STOP result:\n%s", s_out);
    LS_CHECK(!said("stopped"));
#ifdef _WIN32
    _putenv("LSSIM_MESH_STOP_FAIL=");
#else
    unsetenv("LSSIM_MESH_STOP_FAIL");
#endif
}

LS_CASE(wrapped_feed_keeps_the_end_and_history_scrolls)
{
    run("-x ~2,@up", NULL);
    LS_CHECK_MSG(said("NORTHFIELD"), "history unavailable:\n%s", s_out);
    LS_CHECK_MSG(said("hill again"), "message tail clipped:\n%s", s_out);
}

LS_CASE(options_result_is_above_the_list_and_numeric_fields_use_keypad)
{
    /* Root MESH run/stop action posts a result over the picker. */
    run("-x ~2,43:8,@enter", NULL);
    LS_CHECK_MSG(said("MESH RESULT"), "OPTIONS result invisible:\n%s", s_out);
    run("-x ~2,+/radio,@enter,@enter,@down,@enter", NULL);
    LS_CHECK_MSG(said("MHz"), "numeric editor lost its unit:\n%s", s_out);
    LS_CHECK_MSG(!said("Q W E R T Y"), "numeric field used QWERTY:\n%s", s_out);
}

LS_CASE(protocol_length_feed_wraps_in_both_postures_and_ack_has_its_own_mark)
{
    run("-x ~2", "long"); LS_CHECK_MSG(said("FINALTAIL"), "portrait lost tail:\n%s", s_out);
    LS_CHECK(said("+@SHARK"));
    run("-l -x ~2", "long"); LS_CHECK_MSG(said("FINALTAIL"), "landscape lost tail:\n%s", s_out);
}
