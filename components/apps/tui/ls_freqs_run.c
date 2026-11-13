/* FREQS, the part that does things: TUNE hands an entry to the app that can
   hear it, freq.save keeps a frequency from any app, and `freq` at the
   console does what the screen does. The model is ls_freqs.c. */
#include "ls_freqs.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "ls_app.h"
#include "ls_notify.h"
#include "ls_tui_screen.h"
#include "ls_textpack.h"
#include "esp_attr.h"
#include "scan_engine.h"
#include "lakeshark_backend.h"
#include "apps/fm/fm_hf.h"
#include "ls_haptic.h"
#ifdef ESP_PLATFORM
#include "esp_console.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

/* The two hand-offs that open their own app, after their own checks. */
bool ls_scr_labs_tune_marker(uint32_t hz);
bool ls_scr_subghz_listen(uint32_t hz, const char *mode);

static bool open_app(const char *id)
{
    for (int i = 0; i < ls_app_count(); i++)
        if (!strcmp(ls_app_at(i)->id, id)) { ls_app_open(i); return true; }
    return false;
}

static const char *app_name(const char *id)
{
    const ls_app_t *a = id ? ls_app_by_id(id) : NULL;
    return a ? a->name : id ? id : "";
}

bool ls_freqs_tune(const ls_freq_t *e, char *msg, size_t n)
{
    const ls_freq_route_t r = ls_freq_route(e->mode, e->hz);
    char mhz[16];
    ls_freq_mhz_text(e->hz, mhz, sizeof(mhz));
    if (r.target == LS_FR_NONE) {
        snprintf(msg, n, "%s %s: %s", ls_freq_mode_name(e->mode), mhz, r.note);
        return false;
    }
    if (!ls_app_by_id(r.app)) {
        snprintf(msg, n, "%s is not in this build", r.app);
        return false;
    }
    bool ok = true, opens = true;
    const char *why = NULL;
    switch (r.target) {
    case LS_FR_FM: {
        /* The way a typed frequency goes in (ls_action_builtin.c tuned_fm):
           the lock lets go, the mode and carrier land, and the carrier is
           what is held - choosing it from a list is as deliberate. */
        ls_args_t a = { .n = 1 };
        a.v[0].kind = LS_VAL_TEXT;
        a.v[0].s = r.arg;
        lakeshark_fm_frequency_lock(false);
        ok = ls_action_call("fm.submode", &a, NULL, LS_CAP_TUNE) == LS_ACT_OK;
        if (ok && e->mode != LS_FM_AIS) {
            a.v[0].kind = LS_VAL_INT;
            a.v[0].i = (long)e->hz;
            ok = ls_action_call("fm.freq_hz", &a, NULL, LS_CAP_TUNE) == LS_ACT_OK;
            if (ok) lakeshark_fm_frequency_lock(true);
            else why = "FM tunes 24-1766 MHz";
        }
        break;
    }
    case LS_FR_HF: {
        int demod = FM_HF_AM;
        if (e->hz > 1766000000u) { ok = false; why = "FM tunes up to 1766 MHz"; break; }
        fm_hf_demod_parse(r.arg, e->hz, &demod);
        fm_hf_tune(e->hz, demod);
        break;
    }
    case LS_FR_P25:
        if (e->hz < 24000000u || e->hz > 1766000000u) { ok = false; why = "P25 tunes 24-1766 MHz"; break; }
        scan_engine_stop();
        lakeshark_p25_set_freq(e->hz);
        break;
    case LS_FR_SUBGHZ:
        opens = false;
        ok = ls_scr_subghz_listen(e->hz, ls_freq_mode_name(e->mode));
        break;
    case LS_FR_LABS:
        opens = false;
        ok = ls_scr_labs_tune_marker(e->hz);
        if (!ok) why = "the LoRa radio cannot receive there";
        break;
    default:
        break;
    }
    if (ok && opens) open_app(r.app);
    if (!ok) snprintf(msg, n, "%s refused %s%s%s", app_name(r.app), mhz, why ? ": " : "", why ? why : "");
    else snprintf(msg, n, "%s %s in %s%s%s", mhz, e->name, app_name(r.app), r.note ? "; " : "", r.note ? r.note : "");
    return ok;
}

