#include "AppMedia.hpp"

#include <cstdio>
#include <cstring>

/*LS-741*/
/* The BSP and player headers carry their own extern "C" guards and include
   C++-aware IDF headers; wrapping them in another extern "C" makes
   esp_lcd_io_i2c.h redeclare with C linkage and the build dies on a
   conflicting declaration. Only the plain C headers go in the block. */
#include "bsp/esp-bsp.h"
#include "bsp_board_extra.h"
#include "audio_player.h"
#include "media_playlist.h"
#include "ls_media_handoff.h"
/*LS-754*/
#include "ls_media_lifecycle.h"

extern "C" {
#include "audio_out.h"
#include "esp_log.h"
#include "lakeshark_backend.h"
}

#include "sdr_ui/sdr_ui.h"
#include "ui/ls_ui.h"

#define COL_LABEL   SDR_LABEL
#define COL_TEXT    SDR_TEXT
#define COL_DIM     SDR_DIM
#define COL_CYAN    SDR_CYAN
#define COL_GREEN   SDR_GREEN

static const char *TAG = "media";

#ifndef BSP_SD_MOUNT_POINT
#define BSP_SD_MOUNT_POINT "/sdcard"
#endif

/*LS-741*/
/* Two sources, because the two behave differently and hiding that is unkind:
   the SD card is where the user's own music goes and may be absent entirely,
   while /spiffs/music is baked into the firmware image and is always there.
   The media playlist does the directory walking and extension filtering, and
   bsp_extra_player_play_index does the decode - all of it already shipped
   in this tree (esp-audio-player + libhelix-mp3) and never had a UI. */
static const char *SRC_PATH[2] = { BSP_SD_MOUNT_POINT "/music", "/spiffs/music" };
static const char *SRC_NAME[2] = { "SD /music", "internal" };

/*LS-741*/
/*LS-755*/
/* One iterator per source, built lazily by rescan() and released by close().
   Older revisions kept these forever because file_iterator_delete() was only
   DECLARED in the vendored header and NEVER IMPLEMENTED, so calling it was a
   link error and nothing was ever freed. That trap is gone - the missing
   symbol lives in file_iterator_delete.c and the iterators AppMedia builds
   are through ls_media_playlist_open(), which has its own matching close(). */
static file_iterator_instance_t *s_iter_src[2] = { NULL, NULL };
static file_iterator_instance_t *s_iter = NULL;

/*LS-743*/
/* Set by the file browser before it launches this app, so opening an .mp3
   from FILES plays that file rather than dumping you on a list. Consumed on
   run() AND resume() and cleared, so a later manual launch is not hijacked
   by it - the storage now lives in ls_media_handoff so both call sites go
   through the same one-shot take(). */
void ls_media_play_path(const char *path)
{
    ls_media_handoff_set(path);
}
static bool s_player_up = false;

/*LS-754*/
/* Codec transition hooks.  Firmware wires enter -> park the radio, leave ->
   stop the audio player (gated on s_player_up because audio_player_stop must
   not run before audio_player_init).  See ls_media_lifecycle.h. */
static void music_hook_park_radio(void)
{
    lakeshark_radio_park();
}
static void music_hook_stop_audio(void)
{
    if (s_player_up) audio_player_stop();
}
static bool s_hooks_wired = false;
static void music_wire_hooks_once(void)
{
    if (s_hooks_wired) return;
    ls_media_lifecycle_hooks_t h = { music_hook_park_radio, music_hook_stop_audio };
    ls_media_lifecycle_configure(&h);
    s_hooks_wired = true;
}

/*LS-743*/
static void audio_player_play_file_path(const char *p);

/*LS-753*/
/* Consume a pending Files-to-Music handoff, if any.  Called from both run()
   and resume(): the old code only fired on run(), so after the shell had
   built Music once every later file pick landed on resume() with the path
   still sitting in the handoff store and nothing playing it. */
static void consume_pending_handoff(void)
{
    char path[192];
    if (ls_media_handoff_take(path, sizeof(path)))
        audio_player_play_file_path(path);
}

static lv_obj_t *mono(lv_obj_t *parent, lv_color_t col)
{
    return sdr_label(parent, sdr_font_mono(), col);
}

AppMedia::AppMedia() : LsApp("MUSIC", "files") {}
AppMedia::~AppMedia() = default;

/*LS-754*/
/* pause() and background() both mean "another app is coming up".  Stop the
   audio player before the shell hands off, so the next app - radio or
   otherwise - never has to fight audio_player for the codec.  The old code
   only paused the LVGL timer and let audio bleed into P25/FM despite the
   comment on close() claiming that was the reason for stopping. */
