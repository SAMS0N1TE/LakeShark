/* See ls_userapp.h. A described screen, bound to published values. */
#include "ls_userapp.h"

#include <dirent.h>
#include <stdio.h>

#include "esp_attr.h"
#include <stdlib.h>
#include <string.h>

#include "ls_app.h"
#include "ls_icons.h"
#include "ls_tui_screen.h"
#include "ls_tui_ui.h"
#include "ls_value.h"
#include "ls_waterfall.h"

/* --------------------------------------------------------------- parsing */

static const struct { const char *name; uint8_t icon; } ICONS[] = {
    { "tower", LS_ICON_TOWER }, { "wave",  LS_ICON_WAVE  },
    { "plane", LS_ICON_PLANE }, { "record",LS_ICON_RECORD},
    { "falls", LS_ICON_FALLS }, { "map",   LS_ICON_MAP   },
    { "files", LS_ICON_FILES }, { "gear",  LS_ICON_GEAR  },
    { "pager", LS_ICON_PAGER }, { "chip",  LS_ICON_CHIP  },
    { "star",  LS_ICON_STAR  }, { "shark", LS_ICON_SHARK },
};

static const struct { const char *name; uint8_t hue; } HUES[] = {
    { "red",     TUI_RED     }, { "green",   TUI_GREEN   },
    { "yellow",  TUI_YELLOW  }, { "blue",    TUI_BLUE    },
    { "magenta", TUI_MAGENTA }, { "cyan",    TUI_CYAN    },
    { "white",   TUI_WHITE   },
};

static void fail(ls_ua_err_t *e, int line, const char *why)
{
    if (!e) return;
    e->line = line;
    snprintf(e->reason, sizeof(e->reason), "%s", why);
}

