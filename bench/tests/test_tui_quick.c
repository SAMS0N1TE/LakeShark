/* LS_TEST_SOURCES: ls_quick.c over the real action and value tables */
/* The quick-control panel, against real actions. */

#include "ls_test.h"

#include "ls_quick.h"
#include "ls_action.h"
#include "ls_value.h"

#include <string.h>

/* ---------------------------------------------------- the fake hardware -- */

static long  s_volume = 50;
static bool  s_rx_on;
static char  s_mode[16] = "listen";
static long  s_split = 50;

static bool v_volume(ls_val_t *o) { o->kind = LS_VAL_INT;  o->i = s_volume; return true; }
static bool v_rx(ls_val_t *o)     { o->kind = LS_VAL_BOOL; o->i = s_rx_on;  return true; }
static bool v_mode(ls_val_t *o)   { o->kind = LS_VAL_TEXT; o->s = s_mode;   return true; }
static bool v_split(ls_val_t *o)  { o->kind = LS_VAL_INT;  o->i = s_split;  return true; }

static int s_calls;

static ls_act_status_t a_volume(const ls_args_t *in, ls_val_t *out)
{
    (void)out;
    s_calls++;
    if (!in || in->n != 1) return LS_ACT_BADARG;
    s_volume = in->v[0].i;
    return LS_ACT_OK;
}

static ls_act_status_t a_rx(const ls_args_t *in, ls_val_t *out)
{
    (void)out;
    s_calls++;
    if (!in || in->n != 1) return LS_ACT_BADARG;
    s_rx_on = (in->v[0].i != 0);
    return LS_ACT_OK;
}

static ls_act_status_t a_mode(const ls_args_t *in, ls_val_t *out)
{
    (void)out;
    s_calls++;
    if (!in || in->n != 1 || !in->v[0].s) return LS_ACT_BADARG;
    snprintf(s_mode, sizeof(s_mode), "%s", in->v[0].s);
    return LS_ACT_OK;
}

static ls_act_status_t a_split(const ls_args_t *in, ls_val_t *out)
{
    (void)out;
    s_calls++;
    if (!in || in->n != 1) return LS_ACT_BADARG;
    s_split = in->v[0].i;
    return LS_ACT_OK;
}

/* Needs a capability a panel is not given, so the AND at the call site has
   something to refuse. */
static ls_act_status_t a_transmit(const ls_args_t *in, ls_val_t *out)
{
    (void)in; (void)out;
    s_calls++;
    return LS_ACT_OK;
}

static void setup_once(void)
{
    static bool done;
    if (done) return;
    done = true;

    ls_value_publish("t.volume", "%", v_volume);
    ls_value_publish("t.rx",     NULL, v_rx);
    ls_value_publish("t.mode",   NULL, v_mode);
    ls_value_publish("t.split",  "%", v_split);

    ls_action_register("t.volume", "i", LS_CAP_TUNE, a_volume, "volume");
    ls_action_register("t.rx",     "b", LS_CAP_TUNE, a_rx,     "receiver");
    ls_action_register("t.mode",   "s", LS_CAP_TUNE, a_mode,   "mode");
    ls_action_register("t.split",  "i", LS_CAP_UI,   a_split,  "split");
    ls_action_register("t.tx",     "",  LS_CAP_TX,   a_transmit, "emit");
}

static const char *const MODES[] = { "listen", "scan", "pocsag" };
static const char *const SPLITS[] = { "0", "25", "50", "75", "100" };

static const ls_quick_t VOL_UP = {
    .label = "VOL+", .kind = LS_QUICK_STEP, .action = "t.volume",
    .value = "t.volume", .delta = 5, .lo = 0, .hi = 100, .key = '+',
};
static const ls_quick_t VOL_DN = {
    .label = "VOL-", .kind = LS_QUICK_STEP, .action = "t.volume",
    .value = "t.volume", .delta = -5, .lo = 0, .hi = 100, .key = '-',
};
static const ls_quick_t RX = {
    .label = "RX", .kind = LS_QUICK_TOGGLE, .action = "t.rx",
    .value = "t.rx", .key = 'r',
};
static const ls_quick_t MODE = {
    .label = "MODE", .kind = LS_QUICK_CYCLE, .action = "t.mode",
    .value = "t.mode", .choices = MODES, .nchoices = 3, .key = 'm',
};
static const ls_quick_t SPLIT = {
    .label = "SPLIT", .kind = LS_QUICK_CYCLE, .action = "t.split",
    .value = "t.split", .choices = SPLITS, .nchoices = 5, .key = 's',
};
static const ls_quick_t TX = {
    .label = "TX", .kind = LS_QUICK_ACTION, .action = "t.tx", .key = 't',
};
static const ls_quick_t GHOST = {
    .label = "GHOST", .kind = LS_QUICK_ACTION, .action = "t.nothing", .key = 'g',
};

