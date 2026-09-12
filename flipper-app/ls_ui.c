#include "ls_ui.h"

#include <stdio.h>
#include <string.h>

bool ls_ui_portrait(Canvas* c) {
    return canvas_width(c) < 100;
}

int ls_ui_rows(Canvas* c) {
    int n = (canvas_height(c) - LS_HDR_H) / LS_ROW_H;
    return n < 1 ? 1 : n;
}

void ls_ui_scroll(int* top, int sel, int count, int rows) {
    if(sel < *top) *top = sel;
    if(sel >= *top + rows) *top = sel - rows + 1;
    if(*top > count - rows) *top = count - rows;
    if(*top < 0) *top = 0;
}

void ls_ui_header(
    Canvas* c,
    const char* title,
    int page,
    int pages,
    bool linked,
    const char* badge) {
    const int w = canvas_width(c);

    canvas_draw_box(c, 0, 0, w, LS_HDR_H);
    canvas_set_color(c, ColorWhite);
    canvas_set_font(c, FontPrimary);
    canvas_draw_str(c, 2, LS_HDR_BASE, title);

    int right = w - 2;
    if(linked) {
        canvas_draw_disc(c, right - 3, 6, 3);
    } else {
        canvas_draw_circle(c, right - 3, 6, 3);
    }
    right -= 9;

    if(badge && badge[0]) {
        canvas_set_font(c, FontSecondary);
        canvas_draw_str_aligned(c, right, LS_HDR_BASE, AlignRight, AlignBottom, badge);
        right -= canvas_string_width(c, badge) + 3;
    }

    if(pages > 1) {
        if(ls_ui_portrait(c)) {

            char n[24];
            snprintf(n, sizeof(n), "%d/%d", page + 1, pages);
            canvas_set_font(c, FontSecondary);
            canvas_draw_str_aligned(c, right, LS_HDR_BASE, AlignRight, AlignBottom, n);
        } else {
            const int spacing = 7;
            int total = (pages - 1) * spacing;
            int x0 = w / 2 - total / 2;

            canvas_set_font(c, FontPrimary);
            int title_end = 2 + canvas_string_width(c, title) + 4;
            if(x0 < title_end) x0 = title_end;

            for(int i = 0; i < pages; i++) {
                int x = x0 + i * spacing;
                if(x > right - 2) break;
                if(i == page) {
                    canvas_draw_disc(c, x, 6, 2);
                } else {
                    canvas_draw_circle(c, x, 6, 1);
                }
            }
        }
    }

    canvas_set_color(c, ColorBlack);
    canvas_set_font(c, FontSecondary);
}

static void edit_wrap(char* out, size_t len, const char* value) {
    snprintf(out, len, "<%s>", value ? value : "");
}

void ls_ui_row_edit(
    Canvas* c,
    int y,
    const char* label,
    const char* value,
    bool selected,
    bool editing) {
    const int w = canvas_width(c);

    char marked[32];
    if(editing && value && value[0]) {
        edit_wrap(marked, sizeof(marked), value);
        value = marked;
    }

    if(selected) {
        canvas_draw_box(c, 0, y, w, LS_ROW_H);
        canvas_set_color(c, ColorWhite);
    }

    int base = y + LS_ROW_H - 3;
    int avail = w - 6;

    if(value && value[0]) {
        canvas_draw_str_aligned(c, w - 3, base, AlignRight, AlignBottom, value);
        avail -= canvas_string_width(c, value) + 4;
    }

    if(canvas_string_width(c, label) <= avail) {
        canvas_draw_str(c, 3, base, label);
    } else {
        char clip[28];
        size_t n = strlen(label);
        if(n >= sizeof(clip)) n = sizeof(clip) - 1;
        memcpy(clip, label, n);
        clip[n] = '\0';
        while(n > 1 && canvas_string_width(c, clip) > avail) {
            clip[--n] = '\0';
        }
        canvas_draw_str(c, 3, base, clip);
    }

    if(selected) canvas_set_color(c, ColorBlack);
}

void ls_ui_row(Canvas* c, int y, const char* label, const char* value, bool selected) {
    ls_ui_row_edit(c, y, label, value, selected, false);
}

void ls_ui_divider_row(Canvas* c, int y, const char* label) {
    const int w = canvas_width(c);
    const int base = y + LS_ROW_H - 3;

    canvas_set_font(c, FontSecondary);
    canvas_draw_str(c, 3, base, label);

    int x = 3 + canvas_string_width(c, label) + 4;
    if(x < w - 4) canvas_draw_line(c, x, base - 3, w - 4, base - 3);
}

