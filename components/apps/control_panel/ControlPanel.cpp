#include "ControlPanel.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sstream>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_http_client.h"
#include "cJSON.h"
#include "bsp/esp-bsp.h"

LV_IMG_DECLARE(img_app_control_panel);

static const char *TAG = "ControlPanel";

AppControlPanel *AppControlPanel::_instance = nullptr;

#define PANELS_DIR  BSP_SPIFFS_MOUNT_POINT "/panels"

AppControlPanel::AppControlPanel()
    : ESP_Brookesia_PhoneApp("Panel", &img_app_control_panel, true),
      _tabview(nullptr), _httpd(nullptr)
{
    _instance = this;
}

AppControlPanel::~AppControlPanel()
{
    stopHttpServer();
    if (_instance == this) _instance = nullptr;
}

bool AppControlPanel::init(void)
{

    loadAllPanels();
    startHttpServer();
    return true;
}

bool AppControlPanel::pause(void)  { return true; }
bool AppControlPanel::resume(void) { return true; }

bool AppControlPanel::run(void)
{

    loadAllPanels();
    buildUi(lv_scr_act());
    return true;
}

bool AppControlPanel::back(void)
{
    return notifyCoreClosed();
}

bool AppControlPanel::close(void)
{
    for (auto &p : _panels) {
        p->tab_page = nullptr;
        for (auto &w : p->widgets) w->lv_obj = nullptr;
    }
    _tabview = nullptr;
    return true;
}

static AppControlPanel::WidgetType type_from_str(const char *s)
{
    if (!s) return AppControlPanel::WidgetType::Unknown;
    if (!strcmp(s, "button")) return AppControlPanel::WidgetType::Button;
    if (!strcmp(s, "toggle")) return AppControlPanel::WidgetType::Toggle;
    if (!strcmp(s, "slider")) return AppControlPanel::WidgetType::Slider;
    if (!strcmp(s, "label"))  return AppControlPanel::WidgetType::Label;
    return AppControlPanel::WidgetType::Unknown;
}

bool AppControlPanel::loadAllPanels(void)
{
    _panels.clear();

    DIR *dir = opendir(PANELS_DIR);
    if (!dir) {
        ESP_LOGW(TAG, "No %s directory; no panels loaded", PANELS_DIR);
        return false;
    }
    struct dirent *de;
    std::vector<std::string> files;
    while ((de = readdir(dir)) != nullptr) {
        std::string n = de->d_name;
        if (n.size() > 5 && n.substr(n.size() - 5) == ".json") {
            files.push_back(n);
        }
    }
    closedir(dir);
    std::sort(files.begin(), files.end());

    for (auto &fname : files) {
        loadPanelFile(std::string(PANELS_DIR) + "/" + fname);
    }
    ESP_LOGI(TAG, "Loaded %d panel(s)", (int)_panels.size());
    return !_panels.empty();
}

