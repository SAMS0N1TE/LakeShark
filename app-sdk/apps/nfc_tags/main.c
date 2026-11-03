/* API 2.4: bounded UID/NDEF access, no raw bus or authentication keys. */
#include "lakeshark_app.h"
#include <stdio.h>
static void print(int x, int y, uint8_t attr, const char *fmt, ...) {
    char text[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    lap_put_str(x, y, text, attr);
}

static int h, error;
static lap_nfc_event_t tag;
static uint8_t ndef[2048];
static void enter(void) {
    h = 0;
    error = 0;
    tag = (lap_nfc_event_t){0};
    lap_caps_t c;
    if (!lap_caps(&c, sizeof(c)) && (c.hardware & LAP_HW_NFC)) {
        h = lap_nfc_open();
        if (h < 0) {
            error = h;
            h = 0;
        }
    }
}
static void leave(void) {
    if (h > 0)
        lap_nfc_close(h);
    h = 0;
}
static void draw(void) {
    if (!h && !(lap_frame() % 25)) {
        lap_caps_t c;
        lap_caps(&c, sizeof(c));
        if (c.hardware & LAP_HW_NFC) {
            h = lap_nfc_open();
            if (h < 0) {
                error = h;
                h = 0;
            }
        }
    }
    if (h > 0) {
        lap_nfc_event_t e;
        int n = lap_nfc_next(h, &e, ndef, sizeof(ndef));
        if (n > 0)
            tag = e;
        else if (n < 0)
            error = n;
    }
    lap_put_str(1, 1, "NFC TAGS / UID + NDEF", LAP_ATTR(LAP_CYAN, LAP_BLACK));
    print(1, 3, LAP_ATTR(LAP_WHITE, LAP_BLACK), "%s event %lu UID %u bytes NDEF %u error %d",
          h ? "ready" : "front end unavailable", (unsigned long)tag.kind, tag.uid_len,
          tag.ndef_bytes, error);
    lap_put_str(1, 5, "W writes a short NDEF text to the current writable tag",
                LAP_ATTR(LAP_GREEN, LAP_BLACK));
}
static bool key(lap_key_t key, char ch) {
    if (key == LAP_KEY_CHAR && ch == 'w' && h > 0 && tag.writable) {
        static const uint8_t text[] = {0xd1, 1, 5, 'T', 2, 'e', 'n', 'H', 'i'};
        error = lap_nfc_write(h, text, sizeof(text));
        return true;
    }
    return false;
}
LAP_APP(.id = "nfc_tags", .name = "NFC TAGS", .sub = "UID / bounded NDEF", .version = "1.0",
        .author = "LakeShark", .icon = LAP_ICON_CHIP, .color = LAP_CYAN, .enter = enter,
        .leave = leave, .draw = draw, .key = key);