/* ---------------------------------------------------------------- cases -- */

LS_CASE(a_step_moves_from_where_the_radio_is_and_stops_at_the_ends)
{
    setup_once();
    const ls_cap_t g = ls_quick_grant_builtin();

    s_volume = 50;
    LS_CHECK(ls_quick_fire(&VOL_UP, g) == LS_ACT_OK);
    LS_CHECK_MSG(s_volume == 55, "one step up gave %ld", s_volume);

    /* The second press has to start from 55, not from 50. A panel that
       remembered its own idea of the value would put 55 here twice and look
       right until something else moved the volume. */
    LS_CHECK(ls_quick_fire(&VOL_UP, g) == LS_ACT_OK);
    LS_CHECK_MSG(s_volume == 60, "two steps up gave %ld", s_volume);

    /* Clamped, not wrapped. Full to silent on one press is a worse surprise
       than a control that stops. */
    s_volume = 98;
    ls_quick_fire(&VOL_UP, g);
    LS_CHECK_MSG(s_volume == 100, "stepping past the top gave %ld", s_volume);
    ls_quick_fire(&VOL_UP, g);
    LS_CHECK_MSG(s_volume == 100, "stepping at the top moved to %ld", s_volume);

    s_volume = 2;
    ls_quick_fire(&VOL_DN, g);
    LS_CHECK_MSG(s_volume == 0, "stepping past the bottom gave %ld", s_volume);
    ls_quick_fire(&VOL_DN, g);
    LS_CHECK_MSG(s_volume == 0, "stepping at the bottom moved to %ld", s_volume);
}

LS_CASE(a_toggle_writes_the_inverse_of_what_it_read)
{
    setup_once();
    const ls_cap_t g = ls_quick_grant_builtin();

    s_rx_on = false;
    LS_CHECK(ls_quick_fire(&RX, g) == LS_ACT_OK);
    LS_CHECK_MSG(s_rx_on, "toggling an off receiver left it off");
    LS_CHECK(ls_quick_fire(&RX, g) == LS_ACT_OK);
    LS_CHECK_MSG(!s_rx_on, "toggling an on receiver left it on");
}

LS_CASE(a_cycle_walks_its_list_and_wraps)
{
    setup_once();
    const ls_cap_t g = ls_quick_grant_builtin();

    snprintf(s_mode, sizeof(s_mode), "listen");
    ls_quick_fire(&MODE, g);
    LS_CHECK_MSG(strcmp(s_mode, "scan") == 0, "mode went to '%s'", s_mode);
    ls_quick_fire(&MODE, g);
    LS_CHECK_MSG(strcmp(s_mode, "pocsag") == 0, "mode went to '%s'", s_mode);
    ls_quick_fire(&MODE, g);
    LS_CHECK_MSG(strcmp(s_mode, "listen") == 0, "the cycle did not wrap: '%s'",
                 s_mode);

    /* A cycle over an int action is described exactly like one over a text
       action; the action's own signature is what parses the choice. */
    s_split = 50;
    ls_quick_fire(&SPLIT, g);
    LS_CHECK_MSG(s_split == 75, "split went to %ld", s_split);
    s_split = 100;
    ls_quick_fire(&SPLIT, g);
    LS_CHECK_MSG(s_split == 0, "split did not wrap: %ld", s_split);
}

LS_CASE(a_cycle_finds_its_place_whatever_the_case_of_the_reading)
{
    /* Found on the board. A control's choices are what the ACTION parses and
       the value it reads is what the SCREEN displays, and those are not the
       same spelling: fm.submode takes "pocsag" and fm.mode reads back
       "POCSAG". A case-sensitive match never recognised where the radio was,
       so every press restarted the cycle at its first choice and the control
       looked stuck on one setting. The action compares with strcasecmp; this
       has to agree with it. */
    setup_once();
    const ls_cap_t g = ls_quick_grant_builtin();

    snprintf(s_mode, sizeof(s_mode), "POCSAG");
    ls_quick_fire(&MODE, g);
    LS_CHECK_MSG(strcmp(s_mode, "listen") == 0,
                 "an upper case reading did not advance to the next choice, "
                 "it went to '%s'", s_mode);

    snprintf(s_mode, sizeof(s_mode), "Scan");
    ls_quick_fire(&MODE, g);
    LS_CHECK_MSG(strcmp(s_mode, "pocsag") == 0,
                 "a mixed case reading went to '%s'", s_mode);
}