bool AppControlPanel::loadPanelFile(const std::string &full_path)
{
    FILE *f = fopen(full_path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > 32 * 1024) { fclose(f); return false; }
    std::string text(n, 0);
    fread(&text[0], 1, n, f);
    fclose(f);

    cJSON *root = cJSON_Parse(text.c_str());
    if (!root) {
        ESP_LOGE(TAG, "JSON parse failed in %s", full_path.c_str());
        return false;
    }

    auto panel = std::make_unique<Panel>();

    auto slash = full_path.find_last_of('/');
    std::string fname = (slash == std::string::npos) ? full_path : full_path.substr(slash + 1);
    panel->name = fname.substr(0, fname.size() - 5);
    panel->cols = 4;
    panel->rows = 6;
    panel->tab_page = nullptr;

    cJSON *title = cJSON_GetObjectItem(root, "title");
    panel->title = (title && cJSON_IsString(title)) ? title->valuestring : panel->name;

    cJSON *grid = cJSON_GetObjectItem(root, "grid");
    if (grid) {
        cJSON *c = cJSON_GetObjectItem(grid, "cols");
        cJSON *r = cJSON_GetObjectItem(grid, "rows");
        if (c && cJSON_IsNumber(c)) panel->cols = c->valueint;
        if (r && cJSON_IsNumber(r)) panel->rows = r->valueint;
    }
    if (panel->cols < 1) panel->cols = 1;
    if (panel->rows < 1) panel->rows = 1;

    cJSON *widgets = cJSON_GetObjectItem(root, "widgets");
    if (widgets && cJSON_IsArray(widgets)) {
        cJSON *w;
        cJSON_ArrayForEach(w, widgets) {
            auto getstr = [&](const char *k, const char *def) {
                cJSON *x = cJSON_GetObjectItem(w, k);
                return (x && cJSON_IsString(x)) ? x->valuestring : def;
            };
            auto getint = [&](const char *k, int def) {
                cJSON *x = cJSON_GetObjectItem(w, k);
                return (x && cJSON_IsNumber(x)) ? x->valueint : def;
            };
            auto getdbl = [&](const char *k, double def) {
                cJSON *x = cJSON_GetObjectItem(w, k);
                return (x && cJSON_IsNumber(x)) ? x->valuedouble : def;
            };

            auto wi = std::make_unique<Widget>();
            wi->id      = getstr("id", "");
            wi->title   = getstr("title", wi->id.c_str());
            wi->type    = type_from_str(getstr("type", ""));
            wi->col     = getint("col", 0);
            wi->row     = getint("row", 0);
            wi->w       = getint("w", 1);
            wi->h       = getint("h", 1);
            wi->min     = getdbl("min", 0);
            wi->max     = getdbl("max", 100);
            wi->webhook = getstr("webhook", "");
            wi->lv_obj  = nullptr;

            cJSON *v = cJSON_GetObjectItem(w, "value");
            switch (wi->type) {
                case WidgetType::Toggle:
                    wi->value = (v && cJSON_IsBool(v)) ? (bool)cJSON_IsTrue(v) : false;
                    break;
                case WidgetType::Slider:
                    wi->value = (v && cJSON_IsNumber(v)) ? v->valuedouble : 0.0;
                    break;
                case WidgetType::Label:
                    wi->value = std::string((v && cJSON_IsString(v)) ? v->valuestring : "");
                    break;
                case WidgetType::Button:
                default:
                    wi->value = false;
                    break;
            }

            if (wi->id.empty() || wi->type == WidgetType::Unknown) {
                ESP_LOGW(TAG, "[%s] skipping widget with missing id or unknown type",
                         panel->name.c_str());
                continue;
            }
            Widget *raw = wi.get();
            panel->by_id[wi->id] = raw;
            panel->widgets.push_back(std::move(wi));
        }
    }

    cJSON_Delete(root);
    ESP_LOGI(TAG, "Loaded panel '%s': %dx%d, %d widgets",
             panel->name.c_str(), panel->cols, panel->rows, (int)panel->widgets.size());
    _panels.push_back(std::move(panel));
    return true;
}

