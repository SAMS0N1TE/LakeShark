#pragma once

#include <gui/gui.h>
#include <gui/elements.h>

#define LS_HDR_H 14
#define LS_HDR_BASE 11

#define LS_ROW_H 11

bool ls_ui_portrait(Canvas* c);

void ls_ui_header(
    Canvas* c,
    const char* title,
    int page,
    int pages,
    bool linked,
    const char* badge);

void ls_ui_row(Canvas* c, int y, const char* label, const char* value, bool selected);

void ls_ui_row_edit(
    Canvas* c,
    int y,
    const char* label,
    const char* value,
    bool selected,
    bool editing);

void ls_ui_divider_row(Canvas* c, int y, const char* label);

int ls_ui_rows(Canvas* c);

void ls_ui_scroll(int* top, int sel, int count, int rows);

void ls_ui_bar(Canvas* c, int x, int y, int w, int h, int pct);

void ls_ui_level(
    Canvas* c,
    int y,
    const char* label,
    const char* value,
    int pct,
    bool selected);

void ls_ui_level_edit(
    Canvas* c,
    int y,
    const char* label,
    const char* value,
    int pct,
    bool selected,
    bool editing);

void ls_ui_empty(Canvas* c, const char* line1, const char* line2);

void ls_ui_modal(Canvas* c, const char* title, const char* line1, const char* line2);

void ls_ui_toast(Canvas* c, const char* text);

void ls_ui_mhz(char* out, size_t len, uint32_t hz);

void ls_ui_age(char* out, size_t len, int32_t age_ms);