LS_CASE(a_cycle_lands_on_its_list_when_the_radio_is_somewhere_else)
{
    /* Something outside this panel - a console, a card app - set a mode the
       control does not offer. Refusing would leave the button dead; starting
       at the first choice puts the radio somewhere the control can then walk
       from. */
    setup_once();
    snprintf(s_mode, sizeof(s_mode), "wfm");
    ls_quick_fire(&MODE, ls_quick_grant_builtin());
    LS_CHECK_MSG(strcmp(s_mode, "listen") == 0,
                 "an unrecognised mode went to '%s'", s_mode);
}

LS_CASE(the_panel_cannot_widen_the_grant_it_was_given)
{
    /* The property the action layer is built on. A panel drawn for a card
       app differs from one drawn for a built-in screen only by what its
       owner may do, and nothing here can add to that. */
    setup_once();
    const int before = s_calls;

    LS_CHECK_MSG(ls_quick_fire(&TX, ls_quick_grant_builtin()) == LS_ACT_DENIED,
                 "a built-in panel reached a transmit action");
    LS_CHECK_MSG(ls_quick_fire(&TX, ls_action_grant_user()) == LS_ACT_DENIED,
                 "a card app's panel reached a transmit action");
    LS_CHECK_MSG(s_calls == before,
                 "a denied action ran anyway: %d calls", s_calls - before);

    /* And a control a user app may legitimately use still works, so the
       refusals above are about the capability and not about the panel. */
    s_split = 0;
    LS_CHECK(ls_quick_fire(&SPLIT, ls_action_grant_user()) == LS_ACT_OK);
    LS_CHECK_MSG(s_split == 25, "a permitted control did not run: %ld", s_split);
}

LS_CASE(a_control_naming_something_that_does_not_exist_says_so)
{
    /* The failure this project keeps paying for is a control that appears to
       work. An unknown action has to be distinguishable from one that ran. */
    setup_once();
    LS_CHECK_MSG(ls_quick_fire(&GHOST, ls_quick_grant_builtin()) ==
                 LS_ACT_UNKNOWN,
                 "a control naming a missing action did not report unknown");
}

LS_CASE(a_control_shows_what_the_radio_says_not_what_it_last_wrote)
{
    setup_once();
    s_volume = 42;
    const char *st = ls_quick_state(&VOL_UP);
    LS_CHECK_MSG(st && strcmp(st, "42") == 0,
                 "the control reads '%s'", st ? st : "(none)");

    /* Moved from underneath it, the way the console does. */
    s_volume = 7;
    st = ls_quick_state(&VOL_UP);
    LS_CHECK_MSG(st && strcmp(st, "7") == 0,
                 "after an outside change the control reads '%s'",
                 st ? st : "(none)");

    s_rx_on = true;
    st = ls_quick_state(&RX);
    LS_CHECK_MSG(st && strcmp(st, "on") == 0,
                 "a toggle reads '%s'", st ? st : "(none)");
}

LS_CASE(a_key_fires_the_control_that_owns_it_and_nothing_else)
{
    setup_once();
    const ls_quick_t items[] = { VOL_UP, VOL_DN, RX, MODE };
    const ls_cap_t g = ls_quick_grant_builtin();

    s_volume = 50;
    s_rx_on = false;
    ls_act_status_t st = LS_ACT_UNKNOWN;

    LS_CHECK(ls_quick_key('+', items, 4, g, &st));
    LS_CHECK(st == LS_ACT_OK);
    LS_CHECK_MSG(s_volume == 55, "'+' gave %ld", s_volume);

    /* Case-insensitive, because CAPS is the only route to upper case on this
       keyboard and a control should not need it. */
    LS_CHECK(ls_quick_key('R', items, 4, g, &st));
    LS_CHECK_MSG(s_rx_on, "'R' did not reach the receiver toggle");

    const int before = s_calls;
    LS_CHECK_MSG(!ls_quick_key('z', items, 4, g, &st),
                 "an unbound key was claimed");
    LS_CHECK_MSG(s_calls == before, "an unbound key ran something");
}

/* ------------------------------------------------- where a tap may land -- */