void AppControlPanel::buildUi(lv_obj_t *parent)
{
    lv_obj_set_style_bg_color(parent, lv_color_hex(0x10141A), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    if (_panels.empty()) {
        lv_obj_t *l = lv_label_create(parent);
        lv_label_set_text(l, "No panels found in /spiffs/panels/\n"
                             "Drop a *.json file there and re-enter.");
        lv_obj_center(l);
        lv_obj_set_style_text_color(l, lv_color_white(), 0);
        return;
    }

    _tabview = lv_tabview_create(parent, LV_DIR_TOP, 56);
    lv_obj_set_size(_tabview, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(_tabview, lv_color_hex(0x10141A), 0);

    lv_obj_t *tab_btns = lv_tabview_get_tab_btns(_tabview);
    lv_obj_set_style_bg_color(tab_btns, lv_color_hex(0x1E2228), 0);
    lv_obj_set_style_text_color(tab_btns, lv_color_white(), 0);
    lv_obj_set_style_text_font(tab_btns, &lv_font_montserrat_22, 0);

    int content_h = lv_disp_get_ver_res(NULL) - 56 - 8;
    int content_w = lv_disp_get_hor_res(NULL);

    for (auto &p : _panels) {
        lv_obj_t *page = lv_tabview_add_tab(_tabview, p->title.c_str());
        lv_obj_set_style_pad_all(page, 4, 0);
        lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
        p->tab_page = page;

        int cell_w = (content_w - 16) / p->cols;
        int cell_h = (content_h - 16) / p->rows;

        for (auto &uptr : p->widgets) {
            renderWidget(page, *uptr, cell_w, cell_h);
        }
    }
}

void AppControlPanel::renderWidget(lv_obj_t *grid_parent, Widget &w, int cell_w_px, int cell_h_px)
{
    int x = 4 + w.col * cell_w_px;
    int y = 4 + w.row * cell_h_px;
    int wpx = w.w * cell_w_px - 8;
    int hpx = w.h * cell_h_px - 8;
    if (wpx < 40) wpx = 40;
    if (hpx < 40) hpx = 40;

    switch (w.type) {
        case WidgetType::Button: {
            lv_obj_t *btn = lv_btn_create(grid_parent);
            lv_obj_set_pos(btn, x, y);
            lv_obj_set_size(btn, wpx, hpx);

            lv_obj_set_style_min_height(btn, hpx, 0);
            lv_obj_set_style_max_height(btn, hpx, 0);
            lv_obj_set_style_min_width(btn, wpx, 0);
            lv_obj_t *lbl = lv_label_create(btn);
            lv_label_set_text(lbl, w.title.c_str());
            lv_obj_set_style_text_font(lbl, &lv_font_montserrat_22, 0);
            lv_obj_center(lbl);
            lv_obj_set_user_data(btn, &w);
            lv_obj_add_event_cb(btn, onUiButtonClicked, LV_EVENT_CLICKED, this);
            w.lv_obj = btn;
            break;
        }
        case WidgetType::Toggle: {
            lv_obj_t *cont = lv_obj_create(grid_parent);
            lv_obj_set_pos(cont, x, y);
            lv_obj_set_size(cont, wpx, hpx);
            lv_obj_set_style_bg_color(cont, lv_color_hex(0x1A1F26), 0);
            lv_obj_set_style_border_width(cont, 1, 0);
            lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE);

            lv_obj_t *lbl = lv_label_create(cont);
            lv_label_set_text(lbl, w.title.c_str());
            lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 4, 0);
            lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
            lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);

            lv_obj_t *sw = lv_switch_create(cont);
            lv_obj_set_size(sw, 70, 36);
            lv_obj_set_style_min_height(sw, 36, 0);
            lv_obj_align(sw, LV_ALIGN_RIGHT_MID, -4, 0);
            if (std::holds_alternative<bool>(w.value) && std::get<bool>(w.value)) {
                lv_obj_add_state(sw, LV_STATE_CHECKED);
            }
            lv_obj_set_user_data(sw, &w);
            lv_obj_add_event_cb(sw, onUiToggleChanged, LV_EVENT_VALUE_CHANGED, this);
            w.lv_obj = sw;
            break;
        }
        case WidgetType::Slider: {
            lv_obj_t *cont = lv_obj_create(grid_parent);
            lv_obj_set_pos(cont, x, y);
            lv_obj_set_size(cont, wpx, hpx);
            lv_obj_set_style_bg_color(cont, lv_color_hex(0x1A1F26), 0);
            lv_obj_set_style_border_width(cont, 1, 0);
            lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE);

            lv_obj_t *lbl = lv_label_create(cont);
            lv_label_set_text(lbl, w.title.c_str());
            lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, 4, 4);
            lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
            lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);

            lv_obj_t *sl = lv_slider_create(cont);
            lv_obj_set_width(sl, wpx - 24);
            lv_obj_set_style_min_height(sl, 12, 0);
            lv_obj_align(sl, LV_ALIGN_BOTTOM_MID, 0, -8);
            lv_slider_set_range(sl, (int)w.min, (int)w.max);
            if (std::holds_alternative<double>(w.value)) {
                lv_slider_set_value(sl, (int)std::get<double>(w.value), LV_ANIM_OFF);
            }
            lv_obj_set_user_data(sl, &w);
            lv_obj_add_event_cb(sl, onUiSliderChanged, LV_EVENT_VALUE_CHANGED, this);
            w.lv_obj = sl;
            break;
        }
        case WidgetType::Label: {
            lv_obj_t *cont = lv_obj_create(grid_parent);
            lv_obj_set_pos(cont, x, y);
            lv_obj_set_size(cont, wpx, hpx);
            lv_obj_set_style_bg_color(cont, lv_color_hex(0x1A1F26), 0);
            lv_obj_set_style_border_width(cont, 1, 0);
            lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE);

            lv_obj_t *t = lv_label_create(cont);
            lv_label_set_text(t, w.title.c_str());
            lv_obj_align(t, LV_ALIGN_TOP_LEFT, 4, 4);
            lv_obj_set_style_text_color(t, lv_color_hex(0xA0A0A0), 0);
            lv_obj_set_style_text_font(t, &lv_font_montserrat_18, 0);

            lv_obj_t *v = lv_label_create(cont);
            const std::string &s = std::holds_alternative<std::string>(w.value)
                ? std::get<std::string>(w.value) : std::string("");
            lv_label_set_text(v, s.c_str());
            lv_obj_align(v, LV_ALIGN_BOTTOM_LEFT, 4, -4);
            lv_obj_set_style_text_color(v, lv_color_hex(0x80E080), 0);
            lv_obj_set_style_text_font(v, &lv_font_montserrat_22, 0);
            w.lv_obj = v;
            break;
        }
        default: break;
    }
}

