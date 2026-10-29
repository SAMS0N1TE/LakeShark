/* SYSTEM > UPDATE > C6 FIRMWARE. All file/RPC work belongs to the backend. */
#include "ls_c6_view.h"
#include "ls_action.h"
#include "esp_attr.h"
#include "ls_options.h"
#include "ls_value.h"
#include "ls_glyph.h"
#include "ls_radio_panel.h"
#include "../../../main/ls_c6_core.h"
#include <stdio.h>
#include <string.h>
#include "ls_textpack.h"

EXT_RAM_BSS_ATTR static char s_path[192];
static bool s_force, s_confirm;
static void review(const ls_opt_t *o);
static void inspect(void);
EXT_RAM_BSS_ATTR static char s_notice[80];

static bool blocked(void)
{
    ls_val_t value;
    return !ls_value_read("c6.busy", &value, NULL) || value.i != 0;
}
static const char *why_not(const ls_opt_t *o)
{
    (void)o;
    return blocked() ? "Busy or radios inhibited; see status" : NULL;
}
static const char *path_get(const ls_opt_t *o) { (void)o; return s_path; }
static void path_set(const ls_opt_t *o, const char *path)
{
    (void)o;
    if (blocked()) return;
    snprintf(s_path, sizeof(s_path), "%s", path);
    s_confirm = false;
    inspect();
}
static int force_get(const ls_opt_t *o) { (void)o; return s_force; }
static void force_set(const ls_opt_t *o, int value) { (void)o; s_force = value != 0; }
static void install(const ls_opt_t *o)
{
    (void)o;
    if (!s_confirm || blocked()) { s_confirm = false; return; }
    s_confirm = false;
    ls_args_t args = {.n = 2, .v = {{.kind = LS_VAL_TEXT, .s = s_path},
                                   {.kind = LS_VAL_BOOL, .i = s_force}}};
    ls_val_t out;
    ls_act_status_t rc = ls_action_call("c6.update", &args, &out, LS_CAP_STORE | LS_CAP_POWER);
    s_force = false; /* one request, never a persistent downgrade permission */
    snprintf(s_notice, sizeof(s_notice), "%s", rc == LS_ACT_OK ? "" : ls_act_status_str(rc));
}
static const ls_opt_t IMAGE[] = {
    {.label = "SD image", .kind = LS_OPT_TEXT, .text = path_get, .set_text = path_set,
     .max = 191, .why_not = why_not},
};
static const ls_opt_ctx_t IMAGE_MENU = {.name = "IMAGE", .job = -1, .radio = LS_RSEL_NONE, LS_OPT_ROWS(IMAGE)};
static const ls_opt_t POLICY[] = {
    {.label = "Force same/older version", .kind = LS_OPT_TOGGLE, .get = force_get,
     .set = force_set, .why_not = why_not},
};
static const ls_opt_ctx_t POLICY_MENU = {.name = "VERSION", .job = -1, .radio = LS_RSEL_NONE, LS_OPT_ROWS(POLICY)};
static const ls_opt_t OPTIONS[] = {
    {.label = "Image", .kind = LS_OPT_MENU, .sub = &IMAGE_MENU},
    {.label = "Version policy", .kind = LS_OPT_MENU, .sub = &POLICY_MENU},
    {.label = "Review installation", .kind = LS_OPT_ACTION, .act = review, .why_not = why_not, .leaves = true},
};
static const ls_opt_ctx_t MENU = {.name = "C6 FIRMWARE", .job = -1, .radio = LS_RSEL_NONE,
    LS_OPT_ROWS(OPTIONS), .back_to_screen = true, .menu_arrows = true};


static long number(const char *path, long fallback)
{
    ls_val_t v;
    return ls_value_read(path, &v, NULL) && v.kind == LS_VAL_INT ? v.i : fallback;
}
static void text_value(const char *path, char *out, size_t cap)
{
    ls_val_t v;
    snprintf(out, cap, "%s", ls_value_read(path, &v, NULL) &&
             v.kind == LS_VAL_TEXT && v.s && *v.s ? v.s : "not checked");
}
static void inspect(void)
{
    if (blocked()) return;
    ls_args_t args = {.n = 1, .v = {{.kind = LS_VAL_TEXT, .s = s_path}}};
    ls_val_t out;
    ls_act_status_t rc = ls_action_call("c6.inspect", &args, &out, LS_CAP_STORE | LS_CAP_POWER);
    snprintf(s_notice, sizeof(s_notice), "%s", rc == LS_ACT_OK ? "" : ls_act_status_str(rc));
}
void ls_c6_view_enter(void)
{
    s_confirm = false;
    s_force = false;
    s_notice[0] = 0;
    if (!s_path[0]) snprintf(s_path, sizeof(s_path), "%s", LS_C6_DEFAULT_PATH);
    if (number("c6.stage", -1) == -1) inspect();
}
void ls_c6_view_leave(void) { s_confirm = false; s_force = false; }
static void review(const ls_opt_t *o)
{
    (void)o;
    if (!blocked()) s_confirm = true;
}