static void copy_field(char *dst, size_t cap, const char *src, size_t n)
{
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

/* Split "LABEL|path" at the bar. A missing bar is a missing path, which the
   caller decides about - `text=` has no path and `value=` must have one. */
static void split_bar(const char *v, size_t n, const char **lab, size_t *lab_n,
                      const char **path, size_t *path_n)
{
    const char *bar = memchr(v, '|', n);
    if (bar) {
        *lab = v;   *lab_n = (size_t)(bar - v);
        *path = bar + 1; *path_n = n - (size_t)(bar - v) - 1;
    } else {
        *lab = v;   *lab_n = n;
        *path = NULL; *path_n = 0;
    }
}

bool ls_userapp_parse(const char *text, size_t len, ls_ua_model_t *out,
                      ls_ua_err_t *err)
{
    if (err) { err->line = 0; err->reason[0] = 0; }
    if (!text || !out) { fail(err, 0, "no input"); return false; }
    if (len > LS_UA_MAX_BYTES) { fail(err, 0, "file is too large"); return false; }

    ls_ua_model_t m;
    memset(&m, 0, sizeof(m));
    m.icon = LS_ICON_STAR;
    m.hue  = TUI_MAGENTA;

    bool have_version = false, have_name = false;
    int line_no = 0;
    size_t i = 0;

    while (i < len) {
        size_t start = i;
        while (i < len && text[i] != '\n') i++;
        size_t end = i;
        if (i < len) i++;                        /* step over the newline   */
        line_no++;

        if (end > start && text[end - 1] == '\r') end--;   /* CRLF is fine  */
        while (start < end && (text[start] == ' ' || text[start] == '\t')) start++;
        while (end > start && (text[end - 1] == ' ' || text[end - 1] == '\t')) end--;
        if (start == end) continue;                        /* blank         */
        if (text[start] == '#' || text[start] == ';') continue;

        const char *eq = memchr(text + start, '=', end - start);
        if (!eq) { fail(err, line_no, "expected key=value"); return false; }

        const char *key = text + start;
        size_t key_n = (size_t)(eq - key);
        const char *val = eq + 1;
        size_t val_n = (size_t)((text + end) - val);
        while (val_n && (*val == ' ' || *val == '\t')) { val++; val_n--; }

        #define KEY_IS(s) (key_n == strlen(s) && memcmp(key, s, key_n) == 0)

        if (KEY_IS("version")) {
            if (val_n != 1 || val[0] != '1') {
                fail(err, line_no, "only version=1 is understood");
                return false;
            }
            have_version = true;
            continue;
        }
        if (!have_version) {
            fail(err, line_no, "version=1 must come first");
            return false;
        }

        if (KEY_IS("name")) {
            if (!val_n) { fail(err, line_no, "name is empty"); return false; }
            copy_field(m.name, sizeof(m.name), val, val_n);
            have_name = true;
            continue;
        }
        if (KEY_IS("sub")) {
            copy_field(m.sub, sizeof(m.sub), val, val_n);
            continue;
        }
        if (KEY_IS("icon")) {
            for (unsigned k = 0; k < sizeof(ICONS) / sizeof(ICONS[0]); k++)
                if (val_n == strlen(ICONS[k].name) &&
                    memcmp(val, ICONS[k].name, val_n) == 0) {
                    m.icon = ICONS[k].icon;
                    goto next_line;
                }
            fail(err, line_no, "unknown icon name");
            return false;
        }
        if (KEY_IS("color")) {
            for (unsigned k = 0; k < sizeof(HUES) / sizeof(HUES[0]); k++)
                if (val_n == strlen(HUES[k].name) &&
                    memcmp(val, HUES[k].name, val_n) == 0) {
                    m.hue = HUES[k].hue;
                    goto next_line;
                }
            fail(err, line_no, "unknown color name");
            return false;
        }

        /* Elements. */
        {
            uint8_t kind;
            bool needs_path;
            if      (KEY_IS("panel"))     { kind = LS_UA_PANEL;     needs_path = false; }
            else if (KEY_IS("value"))     { kind = LS_UA_VALUE;     needs_path = true;  }
            else if (KEY_IS("bar"))       { kind = LS_UA_BAR;       needs_path = true;  }
            else if (KEY_IS("text"))      { kind = LS_UA_TEXT;      needs_path = false; }
            else if (KEY_IS("waterfall")) { kind = LS_UA_WATERFALL; needs_path = false; }
            else { fail(err, line_no, "unknown field"); return false; }

            if (m.n >= LS_UA_MAX_ELEMS) {
                fail(err, line_no, "too many elements");
                return false;
            }

            const char *lab; size_t lab_n;
            const char *path; size_t path_n;
            split_bar(val, val_n, &lab, &lab_n, &path, &path_n);

            if (needs_path && (!path || !path_n)) {
                fail(err, line_no, "expected LABEL|value.path");
                return false;
            }
            if (lab_n >= LS_UA_STR || path_n >= LS_UA_STR) {
                fail(err, line_no, "label or path is too long");
                return false;
            }

            ls_ua_elem_t *e = &m.elem[m.n++];
            e->kind = kind;
            copy_field(e->label, sizeof(e->label), lab, lab_n);
            if (path && path_n) copy_field(e->path, sizeof(e->path), path, path_n);
        }
    next_line:
        continue;
        #undef KEY_IS
    }

    if (!have_version) { fail(err, 0, "no version=1 line"); return false; }
    if (!have_name)    { fail(err, 0, "no name= line");     return false; }
    if (!m.n)          { fail(err, 0, "nothing to draw");   return false; }

    *out = m;
    return true;
}

/* --------------------------------------------------------------- drawing */

static void draw_value(tui_surface *sf, tui_rect panel, int row,
                       const ls_ua_elem_t *e)
{
    ls_val_t v;
    const char *unit = NULL;
    char buf[32];

    if (!ls_value_read(e->path, &v, &unit)) {

        ls_kv(sf, panel, row, e->label, "--",
              LS_ATTR_DIM);
        return;
    }
    switch (v.kind) {
    case LS_VAL_INT:
        snprintf(buf, sizeof(buf), "%ld%s%s", v.i, unit ? " " : "",
                 unit ? unit : "");
        break;
    case LS_VAL_FLOAT:
        snprintf(buf, sizeof(buf), "%.3f%s%s", (double)v.f, unit ? " " : "",
                 unit ? unit : "");
        break;
    case LS_VAL_BOOL:
        snprintf(buf, sizeof(buf), "%s", v.i ? "YES" : "no");
        break;
    case LS_VAL_TEXT:
        snprintf(buf, sizeof(buf), "%.20s", v.s ? v.s : "--");
        break;
    default:
        snprintf(buf, sizeof(buf), "--");
        break;
    }
    ls_kv(sf, panel, row, e->label, buf,
          TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK));
}