void AppControlPanel::applyValueToWidget(Widget &w)
{
    if (!w.lv_obj || !lv_obj_is_valid(w.lv_obj)) return;
    switch (w.type) {
        case WidgetType::Toggle:
            if (std::holds_alternative<bool>(w.value)) {
                if (std::get<bool>(w.value)) lv_obj_add_state(w.lv_obj, LV_STATE_CHECKED);
                else                          lv_obj_clear_state(w.lv_obj, LV_STATE_CHECKED);
            }
            break;
        case WidgetType::Slider:
            if (std::holds_alternative<double>(w.value)) {
                lv_slider_set_value(w.lv_obj, (int)std::get<double>(w.value), LV_ANIM_OFF);
            }
            break;
        case WidgetType::Label:
            if (std::holds_alternative<std::string>(w.value)) {
                lv_label_set_text(w.lv_obj, std::get<std::string>(w.value).c_str());
            }
            break;
        default: break;
    }
}

AppControlPanel::Panel *AppControlPanel::findPanel(const std::string &name)
{
    for (auto &p : _panels) if (p->name == name) return p.get();
    return nullptr;
}

const AppControlPanel::Panel *AppControlPanel::findPanel(const std::string &name) const
{
    for (const auto &p : _panels) if (p->name == name) return p.get();
    return nullptr;
}

