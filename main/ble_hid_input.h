#pragma once
#include "ble_hid_report.h"
#include "tui/ls_input_ev.h"
typedef bool (*hid_input_sink_t)(const ls_input_ev_t *, void *);
typedef struct {
    uint8_t keys[2][HID_REPORTS_MAX][32], buttons[2][HID_REPORTS_MAX];
    uint16_t consumer[2][HID_REPORTS_MAX][8];
    uint8_t held[32], held_buttons;
    uint32_t next_repeat[256], seq, dropped;
    ls_input_ev_t pressed[256]; /* release uses the same key/character as down */
    int32_t x, y;
    int cols, rows, cw, ch;
    bool caps, num, resync;
} hid_input_t;
void hid_input_report(hid_input_t *, unsigned peer, unsigned report, const hid_sample_t *,
                      uint32_t now_ms, int cols, int rows, int cw, int ch, hid_input_sink_t,
                      void *);
void hid_input_release(hid_input_t *, unsigned peer, uint32_t now_ms, hid_input_sink_t, void *);
void hid_input_repeat(hid_input_t *, uint32_t now_ms, hid_input_sink_t, void *);
