/*
 * tui_log_hook.h - capture ESP_LOG output into the TUI log page.
 *
 * Without this hook, every ESP_LOGI/W/E call writes raw text to UART0
 * mid-frame, corrupting whatever cell the TUI cursor is positioned on.
 * The hook intercepts via esp_log_set_vprintf(), strips ESP-IDF's leading
 * ANSI colour, classifies by level letter, trims trailing escapes, and
 * funnels the cleaned text into tui_log() with an appropriate colour.
 *
 * Install this BEFORE any other subsystem logs - in practice, first call
 * in app_main(). Boot-bootloader logs that happen before main are not
 * captured (they're printed by ROM code), but the full-screen clear in
 * tui_init() wipes them when the UI starts.
 */
#ifndef TUI_LOG_HOOK_H
#define TUI_LOG_HOOK_H

#ifdef __cplusplus
extern "C" {
#endif

void tui_install_log_hook(void);

#ifdef __cplusplus
}
#endif

#endif