bool AppControlPanel::setWidgetValue(const std::string &panel_name,
                                     const std::string &id,
                                     const std::string &json_value)
{
    Panel *p = findPanel(panel_name);
    if (!p) return false;
    auto it = p->by_id.find(id);
    if (it == p->by_id.end()) return false;
    Widget &w = *it->second;

    cJSON *parsed = cJSON_Parse(json_value.c_str());
    if (!parsed) return false;
    bool ok = true;
    switch (w.type) {
        case WidgetType::Toggle:
            if (cJSON_IsBool(parsed))   w.value = (bool)cJSON_IsTrue(parsed);
            else                         ok = false;
            break;
        case WidgetType::Slider:
            if (cJSON_IsNumber(parsed)) w.value = parsed->valuedouble;
            else                         ok = false;
            break;
        case WidgetType::Label:
            if (cJSON_IsString(parsed)) w.value = std::string(parsed->valuestring);
            else                         ok = false;
            break;
        case WidgetType::Button:
            w.value = true;

            if (!w.webhook.empty()) fireWebhook(w.webhook);
            break;
        default: ok = false; break;
    }
    cJSON_Delete(parsed);

    if (ok) {
        bsp_display_lock(0);
        applyValueToWidget(w);
        bsp_display_unlock();
    }
    return ok;
}

std::string AppControlPanel::snapshotPanelList(void) const
{
    cJSON *arr = cJSON_CreateArray();
    for (const auto &p : _panels) {
        cJSON_AddItemToArray(arr, cJSON_CreateString(p->name.c_str()));
    }
    char *s = cJSON_PrintUnformatted(arr);
    std::string out = s ? s : "[]";
    cJSON_free(s);
    cJSON_Delete(arr);
    return out;
}

bool AppControlPanel::isValidPanelName(const std::string &name)
{
    if (name.empty() || name.size() > 32) return false;
    for (char c : name) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
               || (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

std::string AppControlPanel::readConfig(const std::string &name) const
{
    if (!isValidPanelName(name)) return "";
    std::string path = std::string(PANELS_DIR) + "/" + name + ".json";
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return "";
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > 32 * 1024) { fclose(f); return ""; }
    std::string out(n, 0);
    fread(&out[0], 1, n, f);
    fclose(f);
    return out;
}

bool AppControlPanel::saveConfig(const std::string &name, const std::string &json_text)
{
    if (!isValidPanelName(name))         return false;
    if (json_text.empty() || json_text.size() > 32 * 1024) return false;

    cJSON *parsed = cJSON_Parse(json_text.c_str());
    if (!parsed) return false;
    bool looks_like_panel = cJSON_HasObjectItem(parsed, "widgets");
    cJSON_Delete(parsed);
    if (!looks_like_panel) return false;

    mkdir(PANELS_DIR, 0775);

    std::string path = std::string(PANELS_DIR) + "/" + name + ".json";
    FILE *f = fopen(path.c_str(), "wb");
    if (!f) return false;
    size_t written = fwrite(json_text.data(), 1, json_text.size(), f);
    fclose(f);
    if (written != json_text.size()) {
        ESP_LOGE(TAG, "short write to %s: %u/%u", path.c_str(),
                 (unsigned)written, (unsigned)json_text.size());
        return false;
    }
    ESP_LOGI(TAG, "Saved %s (%u bytes)", path.c_str(), (unsigned)json_text.size());

    loadAllPanels();
    return true;
}

bool AppControlPanel::deleteConfig(const std::string &name)
{
    if (!isValidPanelName(name)) return false;
    std::string path = std::string(PANELS_DIR) + "/" + name + ".json";
    if (remove(path.c_str()) != 0) return false;
    ESP_LOGI(TAG, "Deleted %s", path.c_str());
    loadAllPanels();
    return true;
}

std::string AppControlPanel::snapshotPanelWidgets(const std::string &name) const
{
    const Panel *p = findPanel(name);
    if (!p) return "{}";
    cJSON *root = cJSON_CreateObject();
    for (const auto &uptr : p->widgets) {
        const Widget &w = *uptr;
        if (std::holds_alternative<bool>(w.value))
            cJSON_AddBoolToObject(root, w.id.c_str(), std::get<bool>(w.value));
        else if (std::holds_alternative<double>(w.value))
            cJSON_AddNumberToObject(root, w.id.c_str(), std::get<double>(w.value));
        else if (std::holds_alternative<std::string>(w.value))
            cJSON_AddStringToObject(root, w.id.c_str(), std::get<std::string>(w.value).c_str());
    }
    char *s = cJSON_PrintUnformatted(root);
    std::string out = s ? s : "{}";
    cJSON_free(s);
    cJSON_Delete(root);
    return out;
}

namespace {
constexpr int WEBHOOK_QUEUE_SLOTS  = 4;
constexpr int WEBHOOK_TIMEOUT_MS   = 1000;
constexpr int WEBHOOK_TASK_STACK   = 4096;
constexpr int WEBHOOK_TASK_PRIO    = 3;

QueueHandle_t s_webhook_queue = nullptr;

void webhook_worker_task(void *)
{
    char *url = nullptr;
    while (true) {
        if (xQueueReceive(s_webhook_queue, &url, portMAX_DELAY) != pdTRUE) continue;
        if (!url) continue;

        esp_http_client_config_t cfg = {};
        cfg.url        = url;
        cfg.timeout_ms = WEBHOOK_TIMEOUT_MS;
        cfg.method     = HTTP_METHOD_GET;
        esp_http_client_handle_t cli = esp_http_client_init(&cfg);
        if (cli) {
            esp_err_t err = esp_http_client_perform(cli);
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "webhook %s -> %d", url, esp_http_client_get_status_code(cli));
            } else {
                ESP_LOGW(TAG, "webhook %s failed: %s", url, esp_err_to_name(err));
            }
            esp_http_client_cleanup(cli);
        }
        free(url);
        url = nullptr;
    }
}
}