void ls_userapp_draw(const ls_ua_model_t *m, tui_surface *sf, tui_rect area)
{
    if (!m || !sf) return;

    /* Count panels so the area can be divided before anything is drawn. A
       waterfall element claims a whole panel's worth of room, because a
       waterfall in four rows is not a waterfall. */
    int panels = 0;
    for (int i = 0; i < m->n; i++)
        if (m->elem[i].kind == LS_UA_PANEL) panels++;
    if (panels == 0) panels = 1;

    tui_rect first = area, second = area;
    bool split = false;
    if (panels > 1) { ls_tui_split(area, &first, &second); split = true; }

    tui_rect cur = first;
    int panel_index = 0, row = 1;
    bool open = false;

    for (int i = 0; i < m->n; i++) {
        const ls_ua_elem_t *e = &m->elem[i];

        if (e->kind == LS_UA_PANEL) {
            panel_index++;
            if (panel_index == 2 && split) cur = second;
            else if (panel_index > 2 && split) {
                /* Third and later panels stack inside the second half. It is
                   crude, and a layout that asks for five panels on a 48-column
                   panel has already made the interesting decision itself. */
                cur.y += cur.h / 2;
                cur.h -= cur.h / 2;
            }
            ls_panel_box(sf, cur, e->label[0] ? e->label : "PANEL",
                         m->hue ? m->hue : (uint8_t)TUI_CYAN);
            open = true;
            row = 1;
            continue;
        }

        if (!open) {
            ls_panel_box(sf, cur, m->name, m->hue ? m->hue : (uint8_t)TUI_CYAN);
            open = true;
            row = 1;
        }
        if (row >= cur.h - 1 && e->kind != LS_UA_WATERFALL) continue;

        switch (e->kind) {
        case LS_UA_VALUE:
            draw_value(sf, cur, row++, e);
            break;
        case LS_UA_BAR: {
            ls_val_t v;
            const char *unit = NULL;
            float f = 0.0f;
            if (ls_value_read(e->path, &v, &unit))
                f = (v.kind == LS_VAL_FLOAT) ? v.f : (float)v.i;
            tui_put_str(sf, cur, cur.x + 2, cur.y + row, e->label,
                        TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK));
            const int bx = cur.x + 2 + (int)strlen(e->label) + 1;
            const int bw = cur.x + cur.w - 2 - bx;
            if (bw > 2) ls_bar(sf, cur, row, bx, bw, f);
            row++;
            break;
        }
        case LS_UA_TEXT:
            tui_put_str(sf, cur, cur.x + 2, cur.y + row++, e->label,
                        TUI_ATTR(TUI_WHITE, TUI_BLACK));
            break;
        case LS_UA_WATERFALL: {
            tui_rect wf = tui_rect_make(cur.x + 1, cur.y + row,
                                        cur.w - 2, cur.h - row - 1);
            if (wf.h >= 3) ls_wf_draw_mini(sf, wf);
            row = cur.h;
            break;
        }
        default:
            break;
        }
    }
}

/* --------------------------------------------------------------- loading */

/* The models live in PSRAM, and the measurement is why. */

EXT_RAM_BSS_ATTR static ls_ua_model_t s_model[LS_UA_MAX_APPS];
static ls_tui_screen_t s_screen[LS_UA_MAX_APPS];
static char           s_hint[LS_UA_MAX_APPS][40];
static int            s_loaded;
static char           s_last_err[96];