/* Word wrapping keeps backend refusal messages complete at either aspect. */
static int paragraph(tui_surface *sf, tui_rect in, int y, const char *text, uint8_t hue)
{
    while (*text && y < in.y + in.h && in.w > 2) {
        char line[240];
        size_t count = strlen(text), width = (size_t)(in.w - 2);
        if (width >= sizeof(line)) width = sizeof(line) - 1;
        if (count > width) {
            count = width;
            while (count > 0 && text[count] != ' ') --count;
            if (!count) count = width;
        }
        memcpy(line, text, count); line[count] = 0;
        ls_safe_line(sf, in, y++, line, TUI_ATTR(hue, TUI_BLACK));
        text += count;
        while (*text == ' ') ++text;
    }
    return y;
}
static void figures(tui_surface *sf, tui_rect r, const char *word, uint8_t hue)
{
    int n = (int)strlen(word), w = r.w / (n ? n : 1);
    if (w > 8) w = 8;
    int x = r.x + (r.w - n * w) / 2;
    for (int i = 0; i < n; ++i)
        ls_glyph_draw_line(sf, tui_rect_make(x + i*w, r.y, w, r.h), word[i],
                      TUI_ATTR(hue | TUI_BRIGHT, TUI_BLACK));
}
/* Computed pins and shared stroke glyphs: no bitmap/table or frame buffer. */
static void emblem(tui_surface *sf, tui_rect r, uint8_t hue, long sent, long size)
{
    int w = r.w > 30 ? 30 : r.w - 4;
    const bool compact = r.h < 26;
    int x = r.x + (r.w - w)/2, y = r.y + 2;
    int height = compact ? 7 : 10;
    tui_rect chip = tui_rect_make(x, y, w, height);
    ls_radio_bezel(sf, chip, "ESP32 / C6");
    for (int pin = 3; pin < w - 2; pin += 3) {
        tui_put_char(sf, r, x + pin, y - 1, '|', TUI_ATTR(TUI_CYAN, TUI_BLACK));
        tui_put_char(sf, r, x + pin, y + height, '|', TUI_ATTR(TUI_BLUE, TUI_BLACK));
    }
    figures(sf, tui_rect_make(x+2,y+(compact ? 1 : 2),w-4,5), "C6", hue);
    if (!compact) ls_safe_line(sf, chip, y+8, "NETWORK ADAPTER", LS_ATTR_DIM);
    for (int j = 0; j < (compact ? 3 : 4); ++j) {
        tui_put_char(sf,r,x-1,y+2+j*2,'-',TUI_ATTR(TUI_MAGENTA,TUI_BLACK));
        tui_put_char(sf,r,x+w,y+2+j*2,'-',TUI_ATTR(TUI_CYAN,TUI_BLACK));
    }
    /* Slot fill advances only on acknowledged bytes; stopped data is still. */
    int yy = y+height+2, bw = w-4;
    ls_safe_line(sf,r,yy,LS_TEXT(t_ls_c6_view_sd_p4_inactive_slot, "SD  >  P4  >  INACTIVE SLOT"),TUI_ATTR(TUI_CYAN,TUI_BLACK));
    if (yy+2 < r.y+r.h) {
        ls_meter_ascii(sf,r,x+2,yy+2,bw,size>0?(float)sent/size:0,hue);
        tui_put_str(sf,r,x+2,yy+1,"0",LS_ATTR_DIM);
        tui_put_str(sf,r,x+2+bw/2-1,yy+1,"50",LS_ATTR_DIM);
        tui_put_str(sf,r,x+2+bw-4,yy+1,"100%",LS_ATTR_DIM);
    }
}
void ls_c6_view_draw(tui_surface *sf, tui_rect a)
{
    const bool off = blocked();
    long stage = number("c6.stage", -2), sent = number("c6.sent",0), size = number("c6.size",0);
    long boot = number("c6.boot",0);
    const bool unavailable = stage == -2;
    const bool failed = unavailable || stage == LS_C6_FAILED, done = stage == LS_C6_DONE;
    const uint8_t hue = failed ? TUI_RED : done ? TUI_GREEN : s_confirm ? TUI_MAGENTA :
                        stage >= LS_C6_STOP ? TUI_YELLOW : TUI_CYAN;
    const char *label = s_confirm ? "REVIEW" : unavailable ? "OFFLINE" : failed ? "STOP" : done ? "DONE" :
        stage == LS_C6_VALIDATE ? "CHECK" : stage == LS_C6_STOP ? "QUIET" :
        stage == LS_C6_BEGIN ? "PREP" : stage == LS_C6_END ? "VERIFY" :
        stage == LS_C6_ACTIVATE ? "BOOT" : stage == LS_C6_WAIT ? "LINK" : "READY";
    ls_btn_t buttons[] = {{s_confirm ? "CANCEL" : "BACK", "ESC", 0, false, false, false},
        {s_confirm ? "CONFIRM" : "INSTALL", s_confirm ? "Y" : "I", s_confirm ? 'y' : 'i', s_confirm, off, false},
        {"CHECK SD", "R", 'r', false, off || s_confirm, false},
        {"OPTIONS", "O", 'o', false, off || s_confirm, false}};
    ls_btn_bar_raised_row(sf,tui_rect_make(a.x,a.y,a.w,5),buttons,4,-1);
    tui_rect body = tui_rect_make(a.x,a.y+5,a.w,a.h-5);
    ls_panel_box(sf,body,s_confirm ? "CONFIRM C6 INSTALL" : "C6 FIRMWARE / SD UPDATE",hue);
    tui_rect in = tui_rect_make(body.x+2,body.y+1,body.w-4,body.h-2);
    const bool wide = in.w >= 80;
    tui_rect art = in, detail = in;
    if (wide) {
        art.w = 38;
        detail.x += 40; detail.w -= 40;
        /* Only the detail pane skirts the landscape camera, not the screen. */
        tui_rect hole;
        if (ls_tui_cutout(&hole) && hole.x > detail.x && hole.x < detail.x+detail.w)
            detail.w = hole.x-detail.x-2;
    } else {
        art.h = 26;
        detail.y += 27; detail.h -= 27;
    }
    int x = art.x+1;
    x = ls_radio_chip(sf,art,x,art.y,"SD",size>0,TUI_CYAN);
    x = ls_radio_chip(sf,art,x,art.y,"WRITE",stage==LS_C6_WRITE,TUI_YELLOW);
    ls_radio_chip(sf,art,x,art.y,"BOOT",done,TUI_GREEN);
    emblem(sf,art,hue,sent,size);
    char num[32];
    if (stage == LS_C6_WRITE) {
        snprintf(num,sizeof(num),"%ld",size>0 ? sent*100/size : 0);
        label = num;
    }
    figures(sf,tui_rect_make(art.x,art.y+(wide ? 14 : 18),art.w,5),label,hue);
    ls_safe_line(sf,art,art.y+(wide ? 19 : 24),stage==LS_C6_WRITE ? "PERCENT / ACKNOWLEDGED BY C6" :
                 done ? "NEW BOOT + VERSION VERIFIED" : failed ? "NO AUTOMATIC RETRY" :
                 s_confirm ? "YOUR APPROVAL IS REQUIRED" : "KEEP POWER CONNECTED",TUI_ATTR(hue,TUI_BLACK));
    char old[32], image[32], current[32], line[240], status[240];
    text_value("c6.old",old,sizeof(old)); text_value("c6.image",image,sizeof(image));
    text_value("c6.new",current,sizeof(current)); text_value("c6.state",status,sizeof(status));
    if (unavailable) LS_SNPUTS(t_ls_c6_view_c6_updater_unavailable_in, status, sizeof(status), "C6 updater unavailable in this build.");
    int y = detail.y;
    y=paragraph(sf,detail,y,done ? "VERIFIED VERSION CHANGE" : "VERSION / PREFLIGHT",TUI_CYAN|TUI_BRIGHT);
    snprintf(line,sizeof(line),done ? "%s -> %s" : "Running C6   %s",old,current);
    y=paragraph(sf,detail,y,line,TUI_WHITE|TUI_BRIGHT);
    snprintf(line,sizeof(line),"SD image     %s",image);
    y=paragraph(sf,detail,y,line,TUI_MAGENTA|TUI_BRIGHT);
    LS_SNPRINTF(t_ls_c6_view_ld_ld_bytes_acknowledged, line,sizeof(line),"%ld / %ld bytes acknowledged",sent,size);
    y=paragraph(sf,detail,y+1,line,TUI_CYAN);
    y=paragraph(sf,detail,y,status,failed ? TUI_RED|TUI_BRIGHT : TUI_WHITE);
    if(s_notice[0]) y=paragraph(sf,detail,y,s_notice,TUI_RED|TUI_BRIGHT);
    if (s_confirm) {
        y=paragraph(sf,detail,y+1,"INSTALL PLAN",TUI_MAGENTA|TUI_BRIGHT);
        y=paragraph(sf,detail,y,LS_TEXT(t_ls_c6_view_recheck_sd_sha_256, "Recheck SD + SHA-256 + power; stop Wi-Fi/BLE; write inactive C6 slot; restart C6 and verify its version."),TUI_WHITE);
        y=paragraph(sf,detail,y+1,LS_TEXT(t_ls_c6_view_wi_fi_ble_stay, "Wi-Fi/BLE stay OFF until you reboot P4."),TUI_YELLOW|TUI_BRIGHT);
        y=paragraph(sf,detail,y,s_force ? "FORCE ON: same/older version permitted once." : "Only a newer version is allowed.",TUI_YELLOW);
    } else {
        y=paragraph(sf,detail,y+1,failed ? "RECOVERY / NEXT STEP" : "CHECKS / SAFETY",hue|TUI_BRIGHT);
        y=paragraph(sf,detail,y,unavailable ? "Return to UPDATE; no C6 action is available." : failed ? (boot ? "Boot not verified. Inspect C6 before retrying." : "Old C6 image remains selected.") :
                    "C6 target + SHA-256 + power + version are checked before writing.",TUI_WHITE);
        y=paragraph(sf,detail,y,done ? "Reboot P4 to use Wi-Fi/BLE again." :
                    "Provisioned two-slot layout required. Keep power connected.",TUI_YELLOW);
    }
    y=paragraph(sf,detail,y+1,LS_TEXT(t_ls_c6_view_if_c6_fails_to, "If C6 fails to boot: USB-UART recovery on the C6 header. After radio shutdown, reboot P4 before retry."),TUI_YELLOW);
    if (y+3 < detail.y+detail.h) {
        y=paragraph(sf,detail,y+1,"IMAGE ON SD",TUI_CYAN|TUI_BRIGHT);
        y=paragraph(sf,detail,y,s_path,TUI_WHITE);
    }
    if (!off && y+2 < detail.y+detail.h)
        paragraph(sf,detail,y+1,s_confirm ? "Tap CONFIRM [Y] to start / CANCEL [ESC] to keep current firmware." :
                  "Tap INSTALL [I] to review / CHECK SD [R] to refresh / OPTIONS [O].",TUI_CYAN);
}
bool ls_c6_view_key(ls_tk_t key, char ch)
{
    if (s_confirm) {
        if (key == LS_TK_ESC) { s_confirm = false; return true; }
        if (ch == 'y' || ch == 'Y') install(NULL);
        return true;
    }
    if (ch == 'r' || ch == 'R') { inspect(); return true; }
    if (!blocked() && ls_opt_key(&MENU, ch)) return true;
    if (key == LS_TK_ENTER || ch == 'i' || ch == 'I') { review(NULL); return true; }
    return false;
}
bool ls_c6_view_touch(int col, int row)
{
    int button = ls_btn_hit(col,row);
    if (button == 0) {
        if (s_confirm) { s_confirm=false; return false; }
        ls_c6_view_leave(); return true;
    }
    if (button == 1) { if (s_confirm) install(NULL); else review(NULL); }
    if (button == 2 && !s_confirm) inspect();
    if (button == 3 && !s_confirm && !blocked()) ls_opt_open(&MENU);
    return false;
}

const char *ls_c6_view_hint(void)
{
    if (s_confirm) return "Y confirm install  ESC cancel  TAP filled keys";
    if (blocked()) return "ESC update  TAP BACK  Keep power connected";
    return "I review  R check SD  O options  ESC update  TAP filled keys";
}
