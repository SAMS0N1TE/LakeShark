#include "FileBrowser.hpp"
/**/
#include "shell/ls_shell.hpp"
/**/
#include "shell/ls_shell_nav.h"
/**/
#include "media_gui/AppMedia.hpp"
/**/
#include "file_browser/file_browser_path.h"
/**/
#include "media_gui/media_playlist.h"
/**/
#include "file_browser/ls_dialog_slot.h"
#include <strings.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>

#include "esp_log.h"
#include "esp_err.h"
#include "bsp/esp-bsp.h"
#include "sdr_ui/sdr_ui.h"
#include "ui/ls_ui.h"

LV_IMG_DECLARE(img_app_file_browser);

static const char *TAG = "FileBrowser";

static const char *kRoots[] = {
    BSP_SPIFFS_MOUNT_POINT,
#ifdef BSP_SD_MOUNT_POINT
    BSP_SD_MOUNT_POINT,
#endif
};

/**/
/* LVGL adapters for ls_dialog_slot.  Kept as free functions with C linkage so
   their addresses fit the void*-based hook signature the module uses to stay
   LVGL-free for the bench. */
extern "C" {
static void ls_fb_dlg_del(void *obj)
{
    lv_obj_del(static_cast<lv_obj_t *>(obj));
}
static bool ls_fb_dlg_is_valid(const void *obj)
{
    return lv_obj_is_valid(static_cast<const lv_obj_t *>(obj));
}
}

AppFileBrowser::AppFileBrowser()
    : LsApp("Files", "files"),
      _container(nullptr),
      _path_label(nullptr), _list(nullptr), _cwd(""), _dialog{nullptr}
{
    /**/
    /* Configure the slot hooks once - the module carries a single set globally
       because there is a single Files instance and this is the only user. */
    const ls_dialog_slot_hooks_t h = { ls_fb_dlg_del, ls_fb_dlg_is_valid };
    ls_dialog_slot_configure(&h);
}

AppFileBrowser::~AppFileBrowser() = default;

bool AppFileBrowser::init(void)    { return true; }

/**/
/* pause() runs when the shell backgrounds Files for another app.  The old
   version left the modal dialog parented to lv_scr_act(), so switching apps
   via the status bar left it visible - and its callbacks live - over
   whatever the shell launched next.  Sweep the slot before we go. */
bool AppFileBrowser::pause(void)
{
    ls_dialog_slot_close(&_dialog);
    return true;
}

bool AppFileBrowser::resume(void)  { return true; }

bool AppFileBrowser::run(lv_obj_t *parent)
{
    _cwd = "";
    /**/
    /* Record the container the shell handed us and clear any prior slot in
       case run() is called again after a close(). */
    _container = parent;
    ls_dialog_slot_forget(&_dialog);
    ls_ui_screen_t screen;
    ls_ui_screen_create(parent, "FILES", false, LS_UI_COLOR_ID_STEEL, &screen);
    _screen_readout = screen.readout;
    _screen_lamp = screen.lamp;
    ls_ui_screen_set_readout(&screen, "STORAGE");
    buildUi(screen.content);
    loadDirectory(_cwd);
    return true;
}

bool AppFileBrowser::back(void)
{
    /**/

    if (ls_dialog_slot_is_open(&_dialog)) {
        ls_dialog_slot_close(&_dialog);
        return true;
    }

    /**/
    /* Same predicate the visible Up button uses in onBackButtonClicked, so
       the two entry points can never disagree about "am I at the roots". */
    if (ls_shell_nav_fb_up_is_home(_cwd.c_str())) {
        return exitToLauncher();
    }
    auto slash = _cwd.find_last_of('/');
    if (slash == std::string::npos || slash == 0) {
        _cwd = "";
    } else {
        _cwd = _cwd.substr(0, slash);

    }
    loadDirectory(_cwd);
    return true;
}

/**/

bool AppFileBrowser::close(void)
{
    ls_dialog_slot_close(&_dialog);
    _path_label = _list = nullptr;
    _container = nullptr;
    /* clear() retained the directory's backing array while another
     * app was foregrounded. Directory state is rebuilt by run(), not settings. */
    std::vector<Entry>().swap(_entries);
    std::string().swap(_cwd);
    std::string().swap(_dialog_path);
    return true;
}

void AppFileBrowser::buildUi(lv_obj_t *parent)
{
    lv_obj_t *bar = ls_ui_panel(parent, nullptr);
    _path_label = ls_ui_readout(bar, "/");

    /**/
    /* Entry point for the music player. registered AppMedia hidden from
       the rail, which is what was asked for, but nothing was ever wired to
       launch it - so the app was on the board and unreachable. Hidden means
       "not in the rail", not "no way in". */
    _list = lv_list_create(parent);
    lv_obj_set_width(_list, lv_pct(100));
    lv_obj_set_height(_list, 0);
    lv_obj_set_flex_grow(_list, 1);
    lv_obj_set_style_bg_color(_list, SDR_BG, 0);
    ls_ui_style_table(_list);

    lv_obj_t *controls = ls_ui_controls(parent);
    ls_ui_button(controls, LV_SYMBOL_UP, LS_BTN_DEFAULT,
                 onBackButtonClicked, this, nullptr);
    ls_ui_button(controls, LV_SYMBOL_AUDIO " MUSIC", LS_BTN_PRIMARY,
                 onMusicClicked, this, nullptr);
}