/* One draw function per slot, because a screen descriptor has no user
   pointer - the router deliberately keeps that contract narrow. Six slots is
   six three-line functions, which is cheaper than widening the contract for
   everything else that will never need it. */
#define UA_SLOT(n) \
    static void ua_draw_##n(tui_surface *sf, tui_rect a) \
    { ls_userapp_draw(&s_model[n], sf, a); }
UA_SLOT(0) UA_SLOT(1) UA_SLOT(2) UA_SLOT(3) UA_SLOT(4) UA_SLOT(5)
#undef UA_SLOT

static void (*const UA_DRAW[LS_UA_MAX_APPS])(tui_surface *, tui_rect) = {
    ua_draw_0, ua_draw_1, ua_draw_2, ua_draw_3, ua_draw_4, ua_draw_5,
};

const char *ls_userapp_last_error(void)
{
    return s_last_err[0] ? s_last_err : NULL;
}

static bool load_one(const char *path, const char *fname)
{
    if (s_loaded >= LS_UA_MAX_APPS) return false;

    FILE *f = fopen(path, "r");
    if (!f) return false;

    /* One file-static scratch in PSRAM, not a stack array and not an
       allocation. This runs once at boot from whatever task starts the shell,
       and LS_UA_MAX_BYTES is far more than that stack should be asked to
       carry. */
    EXT_RAM_BSS_ATTR static char buf[LS_UA_MAX_BYTES + 1];
    size_t n = fread(buf, 1, LS_UA_MAX_BYTES, f);
    const bool over = !feof(f);
    fclose(f);
    if (over) {
        snprintf(s_last_err, sizeof(s_last_err), "%s: larger than 4 KB", fname);
        return false;
    }
    buf[n] = 0;

    ls_ua_err_t err;
    if (!ls_userapp_parse(buf, n, &s_model[s_loaded], &err)) {
        snprintf(s_last_err, sizeof(s_last_err), "%s line %d: %s",
                 fname, err.line, err.reason);
        return false;
    }

    const int slot = s_loaded;
    snprintf(s_hint[slot], sizeof(s_hint[slot]), "user app  ESC  directory");
    s_screen[slot].name  = s_model[slot].name;
    s_screen[slot].hint  = s_hint[slot];
    s_screen[slot].draw  = UA_DRAW[slot];
    s_screen[slot].enter = NULL;
    s_screen[slot].leave = NULL;
    s_screen[slot].key   = NULL;
    s_screen[slot].touch = NULL;

    ls_app_t app = {
        .id     = s_model[slot].name,
        .name   = s_model[slot].name,
        .sub    = s_model[slot].sub[0] ? s_model[slot].sub : "user app",
        .icon   = s_model[slot].icon,
        .hue    = s_model[slot].hue,
        .cat    = LS_APP_USER,
        .screen = &s_screen[slot],
        .live   = NULL,
    };
    if (ls_app_register(&app) < 0) {
        snprintf(s_last_err, sizeof(s_last_err),
                 "%s: no room left in the app directory", fname);
        return false;
    }
    s_loaded++;
    return true;
}

int ls_userapp_load_dir(const char *dir)
{
    if (!dir) return 0;
    s_last_err[0] = 0;

    /* Opening the directory is the card-present test. A missing card is the
       normal case on a board with an empty slot and must not be an error. */
    DIR *d = opendir(dir);
    if (!d) return 0;

    int added = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        const char *dot = strrchr(ent->d_name, '.');
        if (!dot || strcmp(dot, ".lsapp") != 0) continue;
        if (s_loaded >= LS_UA_MAX_APPS) {

            snprintf(s_last_err, sizeof(s_last_err),
                     "%s: directory full at %d apps", ent->d_name,
                     LS_UA_MAX_APPS);
            break;
        }
        char path[160];
        snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
        if (load_one(path, ent->d_name)) added++;
    }
    closedir(d);
    return added;
}
