#include "ls_ctl.h"
#include "settings.h"
#include "scan_channels.h"
#include "scan_engine.h"
#include "lakeshark_backend.h"
#include "audio_out.h"
#include "esp_console.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
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

static void eq_print(void)
{
    printf("eq=%s  hpf=%.0f Hz  bass=%+.1f  mid=%+.1f  treble=%+.1f dB\n",
           audio_voice_eq_enabled() ? "on" : "off", audio_voice_eq_hpf(),
           audio_voice_eq_bass_db(), audio_voice_eq_mid_db(), audio_voice_eq_treble_db());
}

static int cmd_eq(int argc, char **argv)
{
    if (argc < 2) {
        eq_print();
        printf("usage: eq on|off | eq hpf <Hz> | eq bass|mid|treble <dB>\n");
        return 0;
    }
    if (!strcmp(argv[1], "on"))  { audio_voice_eq_enable(true);  eq_print(); return 0; }
    if (!strcmp(argv[1], "off")) { audio_voice_eq_enable(false); eq_print(); return 0; }
    if (argc >= 3) {
        float v = (float)atof(argv[2]);
        if      (!strcmp(argv[1], "hpf"))    audio_voice_eq_set_hpf(v);
        else if (!strcmp(argv[1], "bass"))   audio_voice_eq_set_bass(v);
        else if (!strcmp(argv[1], "mid"))    audio_voice_eq_set_mid(v);
        else if (!strcmp(argv[1], "treble")) audio_voice_eq_set_treble(v);
        else { printf("usage: eq on|off | eq hpf <Hz> | eq bass|mid|treble <dB>\n"); return 0; }
        eq_print();
        return 0;
    }
    printf("usage: eq on|off | eq hpf <Hz> | eq bass|mid|treble <dB>\n");
    return 0;
}

static int cmd_rx(int argc, char **argv)
{
    (void)argc; (void)argv;
    lakeshark_rx_telem_t t;
    lakeshark_p25_rx_telem(&t);

    static int     prev_vc = -1;
    static int64_t last_change = 0;
    int64_t now = esp_timer_get_time();
    if (prev_vc >= 0 && t.voice_count != prev_vc) last_change = now;
    prev_vc = t.voice_count;
    int active = (last_change != 0) && (now - last_change < 1500000);

    int tot = t.bch_ok + t.bch_fail;
    int okpct = tot > 0 ? (t.bch_ok * 100) / tot : 0;

    printf("$RX active=%d nac=%03X tg=%d src=%d enc=%d sync=%d syncn=%d mod=%s "
           "ok=%d fail=%d okpct=%d lvl=%.3f dm=%.1f vox=%d drops=%u under=%u "
           "freq=%.4f vol=%d gain=%.1f mute=%d "
           "eq=%d hpf=%.0f bass=%.1f mid=%.1f treble=%.1f fint=%u fpsram=%u\n",
           active, (unsigned)(t.nac & 0xFFF), t.tg, t.src, t.enc,
           t.has_sync, t.sync_count, t.modulation,
           t.bch_ok, t.bch_fail, okpct, (double)t.iq_level, (double)t.decode_ms,
           t.voice_count, (unsigned)t.drops, (unsigned)t.under,
           lakeshark_p25_get_freq() / 1e6,
           audio_volume_get(), lakeshark_radio_get_gain_tenths() / 10.0,
           audio_is_muted() ? 1 : 0,
           audio_voice_eq_enabled() ? 1 : 0, (double)audio_voice_eq_hpf(),
           (double)audio_voice_eq_bass_db(), (double)audio_voice_eq_mid_db(),
           (double)audio_voice_eq_treble_db(),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    return 0;
}

void ls_ctl_register_commands(void)
{
    const esp_console_cmd_t cmds[] = {
        { .command = "eq", .help = "P25 voice EQ (highpass + bass/mid/treble)",
          .hint = "on|off|hpf <Hz>|bass|mid|treble <dB>", .func = &cmd_eq },
        { .command = "rx", .help = "P25 RX telemetry (active/nac/level/decode health)",
          .func = &cmd_rx },
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