static tui_cell q_back[64 * 40];
static tui_cell q_front[64 * 40];
static tui_surface q_sf;

static void q_fresh(int w, int h)
{
    tui_surface_setup(&q_sf, q_back, q_front, w, h);
    for (int i = 0; i < w * h; i++) {
        q_back[i].ch = ' ';
        q_back[i].attr = TUI_DEFAULT_ATTR;
    }
}

static int draw_settled(tui_rect area, const ls_quick_t *items, int n)
{
    int used = 0;
    for (int i = 0; i < 200; i++) used = ls_quick_draw(&q_sf, area, items, n);
    return used;
}

/* A step control and two plain actions: the two shapes the panel lays out
   differently - one box across, and a row of boxes sharing borders. */
static const ls_quick_t COVER[] = {
    { .label = "VOL", .kind = LS_QUICK_STEP, .action = "t.volume",
      .value = "t.volume", .delta = 5, .lo = 0, .hi = 100 },
    /* Plain actions carrying a fixed argument, which is how a one-direction
       control reaches an action that takes one - the same shape the map's
       ZOOM buttons use. What they set does not matter here; that they answer
       a tap at all is the whole question. */
    { .label = "A", .kind = LS_QUICK_ACTION, .action = "t.volume",
      .choices = (const char *const[]){ "7" }, .nchoices = 1 },
    { .label = "B", .kind = LS_QUICK_ACTION, .action = "t.volume",
      .choices = (const char *const[]){ "9" }, .nchoices = 1 },
};
#define N_COVER ((int)(sizeof(COVER) / sizeof(COVER[0])))

LS_CASE(every_cell_of_a_drawn_panel_answers_a_tap)
{
    /* Both real widths. A 48 column portrait panel and a 115 column
       landscape one lay the same controls out differently, and the dead
       frame was present in both. */
    setup_once();
    static const int WIDTHS[] = { 48, 40 };

    for (unsigned wi = 0; wi < sizeof(WIDTHS) / sizeof(WIDTHS[0]); wi++) {
        const int W2 = WIDTHS[wi];
        const int H2 = 34;
        q_fresh(W2, H2);

        const tui_rect area = tui_rect_make(0, 0, W2, H2);
        const int used = draw_settled(area, COVER, N_COVER);
        LS_CHECK_MSG(used > 0, "the panel drew nothing at %d columns", W2);
        if (used <= 0) continue;

        int dead = 0, first_dead_x = -1, first_dead_y = -1;
        for (int y = area.y; y < area.y + used; y++)
            for (int x = area.x; x < area.x + area.w; x++) {
                s_volume = 50;
                if (ls_quick_touch(x, y, COVER, N_COVER,
                                   ls_quick_grant_builtin(), NULL))
                    continue;
                if (first_dead_x < 0) { first_dead_x = x; first_dead_y = y; }
                dead++;
            }

        LS_CHECK_MSG(dead == 0,
                     "%d of %d cells are dead at %d columns, first at (%d,%d)",
                     dead, used * area.w, W2, first_dead_x, first_dead_y);
    }
}

LS_CASE(a_step_control_splits_exactly_where_its_divider_is_drawn)
{
    setup_once();
    const int W2 = 48, H2 = 34;
    q_fresh(W2, H2);

    const tui_rect area = tui_rect_make(0, 0, W2, H2);
    const int used = draw_settled(area, COVER, N_COVER);
    LS_CHECK(used > 0);
    if (used <= 0) return;

    int div_x = -1, probe_y = -1;
    for (int y = 1; y < used - 1 && div_x < 0; y++)
        for (int x = 4; x < W2 - 4; x++)
            if (q_back[y * W2 + x].ch == '|') { div_x = x; probe_y = y; break; }

    LS_CHECK_MSG(div_x > 0, "no divider was drawn in the step control");
    if (div_x <= 0) return;

    /* Left of it steps down, the divider column itself and right of it step
       up. The divider belongs to one side or the other and which one is
       arbitrary - what matters is that the change happens THERE and not a
       column away, because a column away is a button whose visible edge and
       working edge disagree. */
    s_volume = 50;
    LS_CHECK(ls_quick_touch(div_x - 1, probe_y, COVER, N_COVER,
                            ls_quick_grant_builtin(), NULL));
    LS_CHECK_MSG(s_volume < 50, "the cell left of the divider stepped to %ld",
                 s_volume);

    s_volume = 50;
    LS_CHECK(ls_quick_touch(div_x, probe_y, COVER, N_COVER,
                            ls_quick_grant_builtin(), NULL));
    LS_CHECK_MSG(s_volume > 50, "the divider column itself stepped to %ld",
                 s_volume);

    s_volume = 50;
    LS_CHECK(ls_quick_touch(area.x, probe_y, COVER, N_COVER,
                            ls_quick_grant_builtin(), NULL));
    LS_CHECK_MSG(s_volume < 50, "the panel's left edge column stepped to %ld",
                 s_volume);

    s_volume = 50;
    LS_CHECK(ls_quick_touch(area.x + area.w - 1, probe_y, COVER, N_COVER,
                            ls_quick_grant_builtin(), NULL));
    LS_CHECK_MSG(s_volume > 50, "the panel's right edge column stepped to %ld",
                 s_volume);
}

