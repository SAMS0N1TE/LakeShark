#include "FileBrowser.hpp"

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

LV_IMG_DECLARE(img_app_file_browser);

static const char *TAG = "FileBrowser";

static const char *kRoots[] = {
    BSP_SPIFFS_MOUNT_POINT,
#ifdef BSP_SD_MOUNT_POINT
    BSP_SD_MOUNT_POINT,
#endif
};

AppFileBrowser::AppFileBrowser()
    : LsApp("Files", "files"),
      _path_label(nullptr), _list(nullptr), _cwd(""), _dialog(nullptr)
{
}

AppFileBrowser::~AppFileBrowser() = default;

bool AppFileBrowser::init(void)    { return true; }
bool AppFileBrowser::pause(void)   { return true; }
bool AppFileBrowser::resume(void)  { return true; }

bool AppFileBrowser::run(lv_obj_t *parent)
{
    _cwd = "";
    _dialog = nullptr;
    buildUi(parent);
    loadDirectory(_cwd);
    return true;
}

bool AppFileBrowser::back(void)
{

    if (_dialog && lv_obj_is_valid(_dialog)) {
        lv_obj_del(_dialog);
        _dialog = nullptr;
        return true;
    }

    if (_cwd.empty()) {
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

bool AppFileBrowser::close(void)
{
    _path_label = _list = _dialog = nullptr;
    _entries.clear();
    return true;
}

void AppFileBrowser::buildUi(lv_obj_t *parent)
{
    lv_obj_set_style_bg_color(parent, SDR_BG, 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_set_size(bar, lv_pct(100), 64);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(bar, SDR_LCD_BG, 0);
    lv_obj_set_style_border_color(bar, SDR_PAS_GOLD, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 8, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *up_btn = lv_btn_create(bar);
    lv_obj_set_size(up_btn, 80, 44);
    lv_obj_set_style_min_height(up_btn, 44, 0);
    lv_obj_set_style_max_height(up_btn, 44, 0);
    lv_obj_align(up_btn, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *up_lbl = lv_label_create(up_btn);
    lv_label_set_text(up_lbl, LV_SYMBOL_UP);
    lv_obj_center(up_lbl);
    lv_obj_add_event_cb(up_btn, onBackButtonClicked, LV_EVENT_CLICKED, this);

    _path_label = lv_label_create(bar);
    lv_obj_align(_path_label, LV_ALIGN_LEFT_MID, 96, 0);
    lv_obj_set_style_text_color(_path_label, SDR_PAS_GOLD, 0);
    lv_obj_set_style_text_font(_path_label, &lv_font_montserrat_22, 0);
    lv_label_set_text(_path_label, "/");

    _list = lv_list_create(parent);
    lv_obj_set_size(_list, lv_pct(100), lv_pct(100) - 64);
    lv_obj_align(_list, LV_ALIGN_TOP_MID, 0, 64);
    lv_obj_set_style_bg_color(_list, SDR_BG, 0);
    lv_obj_set_style_border_width(_list, 0, 0);
    lv_obj_set_style_pad_all(_list, 4, 0);
}

void AppFileBrowser::loadDirectory(const std::string &path)
{
    _entries.clear();
    if (_list) lv_obj_clean(_list);
    if (_path_label) lv_label_set_text(_path_label, path.empty() ? "/ (roots)" : path.c_str());

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
    std::string full = _cwd.empty() ? e.name : _cwd + "/" + e.name;
    if (e.is_dir) {
        _cwd = full;
        loadDirectory(_cwd);
    } else {
        showFileDialog(full, e.size);
    }
}

void AppFileBrowser::showFileDialog(const std::string &full_path, size_t size)
{
    if (_dialog && lv_obj_is_valid(_dialog)) {
        lv_obj_del(_dialog);
    }
    _dialog_path = full_path;

    _dialog = lv_obj_create(lv_scr_act());
    lv_obj_set_size(_dialog, lv_pct(92), lv_pct(75));
    lv_obj_center(_dialog);
    lv_obj_set_style_bg_color(_dialog, lv_color_hex(0x202830), 0);
    lv_obj_set_style_border_color(_dialog, lv_color_hex(0x60A0E0), 0);
    lv_obj_set_style_border_width(_dialog, 2, 0);
    lv_obj_set_style_radius(_dialog, 12, 0);
    lv_obj_set_style_pad_all(_dialog, 16, 0);
    lv_obj_set_flex_flow(_dialog, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(_dialog, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    auto add_label = [&](const char *text, const lv_font_t *font) {
        lv_obj_t *l = lv_label_create(_dialog);
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
        lv_obj_set_style_text_color(prev, lv_color_hex(0x80E080), 0);
        lv_obj_set_style_bg_color(prev, lv_color_hex(0x101418), 0);
        lv_obj_set_style_bg_opa(prev, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_all(prev, 8, 0);
        lv_obj_set_style_radius(prev, 6, 0);
        lv_obj_set_height(prev, lv_pct(45));
        lv_obj_set_flex_grow(prev, 1);
    }

    lv_obj_t *row = lv_obj_create(_dialog);
    lv_obj_set_size(row, lv_pct(100), 70);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_AROUND, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *close_btn = lv_btn_create(row);
    lv_obj_set_size(close_btn, 180, 56);
    lv_obj_set_style_min_height(close_btn, 56, 0);
    lv_obj_set_style_max_height(close_btn, 56, 0);
    lv_obj_t *cl = lv_label_create(close_btn);
    lv_label_set_text(cl, "Close");
    lv_obj_center(cl);
    lv_obj_add_event_cb(close_btn, onDialogCloseClicked, LV_EVENT_CLICKED, this);

    lv_obj_t *del_btn = lv_btn_create(row);
    lv_obj_set_size(del_btn, 180, 56);
    lv_obj_set_style_min_height(del_btn, 56, 0);
    lv_obj_set_style_max_height(del_btn, 56, 0);
    lv_obj_set_style_bg_color(del_btn, lv_color_hex(0xCC2222), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(del_btn, lv_color_hex(0x881111), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_t *dl = lv_label_create(del_btn);
    lv_label_set_text(dl, "Delete");
    lv_obj_center(dl);
    lv_obj_add_event_cb(del_btn, onDialogDeleteClicked, LV_EVENT_CLICKED, this);
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
    if (app->_cwd.empty()) {

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
    if (!app || !app->_dialog) return;
    lv_obj_del(app->_dialog);
    app->_dialog = nullptr;
}

void AppFileBrowser::onDialogDeleteClicked(lv_event_t *e)
{
    AppFileBrowser *app = static_cast<AppFileBrowser *>(lv_event_get_user_data(e));
    if (!app || !app->_dialog) return;
    if (remove(app->_dialog_path.c_str()) == 0) {
        ESP_LOGI(TAG, "deleted %s", app->_dialog_path.c_str());
    } else {
        ESP_LOGW(TAG, "delete %s failed", app->_dialog_path.c_str());
    }
    lv_obj_del(app->_dialog);
    app->_dialog = nullptr;
    app->loadDirectory(app->_cwd);
}