bool AppMedia::pause(void)
{
    if (_timer) lv_timer_pause(_timer);
    ls_media_lifecycle_leave();
    return true;
}
bool AppMedia::background(void)
{
    if (_timer) lv_timer_pause(_timer);
    ls_media_lifecycle_leave();
    return true;
}
/*LS-753*/
bool AppMedia::resume(void)
{
    /*LS-754*/
    /* Re-take ownership BEFORE consuming a pending handoff.  Coming back from
       Files does not mean the radio is running, but a manual jump from a
       radio app back into Music must park the radio before the player starts
       decoding into the same codec. */
    ls_media_lifecycle_enter();
    if (_timer) lv_timer_resume(_timer);
    /* Files may have stashed a new path while we were paused - play it now,
       otherwise the shell will drop the user on a resumed Music showing the
       old list instead of the file they just picked. */
    consume_pending_handoff();
    if (_now) updateNow();
    return true;
}
bool AppMedia::back(void)       { return exitToLauncher(); }

bool AppMedia::close(void)
{
    /*LS-741*/
    /* Stop on close. Leaving audio running into another app would fight the
       radio for the codec, and the P25/FM apps assume they own it. */
    /*LS-744*/
    /*LS-753*/
    /*LS-754*/
    /* Direct-file playback leaves _playing at -1 - the old check refused to
       stop it, so closing Music with a file playing let audio bleed into the
       next app.  Route through the lifecycle module so pause/background/close
       take the same ordered path and the transition is host-testable. */
    ls_media_lifecycle_leave();
    if (_timer) { lv_timer_del(_timer); _timer = nullptr; }
    _list = nullptr; _now = nullptr; _srclbl = nullptr; _playlbl = nullptr;
    _playing = -1;

    /*LS-755*/
    /* Give back everything run() and rescan() acquired.  Each source iterator
       owns its path string, its pointer array and one allocation per entry;
       the player owns audio_player's decode task, its queue and its buffers.
       The old close() released none of it, so every open/close cycle stacked
       another copy on the internal heap while the app pretended to have gone
       away.  Now close means done - the next run() pays init again. */
    for (int i = 0; i < 2; ++i) {
        if (s_iter_src[i]) {
            ls_media_playlist_close(s_iter_src[i]);
            s_iter_src[i] = NULL;
        }
    }
    s_iter = NULL;
    if (s_player_up) {
        bsp_extra_player_del();
        s_player_up = false;
    }
    return true;
}

void AppMedia::rescan(void)
{
    /*LS-743*/
    /* The managed file_iterator dereferences the result of opendir() without
       testing it, so a missing path used to fault taskLVGL in file_iterator_new
       (captured in the LS-734 coredump). The playlist builder owns the guarded
       scan now and returns NULL for an absent source. */
    if (!s_iter_src[_src]) {
        s_iter_src[_src] = ls_media_playlist_open(SRC_PATH[_src]);
        if (!s_iter_src[_src]) {
            ESP_LOGW(TAG, "%s does not exist - source empty", SRC_PATH[_src]);
        }
    }
    s_iter = s_iter_src[_src];
    _sel = 0;
    _playing = -1;
}

bool AppMedia::run(lv_obj_t *parent)
{
    /*LS-754*/
    /* Take ownership FIRST.  The normal route in is P25/FM -> Files -> Music,
       and Files is passive (LS-604) so the radio decoder was only
       backgrounded - its on_exit was never called and the pipe is still
       running.  Parking now guarantees the radio has released the codec by
       the time audio_out_ensure_unmuted and bsp_extra_player_init touch it,
       so the two writers cannot overlap. */
    music_wire_hooks_once();
    ls_media_lifecycle_enter();

    /*LS-744*/
    /* Open the codec before touching the player. bsp_extra_i2s_write() calls
       esp_codec_dev_write(play_dev_handle, ...) with NO NULL CHECK, so if the
       radio has parked the codec, any player call - including a stop that
       writes silence - faults inside the i2s critical section. The coredump
       named it: taskLVGL, bsp_extra_i2s_write(s_silence, 1024) ->
       xPortEnterCriticalTimeout with a0=0. */
    audio_out_ensure_unmuted();

    if (!s_player_up) {
        s_player_up = (bsp_extra_player_init() == ESP_OK);
        if (!s_player_up) ESP_LOGW(TAG, "player init failed - list will be read-only");
    }
    rescan();

    ls_ui_screen_t screen;
    ls_ui_screen_create(parent, "MUSIC", false, LS_UI_COLOR_ID_ROSE, &screen);
    _screen_readout = screen.readout;
    _screen_lamp = screen.lamp;
    ls_ui_screen_set_readout(&screen, "PLAYER");
    parent = screen.content;

    _srclbl = mono(parent, COL_LABEL);
    lv_obj_set_width(_srclbl, lv_pct(100));

    _now = sdr_label(parent, &lv_font_montserrat_18, COL_CYAN);
    lv_obj_set_width(_now, lv_pct(100));
    lv_label_set_text(_now, "stopped");

    _list = lv_list_create(parent);
    lv_obj_set_width(_list, lv_pct(100));
    lv_obj_set_flex_grow(_list, 1);
    ls_ui_style_plot(_list);

    lv_obj_t *row = ls_ui_controls(parent);

    ls_ui_button(row, "PREV", LS_BTN_DEFAULT, prevCb, this, nullptr);
    ls_ui_button(row, "PLAY", LS_BTN_PRIMARY, playCb, this, &_playlbl);
    ls_ui_button(row, "NEXT", LS_BTN_DEFAULT, nextCb, this, nullptr);
    ls_ui_button(row, "STOP", LS_BTN_DANGER, stopCb, this, nullptr);
    ls_ui_button(row, "SOURCE", LS_BTN_DEFAULT, srcCb, this, nullptr);

    refreshList();
    /*LS-743*/
    consume_pending_handoff();
    _timer = lv_timer_create(timerCb, 500, this);
    return true;
}

