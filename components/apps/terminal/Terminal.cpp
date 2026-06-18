#include "Terminal.hpp"

#include <cstring>
#include <cstdio>
#include <sstream>
#include <vector>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_system.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_heap_caps.h"
#include "esp_chip_info.h"
#include "esp_app_desc.h"

#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/err.h"
#include "soc/lp_system_reg.h"
#include "soc/soc.h"

#include "bsp/esp-bsp.h"

LV_IMG_DECLARE(img_app_terminal);

static const char *TAG = "Terminal";

#define TELNET_PORT             23
#define TELNET_TASK_STACK_SIZE  (1024 * 6)
#define TELNET_TASK_PRIORITY    3
#define TELNET_RX_BUF           512
#define TELNET_TX_BUF           1024

bool AppTerminal::_telnet_started = false;

namespace {

void cmd_help(const std::vector<std::string> &, std::string &out)
{
    out += "Available commands:\r\n"
           "  help            - this listing\r\n"
           "  echo <text>     - echo arguments back\r\n"
           "  mem             - heap stats (internal + PSRAM)\r\n"
           "  sysinfo         - chip / firmware info\r\n"
           "  ip              - current STA IP address\r\n"
           "  wifi            - WiFi connection state\r\n"
           "  reboot          - normal restart\r\n"
           "  download        - reboot into USB-JTAG download mode\r\n"
           "  clear           - clear the on-device output\r\n";
}

void cmd_echo(const std::vector<std::string> &argv, std::string &out)
{
    for (size_t i = 1; i < argv.size(); ++i) {
        if (i > 1) out += ' ';
        out += argv[i];
    }
    out += "\r\n";
}

void cmd_mem(const std::vector<std::string> &, std::string &out)
{
    char buf[160];
    snprintf(buf, sizeof(buf),
             "internal: free %u / total %u KB\r\n"
             "psram:    free %u / total %u KB\r\n",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_total_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
             (unsigned)(heap_caps_get_total_size(MALLOC_CAP_SPIRAM) / 1024));
    out += buf;
}

void cmd_sysinfo(const std::vector<std::string> &, std::string &out)
{
    esp_chip_info_t info;
    esp_chip_info(&info);
    const esp_app_desc_t *app = esp_app_get_description();
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_EFUSE_FACTORY);

    char buf[256];
    snprintf(buf, sizeof(buf),
             "chip:     model=%d rev=%d cores=%d\r\n"
             "mac:      %02X:%02X:%02X:%02X:%02X:%02X\r\n"
             "fw:       %s (%s)\r\n",
             info.model, info.revision, info.cores,
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
             app ? app->project_name : "?", app ? app->version : "?");
    out += buf;
}

void cmd_ip(const std::vector<std::string> &, std::string &out)
{
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!sta) { out += "no STA netif\r\n"; return; }
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(sta, &ip) != ESP_OK) { out += "no IP\r\n"; return; }
    char buf[80];
    snprintf(buf, sizeof(buf), "ip=" IPSTR " gw=" IPSTR " mask=" IPSTR "\r\n",
             IP2STR(&ip.ip), IP2STR(&ip.gw), IP2STR(&ip.netmask));
    out += buf;
}

void cmd_wifi(const std::vector<std::string> &, std::string &out)
{
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        char buf[128];
        snprintf(buf, sizeof(buf), "connected ssid=%s rssi=%d ch=%d\r\n",
                 (const char *)ap.ssid, ap.rssi, ap.primary);
        out += buf;
    } else {
        out += "not connected\r\n";
    }
}

void cmd_reboot(const std::vector<std::string> &, std::string &out)
{
    out += "rebooting...\r\n";
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
}

void cmd_download(const std::vector<std::string> &, std::string &out)
{
    out += "entering USB-JTAG download mode...\r\n";
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(200));
    REG_SET_BIT(LP_SYSTEM_REG_SYS_CTRL_REG, LP_SYSTEM_REG_FORCE_DOWNLOAD_BOOT);
    esp_restart();
}

std::vector<std::string> tokenize(const std::string &line)
{
    std::vector<std::string> out;
    std::istringstream iss(line);
    std::string tok;
    while (iss >> tok) out.push_back(tok);
    return out;
}

}

void AppTerminal::dispatchCommand(const std::string &line, std::string &out)
{
    auto argv = tokenize(line);
    if (argv.empty()) return;
    const std::string &cmd = argv[0];

    if      (cmd == "help")     cmd_help(argv, out);
    else if (cmd == "echo")     cmd_echo(argv, out);
    else if (cmd == "mem")      cmd_mem(argv, out);
    else if (cmd == "sysinfo")  cmd_sysinfo(argv, out);
    else if (cmd == "ip")       cmd_ip(argv, out);
    else if (cmd == "wifi")     cmd_wifi(argv, out);
    else if (cmd == "reboot")   cmd_reboot(argv, out);
    else if (cmd == "download") cmd_download(argv, out);
    else if (cmd == "clear")    {   }
    else                        out += "unknown command: " + cmd + " (try 'help')\r\n";
}