/* ----------------------------------------------------------------- SAVE -- */

/* The banner and the buzz wait for the UI task: a save can come from the
   console, and the notice and the motor are the UI's. */
static EXT_RAM_BSS_ATTR char s_banner[LS_NOTIFY_BODY];
static volatile bool s_banner_due, s_banner_ok;

bool ls_freqs_keep(const ls_freq_t *e, char *msg, size_t n)
{
    const bool ok = ls_freqs_append("mine", e, msg, n);
    /* Said either way: a save that did not happen must not look like one. */
    snprintf(s_banner, sizeof(s_banner), "%s%s", ok ? "" : "NOT SAVED: ", msg);
    s_banner_ok = ok;
    s_banner_due = true;
    return ok;
}

static ls_act_status_t a_save(const ls_args_t *in, ls_val_t *out)
{
    static EXT_RAM_BSS_ATTR char said[96];
    char *end;
    const double mhz = strtod(in->v[0].s, &end);
    const int mode = ls_freq_mode_parse(in->v[1].s);
    if (end == in->v[0].s || mhz < 0.009 || mhz > 6000 || mode < 0) {
        snprintf(said, sizeof(said), "%s", mode < 0 ? "Not a mode FREQS knows" : "Not a frequency FREQS can keep");
        out->kind = LS_VAL_TEXT;
        out->s = said;
        return LS_ACT_BADARG;
    }
    ls_freq_t e;
    memset(&e, 0, sizeof(e));
    e.hz = (uint32_t)(mhz * 1e6 + 0.5);
    e.mode = (uint8_t)mode;
    e.lat_e5 = e.lon_e5 = LS_FREQ_NOWHERE;
    snprintf(e.group, sizeof(e.group), "MINE");
    const char *app = in->v[3].s ? in->v[3].s : "";
    char mtext[16];
    ls_freq_mhz_text(e.hz, mtext, sizeof(mtext));
    if (in->v[2].s && in->v[2].s[0]) snprintf(e.name, sizeof(e.name), "%s", in->v[2].s);
    else snprintf(e.name, sizeof(e.name), "%s%s%s", app, *app ? " " : "", mtext);
    for (char *p = e.name; *p; p++) if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 32);
    if (*app) snprintf(e.tags, sizeof(e.tags), "%s", app);
    for (char *p = e.tags; *p; p++) if (*p >= 'A' && *p <= 'Z') *p = (char)(*p + 32);
    const bool ok = ls_freqs_keep(&e, said, sizeof(said));
    out->kind = LS_VAL_TEXT;
    out->s = said;
    return ok ? LS_ACT_OK : LS_ACT_FAILED;
}

void ls_freqs_register(void)
{
    ls_freqs_lock_init();
    ls_action_register("freq.save", "ssss", LS_CAP_STORE, a_save,
                       LS_TEXT(t_ls_freqs_run_save_help, "keep a frequency in FREQS mine.csv: MHz, mode, name, app"));
}

/* -------------------------------------------------------------- console -- */

/* A console tune waits here for the UI task, which is the one that may
   open an app. */
static EXT_RAM_BSS_ATTR ls_freq_t s_req;
static volatile int s_req_state;      /* 0 idle, 1 asked, 2 answered */
static EXT_RAM_BSS_ATTR char s_req_msg[96];

void ls_freqs_tick(void)
{
    if (s_banner_due) {
        s_banner_due = false;
        const uint8_t hue = s_banner_ok ? TUI_YELLOW : TUI_RED;
        ls_notice_t note = { .screen = -1, .hue = hue, .accent = hue, .quiet = true, .steady = true };
        const ls_app_t *a = ls_app_by_id("freqs");
        if (a) note.screen = ls_tui_screen_index_of(a->screen);
        snprintf(note.title, sizeof(note.title), "FREQS");
        snprintf(note.body, sizeof(note.body), "%s", s_banner);
        ls_notify_post_quiet(&note);
        if (ls_haptic_present()) ls_haptic_play(s_banner_ok ? LS_HAPTIC_CONFIRM : LS_HAPTIC_WARN);
    }
    if (s_req_state == 1) {
        ls_freqs_tune(&s_req, s_req_msg, sizeof(s_req_msg));
        s_req_state = 2;
    }
}

