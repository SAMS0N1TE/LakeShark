/* A boot log for the host: sample lines in place of the chip's log hook, so
   BOOT LOG has a real buffer to show. LSSIM_BOOTLOG_EMPTY=1 leaves it empty;
   LSSIM_BOOTLOG_LONG=1 runs it past the 40 KB that are kept whole. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ls_bootlog.h"

static void line(const char *s) { ls_bootlog_add(s, strlen(s)); }

__attribute__((constructor)) static void fake_bootlog_start(void)
{
    if (getenv("LSSIM_BOOTLOG_EMPTY")) return;
    line("LakeShark boot log\n");
    line("lakeshark 2.21.4-bench  board p4-nano-v3-dsi  built Nov 13 2026  IDF v5.5.4\n");
    line("reset: power on (1)\n");
    line("I (412) headless: console block held (4352 bytes, LP RAM)\n");
    line("I (498) safe: boot 1 since the last healthy run\n");
    line("W (731) headless: no SPIFFS (ESP_ERR_NOT_FOUND) - running without it\n");
    line("I (902) headless: SD card mounted at /sdcard\n");
    line("E (1310) lcd_panel.dsi: esp_lcd_new_panel_dpi(292): no response from the panel, DSI link down\n");
    line("W (1312) tdp_ui: panel did not answer - the desk is on the serial link only\n");
    line("I (1480) codec: ES8311 at 0x18\n");
    char s[96];
    const int n = getenv("LSSIM_BOOTLOG_LONG") ? 1400 : 40;
    for (int i = 0; i < n; i++) {
        snprintf(s, sizeof(s), "%c (%d) radio: step %d of the bring-up\n", i % 17 == 16 ? 'W' : 'I', 1500 + i * 9, i);
        line(s);
    }
    line("I (2210) headless: console ready - type 'help' for commands\n");
}