void AppFileBrowser::loadDirectory(const std::string &path)
{
    _entries.clear();
    if (_list) lv_obj_clean(_list);
    if (_path_label) lv_label_set_text(_path_label, path.empty() ? "/ (roots)" : path.c_str());
    ls_ui_readout_set(_screen_readout, path.empty() ? "ROOTS" : path.c_str());
    ls_ui_lamp_set(_screen_lamp, true, LS_UI_COLOR_ACCENT);

    if (path.empty()) {

        for (auto root : kRoots) {
            Entry e{ root, true, 0 };
            _entries.push_back(e);
        }
    } else {
        DIR *dir = opendir(path.c_str());
        if (!dir) {
            ESP_LOGW(TAG, "opendir(%s) failed", path.c_str());
            lv_obj_t *btn = lv_list_add_btn(_list, LV_SYMBOL_WARNING, "Cannot open");
            lv_obj_clear_flag(btn, LV_OBJ_FLAG_CLICKABLE);
            return;
        }
        struct dirent *de;
        while ((de = readdir(dir)) != nullptr) {
            if (de->d_name[0] == '.' && (de->d_name[1] == 0 ||
                (de->d_name[1] == '.' && de->d_name[2] == 0))) continue;
            std::string full = path + "/" + de->d_name;
            struct stat st {};
            if (stat(full.c_str(), &st) != 0) continue;
            _entries.push_back({ de->d_name, S_ISDIR(st.st_mode), (size_t)st.st_size });
        }
        closedir(dir);

        std::sort(_entries.begin(), _entries.end(), [](const Entry &a, const Entry &b) {
            if (a.is_dir != b.is_dir) return a.is_dir;
            return a.name < b.name;
        });
    }

    for (size_t i = 0; i < _entries.size(); ++i) {
        const Entry &e = _entries[i];
        char label[160];
        if (e.is_dir) {
            snprintf(label, sizeof(label), "%s", e.name.c_str());
        } else {

            if (e.size >= 1024 * 1024) {
                snprintf(label, sizeof(label), "%s   (%.1f MB)", e.name.c_str(), e.size / 1048576.0);
            } else if (e.size >= 1024) {
                snprintf(label, sizeof(label), "%s   (%.1f KB)", e.name.c_str(), e.size / 1024.0);
            } else {
                snprintf(label, sizeof(label), "%s   (%u B)", e.name.c_str(), (unsigned)e.size);
            }
        }
        lv_obj_t *btn = lv_list_add_btn(_list,
                                        e.is_dir ? LV_SYMBOL_DIRECTORY : LV_SYMBOL_FILE,
                                        label);

        lv_obj_set_user_data(btn, (void *)(uintptr_t)(i + 1));
        lv_obj_add_event_cb(btn, onListRowClicked, LV_EVENT_CLICKED, this);
    }
}

void AppFileBrowser::openEntry(const Entry &e)
{
    /**/
    /* _cwd is already an absolute path once we are past the roots view
       (either "/sdcard/..." or "/spiffs/...").  file_browser_join_path
       therefore does not - and must not - prepend a root of its own.  The
       old code built `full` correctly here and then, one branch below,
       re-prefixed it with "/sdcard/" for the music handoff.  That doubled
       the SD root on real songs and silently redirected every /spiffs
       selection to a path that did not exist. */
    char joined[192];
    if (file_browser_join_path(_cwd.c_str(), e.name.c_str(),
                               joined, sizeof(joined)) < 0) {
        ESP_LOGW(TAG, "path too long for %s", e.name.c_str());
        return;
    }
    std::string full(joined);

    if (e.is_dir) {
        _cwd = full;
        loadDirectory(_cwd);
        return;
    }

    /**/

    /**/
    /* The old list here advertised .m4a and .flac too, but the bundled
       chmorgan__esp-audio-player is only compiled with MP3 and WAV
       decoders.  Launching Music on an M4A or FLAC put the user on a
       list where their file could not be played and produced an
       unhelpful "unknown file type" from the audio_player thread.
       Route only what the current build can actually decode, and keep
       the rest on the properties dialog with a note that says so. */
    const char *dot = strrchr(e.name.c_str(), '.');
    if (dot) {
        bool audio_ext = !strcasecmp(dot, ".mp3") || !strcasecmp(dot, ".wav") ||
                         !strcasecmp(dot, ".m4a") || !strcasecmp(dot, ".flac");
        if (audio_ext) {
            if (ls_media_supported_extension(e.name.c_str())) {
                ls_media_play_path(full.c_str());
                LsShell::instance().launchByName("MUSIC");
                return;
            }
            showFileDialog(full, e.size,
                           "Unsupported audio format: this build only decodes MP3 and WAV.");
            return;
        }
    }
    showFileDialog(full, e.size, nullptr);
}