/* Every entry on the card in the order FREQS lists ALL BANKS, numbered from
   1, so a number `freq list` prints is one `freq tune` takes. */
typedef struct {
    const char *find;
    int number, want, shown;
    uint32_t hz;
    bool done;
    ls_freq_t hit;
    ls_freq_out_t *out;
} walk_t;

static bool walk_sink(const ls_freq_t *e, void *ctx)
{
    walk_t *w = ctx;
    w->number++;
    if (w->out) { ls_freq_out_put(w->out, e); return true; }
    if (w->want || w->hz) {
        if (w->want ? w->number != w->want : e->hz != w->hz) return true;
        w->hit = *e;
        w->done = true;
        return false;
    }
    ls_freq_filter_t f = { 0 };
    snprintf(f.find, sizeof(f.find), "%s", w->find ? w->find : "");
    if (!ls_freq_matches(e, &f, false, 0, 0)) return true;
    char mhz[16];
    ls_freq_mhz_text(e->hz, mhz, sizeof(mhz));
    printf("%4d %12s %-7s %-24s %-3s %s\n", w->number, mhz, ls_freq_mode_name(e->mode), e->name,
           ls_freq_group_tag(e->group), e->tags);
    w->shown++;
    return true;
}

/* One bank (or all of them, or the built-in set) through `w`. */
static void walk(int which, walk_t *w)
{
    static EXT_RAM_BSS_ATTR char line[256];
    ls_freq_tally_t t;
    if (which == LS_FREQS_BUILTIN || (which == LS_FREQS_ALL && !ls_freqs_banks())) {
        ls_freq_csv_t c;
        ls_freq_csv_begin(&c);
        for (const char *p = ls_freqs_builtin_text(); *p && !w->done; ) {
            const size_t k = strcspn(p, "\n");
            snprintf(line, sizeof(line), "%.*s", (int)(k < sizeof(line) ? k : sizeof(line) - 1), p);
            p += k + (p[k] == '\n');
            ls_freq_t e;
            if (ls_freq_csv_line(&c, line, &e) == 1) walk_sink(&e, w);
        }
        return;
    }
    for (int b = which >= 0 ? which : 0; b < ls_freqs_banks() && (which < 0 || b == which) && !w->done; b++) {
        char path[96];
        snprintf(path, sizeof(path), "%s/%s.csv", ls_freqs_root(), ls_freqs_bank(b));
        FILE *f = fopen(path, "r");
        if (!f) continue;
        ls_freq_read(f, "", walk_sink, w, &t, NULL);
        fclose(f);
    }
}

static int bank_arg(const char *s)
{
    if (!strcasecmp(s, "all")) return LS_FREQS_ALL;
    if (!strcasecmp(s, "builtin") || !strcasecmp(s, "built-in")) return LS_FREQS_BUILTIN;
    return ls_freqs_bank_find(s);
}