void AppControlPanel::fireWebhook(const std::string &url)
{

    if (s_webhook_queue == nullptr) {
        s_webhook_queue = xQueueCreate(WEBHOOK_QUEUE_SLOTS, sizeof(char *));
        if (!s_webhook_queue) {
            ESP_LOGE(TAG, "webhook queue alloc failed");
            return;
        }
        xTaskCreate(webhook_worker_task, "webhook", WEBHOOK_TASK_STACK,
                    nullptr, WEBHOOK_TASK_PRIO, nullptr);
    }

    char *dup = strdup(url.c_str());
    if (!dup) return;

    if (xQueueSend(s_webhook_queue, &dup, 0) != pdTRUE) {
        ESP_LOGW(TAG, "webhook queue full, dropping %s", dup);
        free(dup);
    }
}

void AppControlPanel::onUiButtonClicked(lv_event_t *e)
{
    Widget *w = static_cast<Widget *>(lv_obj_get_user_data(lv_event_get_target(e)));
    if (!w) return;
    ESP_LOGI(TAG, "button '%s' pressed", w->id.c_str());
    w->value = true;
    if (!w->webhook.empty()) fireWebhook(w->webhook);
}

void AppControlPanel::onUiToggleChanged(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    Widget *w = static_cast<Widget *>(lv_obj_get_user_data(sw));
    if (!w) return;
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    w->value = on;
    ESP_LOGI(TAG, "toggle '%s' -> %s", w->id.c_str(), on ? "true" : "false");
}

void AppControlPanel::onUiSliderChanged(lv_event_t *e)
{
    lv_obj_t *sl = lv_event_get_target(e);
    Widget *w = static_cast<Widget *>(lv_obj_get_user_data(sl));
    if (!w) return;
    double v = lv_slider_get_value(sl);
    w->value = v;
    ESP_LOGI(TAG, "slider '%s' -> %.2f", w->id.c_str(), v);
}

static esp_err_t http_root_handler(httpd_req_t *req)
{

    static const char html[] =
        "<!DOCTYPE html><html><head><title>ESP32-P4 Control Panel</title>"
        "<style>body{font-family:sans-serif;background:#10141A;color:#eee;}"
        "pre{background:#1A1F26;padding:1em;border-radius:6px;}h2{margin-top:1.5em;}</style></head>"
        "<body><h1>Control Panel state</h1><div id=panels></div>"
        "<script>async function tick(){"
        "const names=await(await fetch('/api/panels')).json();"
        "let html='';for(const n of names){"
        "const w=await(await fetch('/api/panels/'+n+'/widgets')).text();"
        "html+='<h2>'+n+'</h2><pre>'+w+'</pre>';}"
        "document.getElementById('panels').innerHTML=html;}"
        "tick();setInterval(tick,1000);</script>"
        "</body></html>";
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, html, sizeof(html) - 1);
}

