#include "ls_ctl.h"
#include "settings.h"
#include "scan_channels.h"
#include "scan_engine.h"
#include "lakeshark_backend.h"
#include "esp_console.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_attr.h"
#include "esp_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "ls_ctl";

static int cmd_home(int argc, char **argv)
{
    if (argc < 3) {
        float lat = 0, lon = 0;
        if (settings_get_home(&lat, &lon)) printf("home=%.5f,%.5f\n", lat, lon);
        else printf("home=unset (usage: home <lat> <lon>)\n");
        return 0;
    }
    float lat = (float)atof(argv[1]);
    float lon = (float)atof(argv[2]);
    if (lat < -90.0f || lat > 90.0f || lon < -180.0f || lon > 180.0f) {
        printf("bad coords (lat -90..90, lon -180..180)\n");
        return 0;
    }
    settings_set_home(lat, lon);
    printf("home=%.5f,%.5f\n", lat, lon);
    return 0;
}

static void ch_list(void)
{
    int n = scan_channels_count();
    printf("channels: %d\n", n);
    for (int i = 0; i < n; i++) {
        const scan_channel_t *c = scan_channel_get(i);
        if (!c) continue;
        printf("  %2d  %-12s %9.4f MHz  %-3s z%d%s%s%s\n",
               i, c->name, c->freq_hz / 1e6, scan_mode_name(c->mode), c->zone,
               (c->flags & SCAN_FLAG_ENABLED)  ? "" : " off",
               (c->flags & SCAN_FLAG_LOCKOUT)  ? " lock" : "",
               (c->flags & SCAN_FLAG_PRIORITY) ? " pri" : "");
    }
}

static int cmd_ch(int argc, char **argv)
{
    if (argc < 2 || !strcmp(argv[1], "list")) { ch_list(); return 0; }

    if (!strcmp(argv[1], "add")) {
        if (argc < 4) { printf("usage: ch add <MHz> <p25|nfm|wfm> [name] [zone]\n"); return 0; }
        uint32_t hz = (uint32_t)(atof(argv[2]) * 1e6 + 0.5);
        int mode = scan_mode_parse(argv[3]);
        if (mode < 0) { printf("bad mode (p25|nfm|wfm)\n"); return 0; }
        const char *name = (argc > 4) ? argv[4] : NULL;
        int zone = (argc > 5) ? atoi(argv[5]) : 0;
        int idx = scan_channel_add(name, hz, (scan_mode_t)mode, (uint8_t)zone);
        if (idx < 0) printf("add failed (list full or invalid freq)\n");
        else printf("added ch %d\n", idx);
        return 0;
    }
    if (!strcmp(argv[1], "del")) {
        if (argc < 3) { printf("usage: ch del <idx>\n"); return 0; }
        printf("%s\n", scan_channel_remove(atoi(argv[2])) ? "removed" : "bad idx");
        return 0;
    }
    if (!strcmp(argv[1], "lock") || !strcmp(argv[1], "pri") || !strcmp(argv[1], "en")) {
        if (argc < 4) { printf("usage: ch %s <idx> on|off\n", argv[1]); return 0; }
        int idx = atoi(argv[2]);
        bool on = !strcmp(argv[3], "on") || !strcmp(argv[3], "1");
        bool ok;
        if      (!strcmp(argv[1], "lock")) ok = scan_channel_set_lockout(idx, on);
        else if (!strcmp(argv[1], "pri"))  ok = scan_channel_set_priority(idx, on);
        else                               ok = scan_channel_set_enabled(idx, on);
        printf("%s\n", ok ? "ok" : "bad idx");
        return 0;
    }
    if (!strcmp(argv[1], "clear")) { scan_channels_clear(); printf("cleared\n"); return 0; }

    printf("usage: ch [list|add|del|lock|pri|en|clear]\n");
    return 0;
}