void AppTerminal::telnetServerTask(void *)
{
    int listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_sock < 0) {
        ESP_LOGE(TAG, "telnet socket failed errno=%d", errno);
        vTaskDelete(NULL);
        return;
    }
    int yes = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in bind_addr = {};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    bind_addr.sin_port = htons(TELNET_PORT);
    if (bind(listen_sock, (sockaddr*)&bind_addr, sizeof(bind_addr)) != 0) {
        ESP_LOGE(TAG, "telnet bind failed errno=%d", errno);
        ::close(listen_sock);
        vTaskDelete(NULL);
        return;
    }
    if (listen(listen_sock, 1) != 0) {
        ESP_LOGE(TAG, "telnet listen failed errno=%d", errno);
        ::close(listen_sock);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "telnet listening on :%d", TELNET_PORT);

    char rxbuf[TELNET_RX_BUF];
    std::string line;

    while (true) {
        sockaddr_in peer = {};
        socklen_t peer_len = sizeof(peer);
        int client = accept(listen_sock, (sockaddr*)&peer, &peer_len);
        if (client < 0) {
            ESP_LOGW(TAG, "telnet accept errno=%d", errno);
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        ESP_LOGI(TAG, "telnet client connected");

        const char banner[] =
            "ESP_Brookesia terminal\r\n"
            "Type 'help' for commands.\r\n"
            "> ";
        send(client, banner, sizeof(banner) - 1, 0);
        line.clear();

        while (true) {
            int n = recv(client, rxbuf, sizeof(rxbuf), 0);
            if (n <= 0) break;

            for (int i = 0; i < n; ++i) {
                unsigned char c = (unsigned char)rxbuf[i];

                if (c == 0xFF && i + 2 < n) { i += 2; continue; }
                if (c == '\r') continue;
                if (c == '\n') {
                    std::string out;
                    dispatchCommand(line, out);
                    if (!out.empty()) send(client, out.data(), out.size(), 0);
                    send(client, "> ", 2, 0);
                    line.clear();
                } else if (c == 0x7F || c == 0x08) {
                    if (!line.empty()) line.pop_back();
                } else if (c >= 0x20 && c < 0x7F) {
                    line.push_back((char)c);
                }
            }
        }

        ESP_LOGI(TAG, "telnet client disconnected");
        ::close(client);
    }
}

AppTerminal::AppTerminal()

    : ESP_Brookesia_PhoneApp("Terminal", &img_app_terminal, true),
      _screen(nullptr), _output(nullptr), _input(nullptr), _keyboard(nullptr)
{
}

AppTerminal::~AppTerminal() = default;

bool AppTerminal::init(void)
{

    if (!_telnet_started) {
        _telnet_started = true;
        xTaskCreate(telnetServerTask, "telnet", TELNET_TASK_STACK_SIZE,
                    nullptr, TELNET_TASK_PRIORITY, nullptr);
    }
    return true;
}

bool AppTerminal::run(void)
{

    buildUi(lv_scr_act());
    return true;
}

bool AppTerminal::back(void)
{
    return notifyCoreClosed();
}

bool AppTerminal::close(void)
{

    _screen = _output = _input = _keyboard = nullptr;
    return true;
}

bool AppTerminal::pause(void)  { return true; }
bool AppTerminal::resume(void) { return true; }

void AppTerminal::buildUi(lv_obj_t *parent)
{
    _screen = parent;
    lv_obj_set_style_bg_color(_screen, lv_color_hex(0x101418), 0);
    lv_obj_set_style_bg_opa(_screen, LV_OPA_COVER, 0);

    _output = lv_textarea_create(_screen);
    lv_obj_set_size(_output, lv_pct(100), lv_pct(45));
    lv_obj_align(_output, LV_ALIGN_TOP_MID, 0, 0);
    lv_textarea_set_one_line(_output, false);
    lv_obj_set_style_text_font(_output, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(_output, lv_color_hex(0x40E040), 0);
    lv_obj_set_style_bg_color(_output, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_width(_output, 0, 0);
    lv_obj_clear_state(_output, LV_STATE_FOCUSED);
    lv_textarea_set_text(_output, "ESP_Brookesia terminal\nType 'help' for commands.\n");

    _input = lv_textarea_create(_screen);
    lv_obj_set_size(_input, lv_pct(100), 55);
    lv_obj_align(_input, LV_ALIGN_TOP_MID, 0, lv_pct(45));
    lv_textarea_set_one_line(_input, true);
    lv_textarea_set_placeholder_text(_input, "> command, then tap Enter");
    lv_obj_set_style_text_font(_input, &lv_font_montserrat_22, 0);
    lv_obj_add_event_cb(_input, onUiInputReadyCallback, LV_EVENT_READY, this);

    _keyboard = lv_keyboard_create(_screen);
    lv_obj_set_size(_keyboard, lv_pct(100), lv_pct(45));
    lv_obj_align(_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(_keyboard, _input);
    lv_obj_add_event_cb(_keyboard, onUiKeyboardCallback, LV_EVENT_VALUE_CHANGED, this);

    appendOutputPrompt();
}

void AppTerminal::appendOutput(const std::string &text)
{
    if (!_output) return;
    lv_textarea_add_text(_output, text.c_str());
}

void AppTerminal::appendOutputPrompt(void)
{
    appendOutput("> ");
}

void AppTerminal::onUiInputReadyCallback(lv_event_t *e)
{
    AppTerminal *app = static_cast<AppTerminal *>(lv_event_get_user_data(e));
    if (!app || !app->_input) return;

    const char *raw = lv_textarea_get_text(app->_input);
    std::string line(raw ? raw : "");

    app->appendOutput(line + "\n");

    if (line == "clear") {
        lv_textarea_set_text(app->_output, "");
    } else {
        std::string out;
        dispatchCommand(line, out);
        if (!out.empty()) {

            std::string ui_out;
            ui_out.reserve(out.size());
            for (char c : out) if (c != '\r') ui_out.push_back(c);
            app->appendOutput(ui_out);
        }
    }

    lv_textarea_set_text(app->_input, "");
    app->appendOutputPrompt();
}

void AppTerminal::onUiKeyboardCallback(lv_event_t *e)
{

    (void)e;
}