static esp_err_t http_get_panels_handler(httpd_req_t *req)
{
    auto *app = AppControlPanel::_instance;
    if (!app) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no instance"); return ESP_OK; }
    std::string s = app->snapshotPanelList();
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, s.data(), s.size());
}

static bool parse_panel_uri(const char *uri, std::string &name_out, std::string &id_out)
{
    const char *prefix = "/api/panels/";
    size_t prefix_len = strlen(prefix);
    if (strncmp(uri, prefix, prefix_len) != 0) return false;
    const char *after = uri + prefix_len;
    const char *slash = strchr(after, '/');
    if (!slash) return false;
    name_out.assign(after, slash - after);
    const char *widgets = slash + 1;
    if (strncmp(widgets, "widgets", 7) != 0) return false;
    if (widgets[7] == 0) { id_out.clear(); return true; }
    if (widgets[7] != '/') return false;
    id_out.assign(widgets + 8);
    return true;
}

static esp_err_t http_get_widgets_handler(httpd_req_t *req)
{
    auto *app = AppControlPanel::_instance;
    if (!app) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no instance"); return ESP_OK; }

    std::string panel, id;
    if (!parse_panel_uri(req->uri, panel, id) || !id.empty()) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "bad uri");
        return ESP_OK;
    }
    std::string s = app->snapshotPanelWidgets(panel);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, s.data(), s.size());
}

static esp_err_t http_post_widget_handler(httpd_req_t *req)
{
    auto *app = AppControlPanel::_instance;
    if (!app) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no instance"); return ESP_OK; }

    std::string panel, id;
    if (!parse_panel_uri(req->uri, panel, id) || id.empty()) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "bad uri");
        return ESP_OK;
    }

    char buf[256];
    int n = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (n <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body"); return ESP_OK; }
    buf[n] = 0;

    cJSON *root = cJSON_Parse(buf);
    if (!root) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json"); return ESP_OK; }
    cJSON *v = cJSON_GetObjectItem(root, "value");
    if (!v) { cJSON_Delete(root); httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no value"); return ESP_OK; }
    char *vstr = cJSON_PrintUnformatted(v);
    std::string value_json(vstr ? vstr : "null");
    cJSON_free(vstr);
    cJSON_Delete(root);

    bool ok = app->setWidgetValue(panel, id, value_json);
    if (!ok) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "unknown panel/id or bad value");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static bool parse_config_uri(const char *uri, std::string &name_out)
{
    const char *prefix = "/api/configs/";
    size_t prefix_len = strlen(prefix);
    if (strncmp(uri, prefix, prefix_len) != 0) return false;
    const char *after = uri + prefix_len;
    if (*after == 0) return false;
    name_out.assign(after);
    return true;
}

static void http_set_cors(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
}

static esp_err_t http_options_handler(httpd_req_t *req)
{
    http_set_cors(req);
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, nullptr, 0);
    return ESP_OK;
}

static esp_err_t http_get_config_handler(httpd_req_t *req)
{
    auto *app = AppControlPanel::_instance;
    if (!app) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no instance"); return ESP_OK; }
    std::string name;
    if (!parse_config_uri(req->uri, name) || !AppControlPanel::isValidPanelName(name)) {
        http_set_cors(req);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad name");
        return ESP_OK;
    }
    std::string body = app->readConfig(name);
    http_set_cors(req);
    if (body.empty()) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no such panel");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body.data(), body.size());
}