void AppFileBrowser::showFileDialog(const std::string &full_path, size_t size,
                                    const char *note)
{
    /**/
    /* Sweep any previous dialog through the slot so a re-entrant open leaves
       exactly one live object behind. */
    ls_dialog_slot_close(&_dialog);
    _dialog_path = full_path;

    /**/
    /* Parent to the app container, not lv_scr_act().  The old parent was the
       screen root, which meant the shell hiding or replacing our container
       had no effect on the dialog - it survived as an overlay on top of the
       next app with click handlers that still pointed back into Files. */
    lv_obj_t *host = _container ? _container : lv_scr_act();
    lv_obj_t *dlg  = ls_ui_panel(host, nullptr);
    ls_dialog_slot_set(&_dialog, dlg);
    lv_obj_set_size(dlg, lv_pct(92), lv_pct(75));
    lv_obj_center(dlg);
    lv_obj_set_style_bg_color(dlg, LS_UI_PANEL, 0);
    lv_obj_set_flex_flow(dlg, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(dlg, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    auto add_label = [&](const char *text, const lv_font_t *font) {
        lv_obj_t *l = lv_label_create(dlg);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(l, lv_pct(100));
        lv_label_set_text(l, text);
        lv_obj_set_style_text_color(l, lv_color_white(), 0);
        lv_obj_set_style_text_font(l, font, 0);
        return l;
    };

    add_label(full_path.c_str(), &lv_font_montserrat_22);
    char meta[80];
    snprintf(meta, sizeof(meta), "Size: %u bytes", (unsigned)size);
    add_label(meta, &lv_font_montserrat_20);

    /**/

    if (note && note[0]) {
        lv_obj_t *n = add_label(note, &lv_font_montserrat_20);
        lv_obj_set_style_text_color(n, LS_UI_WARN, 0);
    }

    FILE *f = fopen(full_path.c_str(), "rb");
    if (f) {
        char buf[1024];
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        buf[n] = 0;
        bool printable = true;
        for (size_t i = 0; i < n; ++i) {
            unsigned char c = (unsigned char)buf[i];
            if (c != '\n' && c != '\r' && c != '\t' && (c < 0x20 || c > 0x7E)) {
                printable = false;
                break;
            }
        }
        lv_obj_t *prev = add_label(printable ? buf : "(binary file, preview hidden)",
                                   &lv_font_montserrat_16);
        lv_obj_set_style_text_color(prev, LS_UI_ACCENT, 0);
        ls_ui_style_overlay(prev);
        lv_obj_set_height(prev, lv_pct(45));
        lv_obj_set_flex_grow(prev, 1);
    }

    lv_obj_t *row = ls_ui_controls(dlg);
    ls_ui_button(row, "Close", LS_BTN_DEFAULT,
                 onDialogCloseClicked, this, nullptr);
    ls_ui_button(row, "Delete", LS_BTN_DANGER,
                 onDialogDeleteClicked, this, nullptr);
}

void AppFileBrowser::onListRowClicked(lv_event_t *e)
{
    AppFileBrowser *app = static_cast<AppFileBrowser *>(lv_event_get_user_data(e));
    if (!app) return;
    lv_obj_t *btn = lv_event_get_target(e);
    uintptr_t idx1 = (uintptr_t)lv_obj_get_user_data(btn);
    if (idx1 == 0 || idx1 > app->_entries.size()) return;
    app->openEntry(app->_entries[idx1 - 1]);
}

void AppFileBrowser::onBackButtonClicked(lv_event_t *e)
{
    AppFileBrowser *app = static_cast<AppFileBrowser *>(lv_event_get_user_data(e));
    if (!app) return;
    /**/
    if (ls_shell_nav_fb_up_is_home(app->_cwd.c_str())) {
        app->exitToLauncher();
        return;
    }
    auto slash = app->_cwd.find_last_of('/');
    app->_cwd = (slash == std::string::npos || slash == 0) ? "" : app->_cwd.substr(0, slash);
    app->loadDirectory(app->_cwd);
}

void AppFileBrowser::onDialogCloseClicked(lv_event_t *e)
{
    AppFileBrowser *app = static_cast<AppFileBrowser *>(lv_event_get_user_data(e));
    if (!app) return;
    /**/
    ls_dialog_slot_close(&app->_dialog);
}

void AppFileBrowser::onDialogDeleteClicked(lv_event_t *e)
{
    AppFileBrowser *app = static_cast<AppFileBrowser *>(lv_event_get_user_data(e));
    if (!app || !ls_dialog_slot_is_open(&app->_dialog)) return;
    if (remove(app->_dialog_path.c_str()) == 0) {
        ESP_LOGI(TAG, "deleted %s", app->_dialog_path.c_str());
    } else {
        ESP_LOGW(TAG, "delete %s failed", app->_dialog_path.c_str());
    }
    /**/
    ls_dialog_slot_close(&app->_dialog);
    app->loadDirectory(app->_cwd);
}

/**/
void AppFileBrowser::onMusicClicked(lv_event_t *e)
{
    (void)e;
    LsShell::instance().launchByName("MUSIC");
}