/*LS-743*/
static void audio_player_play_file_path(const char *p)
{
    if (bsp_extra_player_play_file(p) != ESP_OK)
        ESP_LOGW(TAG, "cannot play %s", p);
}

void AppMedia::refreshList(void)
{
    if (!_list) return;
    lv_obj_clean(_list);

    int n = s_iter ? file_iterator_get_count(s_iter) : 0;
    if (n <= 0) {
        lv_obj_t *b = lv_list_add_btn(_list, NULL,
            (_src == 0) ? "no music on the SD card (/music)"
                        : "no music in the firmware image");
        lv_obj_clear_flag(b, LV_OBJ_FLAG_CLICKABLE);
    }
    for (int i = 0; i < n; i++) {
        const char *nm = file_iterator_get_name_from_index(s_iter, i);
        lv_obj_t *b = lv_list_add_btn(_list, NULL, nm ? nm : "?");
        lv_obj_set_user_data(b, (void *)(intptr_t)i);
        lv_obj_add_event_cb(b, listCb, LV_EVENT_CLICKED, this);
    }
    if (_srclbl) {
        char buf[80];
        snprintf(buf, sizeof(buf), "%s  -  %d track%s",
                 SRC_NAME[_src], n, n == 1 ? "" : "s");
        lv_label_set_text(_srclbl, buf);
    }
}

void AppMedia::updateNow(void)
{
    if (!_now) return;
    char buf[96];
    /*LS-753*/
    /* Two questions, not one.  "Active" says the player still points at this
       track (playing OR paused) - if so, PLAY must resume rather than start
       over, so _playing stays set.  "Playing" is strictly PLAYING and drives
       the button label.  The old single query treated PAUSE like IDLE and
       forced a restart on every resume, and it lied about track zero because
       the underlying comparison was against a stale iterator index. */
    bool active_idx = s_iter && s_player_up && (_playing >= 0) &&
                      bsp_extra_player_is_active_by_index(s_iter, _playing);
    bool playing_idx = active_idx &&
                      bsp_extra_player_is_playing_by_index(s_iter, _playing);

    /*LS-753*/
    /* Direct-file (Files-to-Music) playback has no row in the playlist but
       still owns the codec; show the basename so the user can see what is
       playing and so STOP/PAUSE labels reflect the real state. */
    const char *dpath = s_player_up ? bsp_extra_player_active_path() : nullptr;
    bool active_direct  = (dpath != nullptr);
    bool playing_direct = active_direct && bsp_extra_player_is_playing_path();

    if (active_idx) {
        const char *nm = file_iterator_get_name_from_index(s_iter, _playing);
        snprintf(buf, sizeof(buf), "%s %s", playing_idx ? ">" : "||",
                 nm ? nm : "?");
    } else if (active_direct) {
        const char *slash = strrchr(dpath, '/');
        const char *nm    = slash ? slash + 1 : dpath;
        snprintf(buf, sizeof(buf), "%s %s", playing_direct ? ">" : "||", nm);
    } else {
        /*LS-741*/
        /* A track that finished on its own leaves _playing set; clearing it
           here is what makes PLAY restart rather than look dead. */
        if (_playing >= 0) _playing = -1;
        snprintf(buf, sizeof(buf), "stopped");
    }
    lv_label_set_text(_now, buf);
    ls_ui_readout_set(_screen_readout, buf);
    ls_ui_lamp_set(_screen_lamp, active_idx || active_direct,
                   LS_UI_COLOR_ACCENT);
    if (_playlbl)
        lv_label_set_text(_playlbl,
                          (playing_idx || playing_direct) ? "PAUSE" : "PLAY");
}

