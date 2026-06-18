#include "MeshController.hpp"

#include <cstdio>
#include <cstring>
#include "mesh_gateway.h"

LV_IMG_DECLARE(img_app_mesh);

MeshController::MeshController()

    : LsApp("Mesh", "mesh"),
      _screen(nullptr), _state_lbl(nullptr), _radio_lbl(nullptr),
      _stats_lbl(nullptr), _table(nullptr), _timer(nullptr), _row_count(0),
      _range(nullptr), _range_addr(0), _r_title(nullptr), _r_rssi(nullptr),
      _r_snr(nullptr), _r_margin_bar(nullptr), _r_margin_lbl(nullptr),
      _chart(nullptr), _series(nullptr)
{
    for (int i = 0; i < MESH_MAX_ROUTES; i++) _row_addr[i] = 0;
    for (int i = 0; i < MESH_RSSI_HISTORY; i++) _ychart[i] = LV_CHART_POINT_NONE;
}

MeshController::~MeshController() {}

bool MeshController::init(void) { return true; }

bool MeshController::run(lv_obj_t *parent)
{

    static bool s_mesh_started = false;
    if (!s_mesh_started) { s_mesh_started = true; mesh_gateway_start(); }

    buildUi(parent);
    buildRange(parent);
    _timer = lv_timer_create(timerCb, 1000, this);
    refresh();
    return true;
}

bool MeshController::back(void)
{
    if (_range && !lv_obj_has_flag(_range, LV_OBJ_FLAG_HIDDEN)) {
        closeRange();
        return true;
    }
    return exitToLauncher();
}

bool MeshController::close(void)
{
    if (_timer) { lv_timer_del(_timer); _timer = nullptr; }
    _screen = _state_lbl = _radio_lbl = _stats_lbl = _table = nullptr;
    _range = _r_title = _r_rssi = _r_snr = _r_margin_bar = _r_margin_lbl = _chart = nullptr;
    _series = nullptr;
    return true;
}

bool MeshController::pause(void)
{
    if (_timer) lv_timer_pause(_timer);
    return true;
}

bool MeshController::resume(void)
{
    if (_timer) lv_timer_resume(_timer);
    return true;
}

void MeshController::timerCb(lv_timer_t *t)
{
    static_cast<MeshController *>(t->user_data)->refresh();
}