int ls_freqs_command(int argc, char **argv)
{
    /* The console's stack is 4 KB: everything bigger than a line lives here. */
    static EXT_RAM_BSS_ATTR walk_t w;
    static EXT_RAM_BSS_ATTR ls_freq_out_t out;
    static EXT_RAM_BSS_ATTR char msg[128], name[LS_FREQ_NAME];
    const char *verb = argc > 1 ? argv[1] : "banks";
    ls_freqs_scan();
    memset(&w, 0, sizeof(w));
    if (!strcmp(verb, "banks")) {
        printf("%s: %d bank%s\n", ls_freqs_root(), ls_freqs_banks(), ls_freqs_banks() == 1 ? "" : "s");
        for (int i = 0; i < ls_freqs_banks(); i++) printf("  %s\n", ls_freqs_bank(i));
        if (!ls_freqs_banks()) puts("  none: FREQS uses the built-in set");
        return 0;
    }
    if (!strcmp(verb, "list")) {
        w.find = argc > 2 ? argv[2] : NULL;
        walk(LS_FREQS_ALL, &w);
        printf("%d of %d\n", w.shown, w.number);
        return 0;
    }
    if (!strcmp(verb, "save") && argc >= 4) {
        name[0] = 0;
        for (int i = 4; i < argc; i++) {
            const size_t k = strlen(name);
            snprintf(name + k, sizeof(name) - k, "%s%s", k ? " " : "", argv[i]);
        }
        ls_args_t a = { .n = 4 };
        const char *v[4] = { argv[2], argv[3], name, "console" };
        for (int i = 0; i < 4; i++) { a.v[i].kind = LS_VAL_TEXT; a.v[i].s = v[i]; }
        ls_val_t r;
        const ls_act_status_t st = a_save(&a, &r);
        puts(st == LS_ACT_BADARG ? "freq save <MHz> <mode> <name...>" : r.s);
        return st == LS_ACT_OK ? 0 : 1;
    }
    if (!strcmp(verb, "import") && argc == 3) {
        const bool ok = ls_freqs_import(argv[2], msg, sizeof(msg));
        puts(msg);
        return ok ? 0 : 1;
    }
    if (!strcmp(verb, "export") && argc == 4) {
        const int b = bank_arg(argv[2]);
        const size_t len = strlen(argv[3]);
        if (b == -1 && strcasecmp(argv[2], "all")) { printf("No bank %s\n", argv[2]); return 1; }
        if (!ls_freq_out_open(&out, argv[3], len > 6 && !strcasecmp(argv[3] + len - 6, ".lscan"))) {
            if (out.f) fclose(out.f);
            printf("Cannot write %s\n", argv[3]);
            return 1;
        }
        w.out = &out;
        walk(b, &w);
        const bool ok = ls_freq_out_close(&out, argv[3], msg, sizeof(msg));
        puts(msg);
        return ok ? 0 : 1;
    }
    if (!strcmp(verb, "tune") && argc == 3) {
        char *end;
        const long number = strtol(argv[2], &end, 10);
        if (!strchr(argv[2], '.') && !*end && number > 0) {
            w.want = (int)number;
            walk(LS_FREQS_ALL, &w);
            if (!w.done) { printf("No entry %ld; freq list numbers them\n", number); return 1; }
            s_req = w.hit;
        } else {
            /* A frequency: the first entry on it, or plain NFM. */
            const double mhz = strtod(argv[2], &end);
            if (end == argv[2] || mhz < 0.5 || mhz > 6000) { puts("freq tune <number|MHz>"); return 1; }
            w.hz = (uint32_t)(mhz * 1e6 + 0.5);
            walk(LS_FREQS_ALL, &w);
            if (w.done) s_req = w.hit;
            else {
                memset(&s_req, 0, sizeof(s_req));
                s_req.hz = w.hz;
                s_req.mode = LS_FM_NFM;
                s_req.lat_e5 = s_req.lon_e5 = LS_FREQ_NOWHERE;
                ls_freq_mhz_text(w.hz, s_req.name, sizeof(s_req.name));
            }
        }
        s_req_state = 1;
#ifdef ESP_PLATFORM
        for (int i = 0; i < 40 && s_req_state == 1; i++) vTaskDelay(pdMS_TO_TICKS(25));
#endif
        puts(s_req_state == 2 ? s_req_msg : "Asked; the screen takes it on its next frame");
        s_req_state = s_req_state == 2 ? 0 : s_req_state;
        return 0;
    }
    LS_PUTS(t_ls_freqs_run_usage, "freq banks | list [text] | save <MHz> <mode> <name...> | import <path> |\n"
            "     export <bank|all|builtin> <path.csv|path.lscan> | tune <number|MHz>");
    return 1;
}

void ls_freqs_console_register(void)
{
#ifdef ESP_PLATFORM
    static const esp_console_cmd_t cmd = {
        .command = "freq",
        .help = LS_TXT(con_ls_freqs_freq_help, "FREQS, the frequency library: banks, list, save, import, export, tune"),
        .hint = LS_TXT(con_ls_freqs_freq_hint, "banks | list [text] | save <MHz> <mode> <name> | import <path> | export <bank> <path> | tune <n|MHz>"),
        .func = &ls_freqs_command,
    };
    esp_console_cmd_register(&cmd);
#endif
}