static int cmd_scan(int argc, char **argv)
{
    if (argc < 2 || !strcmp(argv[1], "status")) {
        char st[96]; scan_engine_status(st, sizeof(st));
        printf("scan: %s\n", st);
        return 0;
    }
    if (!strcmp(argv[1], "on")  || !strcmp(argv[1], "start")) { scan_engine_start(); printf("scan on\n");  return 0; }
    if (!strcmp(argv[1], "off") || !strcmp(argv[1], "stop"))  { scan_engine_stop();  printf("scan off\n"); return 0; }
    if (!strcmp(argv[1], "skip")) { scan_engine_skip(); printf("skip\n"); return 0; }
    if (!strcmp(argv[1], "hang")) {
        if (argc < 3) { printf("usage: scan hang <ms>\n"); return 0; }
        scan_engine_set_hang_ms(atoi(argv[2])); printf("hang=%d ms\n", atoi(argv[2])); return 0;
    }
    if (!strcmp(argv[1], "thresh")) {
        if (argc < 3) { printf("usage: scan thresh <dB-over-floor>\n"); return 0; }
        scan_engine_set_threshold_db((float)atof(argv[2])); printf("thresh=%s dB\n", argv[2]); return 0;
    }
    printf("usage: scan [on|off|status|skip|hang <ms>|thresh <db>]\n");
    return 0;
}

static int cmd_p25gate(int argc, char **argv)
{
    if (argc < 2) {
        printf("p25 voice gate = %d (lower mutes more error frames; 99 = play all)\n",
               lakeshark_p25_voice_gate());
        return 0;
    }
    lakeshark_p25_set_voice_gate(atoi(argv[1]));
    printf("p25 voice gate = %d\n", lakeshark_p25_voice_gate());
    return 0;
}

void ls_ctl_register_commands(void)
{
    const esp_console_cmd_t cmds[] = {
        { .command = "p25gate", .help = "P25 voice error gate (lower=mute weak frames)",
          .hint = "<0-99>", .func = &cmd_p25gate },
        { .command = "home", .help = "Get/set home QTH for the radar",
          .hint = "<lat> <lon>", .func = &cmd_home },
        { .command = "ch", .help = "Scanner channel list",
          .hint = "list|add|del|lock|pri|en|clear", .func = &cmd_ch },
        { .command = "scan", .help = "Channel scanner control",
          .hint = "on|off|status|skip|hang <ms>|thresh <db>", .func = &cmd_scan },
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
        esp_console_cmd_register(&cmds[i]);
}

static void cli_task(void *arg)
{
    (void)arg;
    char line[160];
    int  len = 0;
    uint8_t ch;
    for (;;) {
        int n = uart_read_bytes(CONFIG_ESP_CONSOLE_UART_NUM, &ch, 1, portMAX_DELAY);
        if (n != 1) continue;
        if (ch == '\r' || ch == '\n') {
            printf("\n");
            if (len == 0) continue;
            line[len] = 0;
            len = 0;
            int ret = 0;
            esp_err_t e = esp_console_run(line, &ret);
            if (e == ESP_ERR_NOT_FOUND) printf("unknown command (try 'help')\n");
            else if (e == ESP_ERR_INVALID_ARG) { }
            else if (e != ESP_OK) printf("error: %s\n", esp_err_to_name(e));
        } else if ((ch == 0x7f || ch == 0x08) && len > 0) {
            len--;
            printf("\b \b"); fflush(stdout);
        } else if (ch >= 0x20 && len < (int)sizeof(line) - 1) {
            line[len++] = (char)ch;
            putchar((char)ch); fflush(stdout);
        }
    }
}

void ls_ctl_start_repl(void)
{
    esp_console_config_t ccfg = ESP_CONSOLE_CONFIG_DEFAULT();
    if (esp_console_init(&ccfg) != ESP_OK) {
        ESP_LOGW(TAG, "console init failed");
        return;
    }
    esp_console_register_help_command();
    ls_ctl_register_commands();

    if (!uart_is_driver_installed(CONFIG_ESP_CONSOLE_UART_NUM)) {
        uart_driver_install(CONFIG_ESP_CONSOLE_UART_NUM, 512, 0, 0, NULL, 0);
    }
    static EXT_RAM_BSS_ATTR StackType_t cli_stack[4096 / sizeof(StackType_t)];
    static StaticTask_t cli_tcb;
    xTaskCreateStaticPinnedToCore(cli_task, "ls_cli", 4096 / sizeof(StackType_t),
                                  NULL, 3, cli_stack, &cli_tcb, 0);
    printf("\nlakeshark CLI ready (type 'help')\n");
}
