/* Property sheets: what the Control Panel's applets and Display Properties
   are made of (ls_desk_sheet.c).

   A sheet is tabs; a tab is group boxes of controls in two columns, the
   labels and the fields. The controls are a desktop's: a check box, a
   drop-down list, a spin field, a slider, a gauge that only reads, a push
   button, a line of fact. Each reads the board through a getter and writes
   it through a setter that takes the value outright; the sheet keeps what
   was chosen until OK or Apply and forgets it on Cancel. Apply is greyed
   until something changed. Tab walks the focus, a marked letter jumps to
   its control, the pointer lights what it is over.

   One sheet is up at a time, in the Display Properties window
   (DK_SHELL_DISPLAY): the Control Panel stays open behind it. */
#ifndef LS_DESK_SHEET_H
#define LS_DESK_SHEET_H

#include <stddef.h>
#include "ls_desk_priv.h"

/* Inks are the role table's (dk_attr, dk_hue, dk_art: every one checked
   against the palette in use) and the small parts the shell's one widget
   set (dk_label, dk_pushbtn, dk_vscroll, dk_hover): ls_desk_priv.h. */

/* ------------------------------------------------------------ controls -- */

enum { SH_GROUP, SH_TEXT, SH_NOTE, SH_CHECK, SH_DROP, SH_SPIN, SH_SLIDER, SH_GAUGE, SH_BUTTON, SH_CUSTOM };
enum {
    SHF_LIVE = 1,       /* written as it moves (a volume is heard); Cancel puts the old value back */
    SHF_OWN = 2,        /* the value lives in the sheet's own copy: get and set are that copy's */
    SHF_VIEW = 4,       /* with SHF_OWN: changes what is shown, not a setting; Apply stays as it was */
    SHF_SWATCH = 8,     /* a drop-down of colours: each row carries its chip */
    SHF_WARN = 16,      /* a check whose ON is the state to notice */
};

typedef struct dk_sh_ctl {
    uint8_t kind, flags;
    const char *label;                          /* SH_GROUP: its title; SH_NOTE: the line */
    int  (*get)(void);                          /* check 0/1, drop index, spin/slider/gauge value */
    void (*set)(int v);
    int  (*count)(void);                        /* SH_DROP: how many rows */
    void (*text)(int v, char *b, size_t n);     /* drop row / spin, slider value as text */
    void (*show)(char *b, size_t n);            /* SH_TEXT, SH_GAUGE, SH_NOTE: the line as it is now */
    int16_t min, max, step;                     /* spin, slider, gauge */
    const char *cap;                            /* SH_BUTTON: its caption */
    void (*press)(void);                        /* SH_BUTTON: done at once */
    const char *go;                             /* SH_BUTTON without press: an app id, "@tasks"... */
    bool (*hidden)(void);
    bool (*off)(void);                          /* greyed */
    uint8_t rows;                               /* SH_CUSTOM: its height */
    void (*draw)(tui_surface *sf, tui_rect clip, tui_rect r);
    bool (*tap)(tui_rect r, int col, int row);
} dk_sh_ctl_t;

typedef struct {
    const char *title;                          /* "&Background" */
    const dk_sh_ctl_t *ctl;
    int n;
    void (*enter)(void);                        /* the tab came up: read what is slow to read */
    /* A picture down the left side (a preview), the controls to its right. */
    uint8_t side_w, side_h;                     /* its width; the rows it needs */
    void (*side)(tui_surface *sf, tui_rect clip, tui_rect r);
    bool (*side_tap)(tui_rect r, int col, int row);
} dk_sh_page_t;

typedef struct {
    const char *title;                          /* the window's */
    const dk_sh_page_t *pages;
    int n;
    uint8_t hue;                                /* group titles */
    uint8_t w;                                  /* client width wanted; 0: 66 */
    void (*load)(void);                         /* fill the sheet's own copy from the board */
    void (*commit)(void);                       /* Apply: write that copy */
} dk_sheet_t;

/* Open (or change to) a sheet on a tab. */
void dk_sheet_open(const dk_sheet_t *sh, int tab);
/* The same without touching the window: the caller opens it. */
void dk_sheet_begin(const dk_sheet_t *sh, int tab);
const dk_sheet_t *dk_sheet_now(void);
void dk_sheet_set_tab(int tab);
void dk_sheet_dirty(void);                      /* a picture or a button changed the sheet's own copy */
/* What a control holds now: the choice not yet applied, or the board's. */
int  dk_sheet_val(const dk_sh_ctl_t *c);
void dk_sheet_go(const char *where);
void dk_sheet_draw(tui_surface *sf, tui_rect c, tui_rect clip);
bool dk_sheet_tap(tui_rect c, int col, int row);
bool dk_sheet_key(ls_tk_t k, char ch);

/* The sheets: Display Properties (ls_desk_props.c) and one per applet of
   the panel, in the panel's order (ls_desk_cplsys.c; NULL for Display). */
const dk_sheet_t *dk_display_sheet(void);
const dk_sheet_t *dk_cpl_sheet(int applet);

#endif