static esp_err_t http_post_config_handler(httpd_req_t *req)
{
    auto *app = AppControlPanel::_instance;
    if (!app) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no instance"); return ESP_OK; }
    std::string name;
    if (!parse_config_uri(req->uri, name) || !AppControlPanel::isValidPanelName(name)) {
        http_set_cors(req);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad name");
        return ESP_OK;
    }

    if (req->content_len > 32 * 1024) {
        http_set_cors(req);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body too large (>32KB)");
        return ESP_OK;
    }
    std::string body;
    body.reserve(req->content_len);
    char chunk[512];
    int remaining = req->content_len;
    while (remaining > 0) {
        int got = httpd_req_recv(req, chunk, std::min<int>(remaining, (int)sizeof(chunk)));
        if (got <= 0) {
            http_set_cors(req);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv failed");
            return ESP_OK;
        }
        body.append(chunk, got);
        remaining -= got;
    }

    bool ok = app->saveConfig(name, body);
    http_set_cors(req);
    if (!ok) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid panel JSON or write failed");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t http_delete_config_handler(httpd_req_t *req)
{
    auto *app = AppControlPanel::_instance;
    if (!app) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no instance"); return ESP_OK; }
    std::string name;
    if (!parse_config_uri(req->uri, name) || !AppControlPanel::isValidPanelName(name)) {
        http_set_cors(req);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad name");
        return ESP_OK;
    }
    bool ok = app->deleteConfig(name);
    http_set_cors(req);
    if (!ok) { httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no such panel"); return ESP_OK; }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

void AppControlPanel::startHttpServer(void)
{
    if (_httpd) return;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    cfg.max_uri_handlers = 12;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    httpd_handle_t h = nullptr;
    if (httpd_start(&h, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        return;
    }
    _httpd = h;

    httpd_uri_t root   = { .uri = "/",                 .method = HTTP_GET,     .handler = http_root_handler,          .user_ctx = nullptr };
    httpd_uri_t panels = { .uri = "/api/panels",       .method = HTTP_GET,     .handler = http_get_panels_handler,    .user_ctx = nullptr };
    httpd_uri_t getw   = { .uri = "/api/panels/*",     .method = HTTP_GET,     .handler = http_get_widgets_handler,   .user_ctx = nullptr };
    httpd_uri_t postw  = { .uri = "/api/panels/*",     .method = HTTP_POST,    .handler = http_post_widget_handler,   .user_ctx = nullptr };
    httpd_uri_t getc   = { .uri = "/api/configs/*",    .method = HTTP_GET,     .handler = http_get_config_handler,    .user_ctx = nullptr };
    httpd_uri_t postc  = { .uri = "/api/configs/*",    .method = HTTP_POST,    .handler = http_post_config_handler,   .user_ctx = nullptr };
    httpd_uri_t delc   = { .uri = "/api/configs/*",    .method = HTTP_DELETE,  .handler = http_delete_config_handler, .user_ctx = nullptr };

    httpd_uri_t optc   = { .uri = "/api/configs/*",    .method = HTTP_OPTIONS, .handler = http_options_handler,       .user_ctx = nullptr };
    httpd_uri_t optp   = { .uri = "/api/panels/*",     .method = HTTP_OPTIONS, .handler = http_options_handler,       .user_ctx = nullptr };

    httpd_register_uri_handler(h, &root);
    httpd_register_uri_handler(h, &panels);
    httpd_register_uri_handler(h, &getw);
    httpd_register_uri_handler(h, &postw);
    httpd_register_uri_handler(h, &getc);
    httpd_register_uri_handler(h, &postc);
    httpd_register_uri_handler(h, &delc);
    httpd_register_uri_handler(h, &optc);
    httpd_register_uri_handler(h, &optp);

    ESP_LOGI(TAG, "Control Panel HTTP server listening on :80");
}

void AppControlPanel::stopHttpServer(void)
{
    if (!_httpd) return;
    httpd_stop((httpd_handle_t)_httpd);
    _httpd = nullptr;
}
