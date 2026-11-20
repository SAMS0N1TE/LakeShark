/* What the firmware still calls in an app that now lives in a module
   (ls_mod.h): each of these is the app's own function, reached by name.

   LOADING: a console verb brings the module in (it is the user asking for
   the app). A poll - a live lamp, a hook on a sample path - never does: it
   answers "nothing" until the app has been opened once, which is also the
   truth, because nothing of the app runs before that. */
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif
#if defined(ESP_PLATFORM) && defined(CONFIG_LS_APP_MODULES) && CONFIG_LS_APP_MODULES && __has_include("ls_mod_config.h")
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include "esp_console.h"
#include "experiments/rs41_store.h"
#include "ls_experiments.h"
#include "ls_mod.h"
#include "ls_mod_config.h"
#include "ls_tui_screen.h"

/* The address of `sym` in module `mod`, looked up once it is loaded. */
#define MOD_FN(mod, sym, load)                           \
    ({                                                   \
        static void *p_;                                 \
        if (!p_)                                         \
            p_ = ls_mod_sym(mod, #sym, load);            \
        p_;                                              \
    })
#define MOD_ABSENT(name) printf("%s: its app is not on the board yet (see 'mod')\n", name)

#ifdef LS_MOD_HAS_COMPASS
bool ls_scr_compass_console(int argc, char **argv) {
    bool (*f)(int, char **) = MOD_FN("compass", ls_scr_compass_console, true);
    if (!f) {
        MOD_ABSENT("find");
        return false;
    }
    return f(argc, argv);
}
/* FREQS and the waterfall hand LORA LABS a frequency: that opens the app. */
bool ls_scr_labs_tune_marker(uint32_t hz) {
    bool (*f)(uint32_t) = MOD_FN("compass", ls_scr_labs_tune_marker, true);
    return f && f(hz);
}
/* SWEEP (its own module) hands a device to FIND. Defined here, not weak in the
   SWEEP module, which would bind its own stub (-Bsymbolic). */
bool ls_sweep_find_target(const uint8_t *mac, uint8_t radio, const char *label) {
    bool (*f)(const uint8_t *, uint8_t, const char *) = MOD_FN("compass", ls_sweep_find_target, true);
    return f && f(mac, radio, label);
}
#endif

#ifdef LS_MOD_HAS_EXPERIMENTS
void ls_exp_register_builtin(void) {} /* the module's init does it when it loads */
int ls_exp_console(int argc, char **argv) {
    int (*f)(int, char **) = MOD_FN("experiments", ls_exp_console, true);
    if (!f) {
        MOD_ABSENT("exp");
        return 1;
    }
    return f(argc, argv);
}
int ls_meters_command(int argc, char **argv) {
    int (*f)(int, char **) = MOD_FN("experiments", ls_meters_command, true);
    if (!f) {
        MOD_ABSENT("meters");
        return 1;
    }
    return f(argc, argv);
}
const ls_experiment_t *ls_exp_running(void) {
    const ls_experiment_t *(*f)(void) = MOD_FN("experiments", ls_exp_running, false);
    return f ? f() : NULL;
}
/* MAP's sonde layer: there are no flights before the experiment has run. */
bool rs41_show_map(void) {
    bool (*f)(void) = MOD_FN("experiments", rs41_show_map, false);
    return f && f();
}
bool rs41_store_copy(int slot, rs41_sonde_t *out, int64_t now) {
    bool (*f)(int, rs41_sonde_t *, int64_t) = MOD_FN("experiments", rs41_store_copy, false);
    return f && f(slot, out, now);
}
#endif

#ifdef LS_MOD_HAS_DIGI
static int digi_cmd(int argc, char **argv) {
    int (*f)(int, char **) = MOD_FN("digi", digi_console_command, true);
    if (!f) {
        MOD_ABSENT("digi");
        return 1;
    }
    return f(argc, argv);
}
void digi_console_register(void) {
    const esp_console_cmd_t c = {
        .command = "digi",
        .help = "Digital modes (FT8, FT4, WSPR): status, start/stop, band, test source, selftest decode, WAV decode",
        .hint = "[status|start|stop|mode|band|test|selftest|wav|log|trim|lag|clear]",
        .func = &digi_cmd,
    };
    esp_console_cmd_register(&c);
}
bool digi_running(void) {
    bool (*f)(void) = MOD_FN("digi", digi_running, false);
    return f && f();
}
/* FM's audio tap: nothing wants audio until DIGI has been opened. */
bool digi_sink_wanted(void) {
    bool (*f)(void) = MOD_FN("digi", digi_sink_wanted, false);
    return f && f();
}
void digi_sink_push(const int16_t *pcm, int n, int rate_hz, uint64_t dial_hz) {
    void (*f)(const int16_t *, int, int, uint64_t) = MOD_FN("digi", digi_sink_push, false);
    if (f)
        f(pcm, n, rate_hz, dial_hz);
}
#endif

#ifdef LS_MOD_HAS_TILES
/* MAP's place search draws the TILES globe behind its progress. */
void ls_tiles_search_globe(tui_surface *sf, tui_rect a, float progress, double lat, double lon) {
    void (*f)(tui_surface *, tui_rect, float, double, double) = MOD_FN("tiles", ls_tiles_search_globe, true);
    if (f)
        f(sf, a, progress, lat, lon);
}
#endif

#ifdef LS_MOD_HAS_CELL
/* cell.* values and actions exist once CELL WATCH has been opened: the
   module's init publishes them. */
void ls_cell_publish(void) {}
#endif

#ifdef LS_MOD_HAS_FILES
bool ls_scr_files_show_path(const char *path, void (*on_return)(void)) {
    bool (*f)(const char *, void (*)(void)) = MOD_FN("files", ls_scr_files_show_path, true);
    return f && f(path, on_return);
}
#endif

#ifdef LS_MOD_HAS_MUSIC
bool ls_scr_music_play_file(const char *path) {
    bool (*f)(const char *) = MOD_FN("music", ls_scr_music_play_file, true);
    return f && f(path);
}
#endif

#ifdef LS_MOD_HAS_WARDRIVE
/* HOME's lamp: a drive can only be running once the app has been opened. */
bool ls_wardrive_running(void) {
    bool (*f)(void) = MOD_FN("wardrive", ls_wardrive_running, false);
    return f && f();
}
#endif

#endif