void AppMedia::timerCb(lv_timer_t *t)
{
    AppMedia *self = static_cast<AppMedia *>(t->user_data);
    if (self) self->updateNow();
}

void AppMedia::listCb(lv_event_t *e)
{
    AppMedia *self = static_cast<AppMedia *>(lv_event_get_user_data(e));
    if (!self) return;
    lv_obj_t *b = lv_event_get_target(e);
    self->_sel = (int)(intptr_t)lv_obj_get_user_data(b);
    /*LS-756*/
    /* Only claim the track is playing once audio_player agreed to take it.
       Otherwise a full player queue set _playing to a row nothing was
       decoding, and updateNow leaked the "||" / ">" indicator for a file
       that never started - taps stacked because the leaked fp exhausted
       the fatfs handle table. */
    if (bsp_extra_player_play_index(s_iter, self->_sel) == ESP_OK) {
        self->_playing = self->_sel;
    } else {
        self->_playing = -1;
        ESP_LOGW(TAG, "start track %d rejected by player", self->_sel);
    }
    self->updateNow();
}

void AppMedia::playCb(lv_event_t *e)
{
    AppMedia *self = static_cast<AppMedia *>(lv_event_get_user_data(e));
    if (!self) return;
    /*LS-744*/
    if (!s_player_up) return;

    /*LS-753*/
    /* Direct-file playback owns the codec but has no row in the playlist -
       PLAY must toggle the audio_player state directly. */
    if (bsp_extra_player_is_active_path()) {
        if (bsp_extra_player_is_playing_path()) audio_player_pause();
        else                                    audio_player_resume();
        self->updateNow();
        return;
    }

    if (!s_iter) return;
    if (self->_playing >= 0 &&
        bsp_extra_player_is_playing_by_index(s_iter, self->_playing)) {
        audio_player_pause();
    } else if (self->_playing >= 0) {
        audio_player_resume();
    } else {
        /*LS-756*/
        if (bsp_extra_player_play_index(s_iter, self->_sel) == ESP_OK) {
            self->_playing = self->_sel;
        } else {
            self->_playing = -1;
            ESP_LOGW(TAG, "start track %d rejected by player", self->_sel);
        }
    }
    self->updateNow();
}

void AppMedia::stopCb(lv_event_t *e)
{
    AppMedia *self = static_cast<AppMedia *>(lv_event_get_user_data(e));
    /*LS-744*/
    /*LS-753*/
    /* Direct-file playback leaves _playing at -1; the old check refused to
       stop it, so STOP looked broken for anything opened from Files. */
    if (s_player_up && self &&
        (self->_playing >= 0 || bsp_extra_player_is_active_path()))
        audio_player_stop();
    if (self) { self->_playing = -1; self->updateNow(); }
}

void AppMedia::prevCb(lv_event_t *e)
{
    AppMedia *self = static_cast<AppMedia *>(lv_event_get_user_data(e));
    if (!self || !s_iter) return;
    int n = file_iterator_get_count(s_iter);
    if (n <= 0) return;
    self->_sel = (self->_sel - 1 + n) % n;
    /*LS-756*/
    if (bsp_extra_player_play_index(s_iter, self->_sel) == ESP_OK) {
        self->_playing = self->_sel;
    } else {
        self->_playing = -1;
        ESP_LOGW(TAG, "start track %d rejected by player", self->_sel);
    }
    self->updateNow();
}

void AppMedia::nextCb(lv_event_t *e)
{
    AppMedia *self = static_cast<AppMedia *>(lv_event_get_user_data(e));
    if (!self || !s_iter) return;
    int n = file_iterator_get_count(s_iter);
    if (n <= 0) return;
    self->_sel = (self->_sel + 1) % n;
    /*LS-756*/
    if (bsp_extra_player_play_index(s_iter, self->_sel) == ESP_OK) {
        self->_playing = self->_sel;
    } else {
        self->_playing = -1;
        ESP_LOGW(TAG, "start track %d rejected by player", self->_sel);
    }
    self->updateNow();
}

void AppMedia::srcCb(lv_event_t *e)
{
    AppMedia *self = static_cast<AppMedia *>(lv_event_get_user_data(e));
    if (!self) return;
    /*LS-744*/
    /*LS-753*/
    /* Also stops a direct-file track: switching source is a clear "give me a
       different list" gesture and letting the old file keep playing behind a
       stale label is worse than starting silent. */
    if (s_player_up &&
        (self->_playing >= 0 || bsp_extra_player_is_active_path()))
        audio_player_stop();
    self->_playing = -1;
    self->_src = (self->_src + 1) % 2;
    self->rescan();
    self->refreshList();
    self->updateNow();
}