void MeshController::buildUi(lv_obj_t *parent)
{
    _screen = parent;
    lv_obj_set_style_bg_color(_screen, lv_color_hex(0x0b1320), 0);
    lv_obj_set_style_bg_opa(_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(_screen, 20, 0);

    lv_obj_t *title = lv_label_create(_screen);
    lv_label_set_text(title, "MESH GATEWAY");
    lv_obj_set_style_text_color(title, lv_color_hex(0x9fc5ff), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    _state_lbl = lv_label_create(_screen);
    lv_obj_set_style_text_font(_state_lbl, &lv_font_montserrat_40, 0);
    lv_obj_align(_state_lbl, LV_ALIGN_TOP_LEFT, 0, 48);

    _radio_lbl = lv_label_create(_screen);
    lv_obj_set_style_text_font(_radio_lbl, &lv_font_montserrat_24, 0);
    lv_obj_align(_radio_lbl, LV_ALIGN_TOP_LEFT, 0, 112);

    _stats_lbl = lv_label_create(_screen);
    lv_obj_set_style_text_font(_stats_lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(_stats_lbl, lv_color_hex(0xcfe0ff), 0);
    lv_obj_align(_stats_lbl, LV_ALIGN_TOP_LEFT, 0, 156);

    _table = lv_table_create(_screen);
    lv_obj_set_size(_table, 680, 410);
    lv_obj_align(_table, LV_ALIGN_TOP_LEFT, 0, 200);
    lv_obj_set_style_text_font(_table, &lv_font_montserrat_20, LV_PART_ITEMS);
    lv_table_set_col_cnt(_table, 4);
    lv_table_set_col_width(_table, 0, 230);
    lv_table_set_col_width(_table, 1, 160);
    lv_table_set_col_width(_table, 2, 150);
    lv_table_set_col_width(_table, 3, 120);
    lv_table_set_cell_value(_table, 0, 0, "Node");
    lv_table_set_cell_value(_table, 0, 1, "RSSI");
    lv_table_set_cell_value(_table, 0, 2, "SNR");
    lv_table_set_cell_value(_table, 0, 3, "Hops");
    lv_obj_add_event_cb(_table, onTableClick, LV_EVENT_VALUE_CHANGED, this);

    lv_obj_t *rescan = lv_btn_create(_screen);
    lv_obj_set_size(rescan, 180, 46);
    lv_obj_align(rescan, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(rescan, lv_color_hex(0x2a5db0), 0);
    lv_obj_add_event_cb(rescan, onRescanBtn, LV_EVENT_CLICKED, this);
    lv_obj_t *rl = lv_label_create(rescan);
    lv_label_set_text(rl, LV_SYMBOL_REFRESH "  Rescan");
    lv_obj_set_style_text_font(rl, &lv_font_montserrat_20, 0);
    lv_obj_center(rl);

    lv_obj_t *reset = lv_btn_create(_screen);
    lv_obj_set_size(reset, 180, 46);
    lv_obj_align(reset, LV_ALIGN_TOP_RIGHT, 0, 54);
    lv_obj_set_style_bg_color(reset, lv_color_hex(0x7a3b1d), 0);
    lv_obj_add_event_cb(reset, onResetBtn, LV_EVENT_CLICKED, this);
    lv_obj_t *rl2 = lv_label_create(reset);
    lv_label_set_text(rl2, LV_SYMBOL_TRASH "  Reset DB");
    lv_obj_set_style_text_font(rl2, &lv_font_montserrat_20, 0);
    lv_obj_center(rl2);
}

void MeshController::onRescanBtn(lv_event_t *e)
{
    (void)e;
    mesh_gateway_rescan();
}

void MeshController::onResetBtn(lv_event_t *e)
{
    (void)e;
    static const char *btns[] = {"Reset", "Cancel", ""};
    lv_obj_t *mbox = lv_msgbox_create(NULL, "Reset node DB",
                                      "Forget all learned nodes and specs?",
                                      btns, false);
    lv_obj_add_event_cb(mbox, onResetMsgbox, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_center(mbox);
}

void MeshController::onResetMsgbox(lv_event_t *e)
{
    lv_obj_t *mbox = lv_event_get_current_target(e);
    const char *txt = lv_msgbox_get_active_btn_text(mbox);
    if (txt && strcmp(txt, "Reset") == 0) mesh_gateway_node_db_reset();
    lv_msgbox_close(mbox);
}

void MeshController::refresh(void)
{
    if (!_state_lbl) return;

    mesh_snapshot_t s;
    mesh_gateway_get_snapshot(&s);

    lv_label_set_text(_state_lbl, mesh_gateway_state_name(s.state));
    uint32_t col = 0xffb86b;
    if (s.state == 3 || s.state == 4) col = 0x7fe084;
    else if (s.state == 0 || s.state == 5) col = 0xff7a7a;
    lv_obj_set_style_text_color(_state_lbl, lv_color_hex(col), 0);

    if (s.radio_ok) {
        lv_label_set_text(_radio_lbl, LV_SYMBOL_OK "  Radio online");
        lv_obj_set_style_text_color(_radio_lbl, lv_color_hex(0x7fe084), 0);
    } else {
        lv_label_set_text(_radio_lbl, LV_SYMBOL_WARNING "  SX1262 not detected");
        lv_obj_set_style_text_color(_radio_lbl, lv_color_hex(0xff7a7a), 0);
    }

    char buf[176];
    snprintf(buf, sizeof(buf),
             "addr 0x%04X   NM 0x%04X   nodes %u   sync %s   up %us",
             s.node_addr, s.nm, (unsigned)s.nodes,
             s.synced ? "yes" : "no", (unsigned)s.uptime_s);
    lv_label_set_text(_stats_lbl, buf);

    _row_count = 0;
    if (s.route_count == 0) {
        lv_table_set_row_cnt(_table, 2);
        lv_table_set_cell_value(_table, 1, 0, "(no nodes yet)");
        lv_table_set_cell_value(_table, 1, 1, "");
        lv_table_set_cell_value(_table, 1, 2, "");
        lv_table_set_cell_value(_table, 1, 3, "");
    } else {
        lv_table_set_row_cnt(_table, s.route_count + 1);
        for (uint32_t i = 0; i < s.route_count; i++) {
            const mesh_route_t &r = s.routes[i];
            char a[48], rs[24], sn[24], hp[16];
            snprintf(a, sizeof(a), "0x%04X%s%s", r.addr,
                     r.is_nm ? " [NM]" : "", r.is_gw ? " [GW]" : "");
            snprintf(rs, sizeof(rs), "%.0f dBm", (double)r.rssi);
            snprintf(sn, sizeof(sn), "%.1f", (double)r.snr);
            snprintf(hp, sizeof(hp), "%u", r.hops);
            lv_table_set_cell_value(_table, i + 1, 0, a);
            lv_table_set_cell_value(_table, i + 1, 1, rs);
            lv_table_set_cell_value(_table, i + 1, 2, sn);
            lv_table_set_cell_value(_table, i + 1, 3, hp);
            if (i < MESH_MAX_ROUTES) _row_addr[i] = r.addr;
        }
        _row_count = s.route_count;
    }

    if (_range && !lv_obj_has_flag(_range, LV_OBJ_FLAG_HIDDEN)) refreshRange();
}

#define RANGE_FLOOR_DBM (-123.0f)

void MeshController::buildRange(lv_obj_t *screen)
{
    _range = lv_obj_create(screen);
    lv_obj_set_size(_range, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(_range, lv_color_hex(0x0b1320), 0);
    lv_obj_set_style_bg_opa(_range, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_range, 0, 0);
    lv_obj_set_style_pad_all(_range, 20, 0);
    lv_obj_add_flag(_range, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(_range, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *back = lv_btn_create(_range);
    lv_obj_set_size(back, 120, 46);
    lv_obj_align(back, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x2a5db0), 0);
    lv_obj_add_event_cb(back, onRangeBack, LV_EVENT_CLICKED, this);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, LV_SYMBOL_LEFT "  Back");
    lv_obj_set_style_text_font(bl, &lv_font_montserrat_20, 0);
    lv_obj_center(bl);

    _r_title = lv_label_create(_range);
    lv_label_set_text(_r_title, "RANGE");
    lv_obj_set_style_text_font(_r_title, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(_r_title, lv_color_hex(0x9fc5ff), 0);
    lv_obj_align(_r_title, LV_ALIGN_TOP_LEFT, 0, 0);

    _r_rssi = lv_label_create(_range);
    lv_obj_set_style_text_font(_r_rssi, &lv_font_montserrat_48, 0);
    lv_obj_align(_r_rssi, LV_ALIGN_TOP_LEFT, 0, 52);

    _r_snr = lv_label_create(_range);
    lv_obj_set_style_text_font(_r_snr, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(_r_snr, lv_color_hex(0xcfe0ff), 0);
    lv_obj_align(_r_snr, LV_ALIGN_TOP_LEFT, 0, 118);

    _r_margin_lbl = lv_label_create(_range);
    lv_obj_set_style_text_font(_r_margin_lbl, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(_r_margin_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_align(_r_margin_lbl, LV_ALIGN_TOP_LEFT, 0, 162);

    _r_margin_bar = lv_bar_create(_range);
    lv_obj_set_size(_r_margin_bar, 660, 26);
    lv_obj_align(_r_margin_bar, LV_ALIGN_TOP_LEFT, 0, 196);
    lv_bar_set_range(_r_margin_bar, 0, 40);

    _chart = lv_chart_create(_range);
    lv_obj_set_size(_chart, 660, 380);
    lv_obj_align(_chart, LV_ALIGN_TOP_LEFT, 0, 238);
    lv_chart_set_type(_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(_chart, MESH_RSSI_HISTORY);
    lv_chart_set_range(_chart, LV_CHART_AXIS_PRIMARY_Y, -130, -30);
    lv_chart_set_div_line_count(_chart, 5, 0);
    lv_obj_set_style_size(_chart, 0, LV_PART_INDICATOR);
    _series = lv_chart_add_series(_chart, lv_color_hex(0x7fe084), LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_set_ext_y_array(_chart, _series, _ychart);
}

void MeshController::openRange(uint16_t addr)
{
    if (!_range || addr == 0) return;
    _range_addr = addr;
    char t[40];
    snprintf(t, sizeof(t), "RANGE  0x%04X", addr);
    lv_label_set_text(_r_title, t);
    lv_obj_clear_flag(_range, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(_range);
    refreshRange();
}

void MeshController::closeRange(void)
{
    if (_range) lv_obj_add_flag(_range, LV_OBJ_FLAG_HIDDEN);
}

void MeshController::refreshRange(void)
{
    if (!_r_rssi) return;

    mesh_snapshot_t s;
    mesh_gateway_get_snapshot(&s);
    float rssi = 0, snr = 0;
    bool found = false;
    for (uint32_t i = 0; i < s.route_count; i++) {
        if (s.routes[i].addr == _range_addr) {
            rssi = s.routes[i].rssi; snr = s.routes[i].snr; found = true; break;
        }
    }

    char b[32];
    if (found) {
        snprintf(b, sizeof(b), "%.0f dBm", (double)rssi);
        lv_label_set_text(_r_rssi, b);
        uint32_t col = 0x7fe084;
        if (rssi < -110) col = 0xff7a7a;
        else if (rssi < -95) col = 0xffb86b;
        lv_obj_set_style_text_color(_r_rssi, lv_color_hex(col), 0);
        snprintf(b, sizeof(b), "SNR %.1f dB", (double)snr);
        lv_label_set_text(_r_snr, b);

        float margin = rssi - RANGE_FLOOR_DBM;
        if (margin < 0) margin = 0;
        lv_bar_set_value(_r_margin_bar, (int)margin, LV_ANIM_OFF);
        uint32_t mcol = margin > 15 ? 0x7fe084 : (margin > 6 ? 0xffb86b : 0xff7a7a);
        lv_obj_set_style_bg_color(_r_margin_bar, lv_color_hex(mcol), LV_PART_INDICATOR);
        snprintf(b, sizeof(b), "Link margin  +%.0f dB", (double)margin);
        lv_label_set_text(_r_margin_lbl, b);
    } else {
        lv_label_set_text(_r_rssi, "-- dBm");
        lv_obj_set_style_text_color(_r_rssi, lv_color_hex(0xff7a7a), 0);
        lv_label_set_text(_r_snr, "no route");
        lv_label_set_text(_r_margin_lbl, "Link margin  --");
        lv_bar_set_value(_r_margin_bar, 0, LV_ANIM_OFF);
    }

    static float hist[MESH_RSSI_HISTORY];
    uint32_t n = mesh_gateway_get_rssi_history(_range_addr, hist, MESH_RSSI_HISTORY);
    for (uint32_t i = 0; i < MESH_RSSI_HISTORY; i++) {
        _ychart[i] = (i < n) ? (lv_coord_t)hist[i] : LV_CHART_POINT_NONE;
    }
    lv_chart_refresh(_chart);
}

void MeshController::onTableClick(lv_event_t *e)
{
    MeshController *self = static_cast<MeshController *>(lv_event_get_user_data(e));
    lv_obj_t *tbl = lv_event_get_target(e);
    uint16_t row = 0, col = 0;
    lv_table_get_selected_cell(tbl, &row, &col);
    if (row == LV_TABLE_CELL_NONE || row == 0) return;
    uint32_t idx = row - 1;
    if (idx < self->_row_count) self->openRange(self->_row_addr[idx]);
}

void MeshController::onRangeBack(lv_event_t *e)
{
    static_cast<MeshController *>(lv_event_get_user_data(e))->closeRange();
}