void ls_ui_bar(Canvas* c, int x, int y, int w, int h, int pct) {
    if(pct < 0) pct = 0;
    if(pct > 100) pct = 100;
    canvas_draw_frame(c, x, y, w, h);
    int fill = ((w - 2) * pct) / 100;
    if(fill > 0) canvas_draw_box(c, x + 1, y + 1, fill, h - 2);
}

void ls_ui_level_edit(
    Canvas* c,
    int y,
    const char* label,
    const char* value,
    int pct,
    bool selected,
    bool editing) {
    const int w = canvas_width(c);

    char marked[32];
    if(editing && value && value[0]) {
        edit_wrap(marked, sizeof(marked), value);
        value = marked;
    }

    if(selected) {
        canvas_draw_box(c, 0, y, w, LS_ROW_H);
        canvas_set_color(c, ColorWhite);
    }

    int base = y + LS_ROW_H - 3;
    canvas_draw_str(c, 3, base, label);

    int vw = value && value[0] ? canvas_string_width(c, value) : 0;
    if(vw) canvas_draw_str_aligned(c, w - 3, base, AlignRight, AlignBottom, value);

    int lw = canvas_string_width(c, label);
    int bx = 3 + lw + 4;
    int bw = (w - 3 - vw - 4) - bx;
    if(bw >= 16) {
        ls_ui_bar(c, bx, y + 2, bw, LS_ROW_H - 5, pct);
    }

    if(selected) canvas_set_color(c, ColorBlack);
}

void ls_ui_level(
    Canvas* c,
    int y,
    const char* label,
    const char* value,
    int pct,
    bool selected) {
    ls_ui_level_edit(c, y, label, value, pct, selected, false);
}

void ls_ui_empty(Canvas* c, const char* line1, const char* line2) {
    const int w = canvas_width(c);
    const int mid = (LS_HDR_H + canvas_height(c)) / 2;
    canvas_set_font(c, FontSecondary);
    canvas_draw_str_aligned(c, w / 2, mid, AlignCenter, AlignBottom, line1);
    if(line2 && line2[0]) {
        canvas_draw_str_aligned(c, w / 2, mid + 11, AlignCenter, AlignBottom, line2);
    }
}

void ls_ui_modal(Canvas* c, const char* title, const char* line1, const char* line2) {
    const int w = canvas_width(c);
    const int h = canvas_height(c);

    int bw = w - 8;
    int bh = line2 && line2[0] ? 46 : 36;
    int x = 4;
    int y = h / 2 - bh / 2;

    canvas_set_color(c, ColorWhite);
    canvas_draw_box(c, x, y, bw, bh);
    canvas_set_color(c, ColorBlack);
    canvas_draw_frame(c, x, y, bw, bh);
    canvas_draw_frame(c, x + 1, y + 1, bw - 2, bh - 2);

    canvas_set_font(c, FontPrimary);
    canvas_draw_str_aligned(c, w / 2, y + 15, AlignCenter, AlignBottom, title);
    canvas_set_font(c, FontSecondary);
    if(line1 && line1[0]) {
        canvas_draw_str_aligned(c, w / 2, y + 27, AlignCenter, AlignBottom, line1);
    }
    if(line2 && line2[0]) {
        canvas_draw_str_aligned(c, w / 2, y + 38, AlignCenter, AlignBottom, line2);
    }
}

void ls_ui_toast(Canvas* c, const char* text) {
    canvas_set_font(c, FontSecondary);
    const int cw = canvas_width(c);
    const int y = canvas_height(c) - 18;

    int w = canvas_string_width(c, text) + 8;
    if(w > cw) w = cw;
    int x = cw / 2 - w / 2;

    canvas_set_color(c, ColorWhite);
    canvas_draw_box(c, x, y, w, 14);
    canvas_set_color(c, ColorBlack);
    canvas_draw_frame(c, x, y, w, 14);
    canvas_draw_str_aligned(c, cw / 2, y + 11, AlignCenter, AlignBottom, text);
}

void ls_ui_mhz(char* out, size_t len, uint32_t hz) {
    uint32_t mhz = hz / 1000000u;
    uint32_t frac = (hz % 1000000u) / 10u;
    snprintf(out, len, "%lu.%05lu", (unsigned long)mhz, (unsigned long)frac);
}

void ls_ui_age(char* out, size_t len, int32_t age_ms) {
    if(age_ms < 0) {
        snprintf(out, len, "-");
    } else if(age_ms < 1500) {
        snprintf(out, len, "now");
    } else if(age_ms < 60000) {
        snprintf(out, len, "%lds", (long)(age_ms / 1000));
    } else if(age_ms < 3600000) {
        snprintf(out, len, "%ldm", (long)(age_ms / 60000));
    } else {
        snprintf(out, len, "%ldh", (long)(age_ms / 3600000));
    }
}