/* ------------------------------------------------------ what a key says -- */

/* The big word on an action's key is its name. */

/* Does `text` appear anywhere on row `y` of the drawn panel? */
static bool row_has(int w, int y, const char *text)
{
    char line[128];
    int k = 0;
    for (int x = 0; x < w && k < (int)sizeof(line) - 1; x++)
        line[k++] = q_back[y * w + x].ch;
    line[k] = 0;
    return strstr(line, text) != NULL;
}

static const ls_quick_t NAMED[] = {
    { .label = "FIND", .kind = LS_QUICK_ACTION, .action = "t.volume",
      .choices = (const char *const[]){ "1" }, .nchoices = 1 },
    { .label = "HERE", .kind = LS_QUICK_ACTION, .action = "t.volume",
      .choices = (const char *const[]){ "2" }, .nchoices = 1 },
};

LS_CASE(an_action_key_carries_its_own_name_not_the_word_GO)
{
    q_fresh(48, 40);
    const tui_rect area = tui_rect_make(0, 0, 48, 12);
    const int used = draw_settled(area, NAMED, 2);
    LS_CHECK(used > 2);

    /* The key is the interior. Row 0 is the shared top border, so anything
       from row 1 down to the bottom border is the face. */
    bool named = false, go = false;
    for (int y = 1; y < used - 1; y++) {
        if (row_has(48, y, "FIND")) named = true;
        if (row_has(48, y, "GO"))   go = true;
    }
    LS_CHECK_MSG(named, "no row of the key reads FIND");
    LS_CHECK_MSG(!go, "the key still reads GO");
}

LS_CASE(a_border_does_not_repeat_a_name_the_key_already_carries)
{
    /* The same rule this panel already applies to a toggle's state: the same
       word twice on one control was the fault the first version had. */
    q_fresh(48, 40);
    const tui_rect area = tui_rect_make(0, 0, 48, 12);
    draw_settled(area, NAMED, 2);
    LS_CHECK_MSG(!row_has(48, 0, "FIND"),
                 "the border repeats a name the key is already showing");
}

LS_CASE(a_name_too_long_for_the_key_stays_in_the_border)
{
    /* draw_cap centres the word and clips to the PANEL, not to the button,
       so a label wider than its key would run into the neighbouring control.
       GO is the honest fallback - and the name has to remain somewhere, so
       the border keeps it in that case. */
    const ls_quick_t WIDE[] = {
        { .label = "RECALIBRATE ALL", .kind = LS_QUICK_ACTION,
          .action = "t.volume",
          .choices = (const char *const[]){ "1" }, .nchoices = 1 },
        { .label = "REINITIALISE RF", .kind = LS_QUICK_ACTION,
          .action = "t.volume",
          .choices = (const char *const[]){ "2" }, .nchoices = 1 },
        { .label = "RESYNC THE LOT", .kind = LS_QUICK_ACTION,
          .action = "t.volume",
          .choices = (const char *const[]){ "3" }, .nchoices = 1 },
    };
    q_fresh(48, 40);
    const tui_rect area = tui_rect_make(0, 0, 48, 12);
    const int used = draw_settled(area, WIDE, 3);

    bool go = false;
    for (int y = 1; y < used - 1; y++)
        if (row_has(48, y, "GO")) go = true;
    LS_CHECK_MSG(go, "a label that cannot fit its key did not fall back to GO");

    /* And nothing ran into the neighbour: the whole point of the fallback.
       If a label had been centred on a key too small for it, the letters
       would be somewhere on these rows. */
    for (int y = 1; y < used - 1; y++)
        LS_CHECK_MSG(!row_has(48, y, "RECALIBRATE"),
                     "a label wider than its key was drawn anyway, on row %d",
                     y);
}
