/* SPDX-License-Identifier: GPL-3.0-or-later
 * Waveshare MIPI-DSI panels on the ESP32-P4-NANO's DSI connector, chosen at
 * runtime and drawn by the TUI through the same ls_panel.h framebuffer API as
 * the T-Display-P4's built-in panel (ls_panel.c).
 *
 * Panel facts are from Waveshare-ESP32-components (Apache-2.0): the
 * esp32_p4_nano BSP (2 lanes, DSI PHY on LDO3 at 2.5 V, backlight through the
 * panel's I2C bridge at 0x45, GT911 touch) and the esp_lcd_dsi, jd9365,
 * ili9881c and hx8394 drivers (timings, bridge power-up, controller
 * preambles). The controller tables are generated into ls_panel_ws_tables.h.
 * Waveshare's drivers are not linked: they open I2C with the legacy driver on
 * the codec's pins, and IDF refuses both drivers in one image.
 */
#include "ls_panel.h"
#include "ls_panel_ws.h"
#include "ls_board.h"
#include "ls_textpack.h"

#if defined(LS_BOARD_PANEL_RUNTIME)
#include "ls_i2c.h"
#include "ls_touch.h"
#include "ls_nvs_safe.h"
#include "esp_attr.h"
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "esp_ldo_regulator.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/mipi_dsi_ll.h"
#include "nvs.h"
#include "ls_panel_dsi_id.h"
#include "ls_panel_ws_tables.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "panel";

#define BRIDGE_ADDR 0x45

typedef enum { CTL_NONE, CTL_JD9365, CTL_ILI9881C, CTL_HX8394, CTL_ST7701 } ctl_t;
/* A: the TOUCH-A/B adapters (power 0x95, backlight 0x96 or 0x86).
   C: the rev-3-only DSI LCDs (power 0xC0.., backlight 0xAB inverted).
   RPI: panels that copy the official Raspberry Pi 7" display: an ATtiny at
   0x45 (ID 0xDE or 0xC3 in reg 0x80, power 0x85, backlight PWM 0x86) and a
   TC358762 DSI-to-DPI bridge set up over DSI, one lane, RGB888. Register
   facts from the Raspberry Pi kernel's panel-raspberrypi-touchscreen. */
typedef enum { BRIDGE_A, BRIDGE_C, BRIDGE_RPI } bridge_t;

typedef struct {
    const char *name, *label;
    uint16_t w, h, mbps;
    uint8_t dpi_mhz;
    uint16_t hsync, hbp, hfp, vsync, vbp, vfp;
    ctl_t ctl;
    const uint8_t *table;
    uint16_t table_len;
    bridge_t bridge;
    uint8_t bl_reg;
    ls_panel_ws_touch_map_t touch;
    bool rev3_only;
    uint8_t lanes;                  /* 0: two */
} ws_panel_t;

#define TBL(t) (t), (uint16_t)sizeof(t)
#define NOTBL NULL, 0
/* Raw-to-native touch maps, from the BSP's touch flags undone through its
   180 degree display rotation. Unchecked on glass: 'panel touch' overrides. */
#define T_ID   { false, false, false }
#define T_180  { false, true,  true  }
#define T_SWX  { true,  true,  false }
#define T_SWY  { true,  false, true  }

static const ws_panel_t CATALOG[] = {
    { "4a", "4-DSI-TOUCH-A", 480, 800, 500, 30, 12, 42, 42, 8, 2, 60, CTL_ST7701, TBL(WS_ST7701_4A), BRIDGE_A, 0x96, T_ID, false },
    { "10.1a", "10.1-DSI-TOUCH-A",   800, 1280, 1500, 80, 20,  20, 40,  4, 10, 30, CTL_JD9365,   TBL(WS_JD9365_101A), BRIDGE_A, 0x96, T_ID,  false },
    { "101m",  "101M-8001280-IPS-CT-K", 800, 1280, 1500, 80, 20, 20, 40, 4, 10, 30, CTL_JD9365,  TBL(WS_JD9365_101A), BRIDGE_A, 0x86, T_ID,  false },
    { "8a",    "8-DSI-TOUCH-A",      800, 1280, 1500, 80, 20,  20, 40,  4, 10, 30, CTL_JD9365,   TBL(WS_JD9365_8A),   BRIDGE_A, 0x96, T_180, false },
    { "7a",    "7-DSI-TOUCH-A",      720, 1280, 1000, 80, 50, 239, 33, 30, 20,  2, CTL_ILI9881C, TBL(WS_ILI9881C_7A), BRIDGE_A, 0x96, T_ID,  false },
    { "5a",    "5-DSI-TOUCH-A",      720, 1280,  700, 58, 20,  20, 40,  4, 10, 24, CTL_HX8394,   TBL(WS_HX8394_5A),   BRIDGE_A, 0x96, T_180, false },
    { "10.1b", "10.1-DSI-TOUCH-B",   720, 1280, 1200, 70, 50,  50, 50,  4, 12, 26, CTL_JD9365,   TBL(WS_JD9365_101B), BRIDGE_A, 0x96, T_ID,  false },
    { "9b",    "9-DSI-TOUCH-B",      720, 1280, 1200, 70, 50,  50, 50,  4, 12, 26, CTL_JD9365,   TBL(WS_JD9365_9B),   BRIDGE_A, 0x96, T_ID,  false },
    { "2.8",   "2.8inch DSI LCD",    480,  640, 1500, 48, 150, 150, 50, 150, 50, 150, CTL_NONE, NOTBL, BRIDGE_C, 0xAB, T_SWX, true },
    { "3.4c",  "3.4inch DSI LCD (C)", 800, 800, 1500, 48,  6,  40, 120,  4, 12, 16, CTL_NONE,   NOTBL, BRIDGE_C, 0xAB, T_180, true },
    { "4c",    "4inch DSI LCD (C)",  720,  720, 1500, 48, 200, 32, 120, 16,  4,  8, CTL_NONE,   NOTBL, BRIDGE_C, 0xAB, T_180, true },
    { "4",     "4inch DSI LCD",      480,  800, 1500, 48, 100, 150, 150, 100, 20, 20, CTL_NONE, NOTBL, BRIDGE_C, 0xAB, T_SWY, true },
    { "5d",    "5inch DSI LCD (D)",  720, 1280, 1500, 80, 100, 100, 80, 20, 20, 20, CTL_NONE,   NOTBL, BRIDGE_C, 0xAB, T_180, true },
    { "6.25",  "6.25inch DSI LCD",   720, 1560, 1500, 48, 50,  50, 50, 20, 20, 20, CTL_NONE,    NOTBL, BRIDGE_C, 0xAB, T_180, true },
    { "5c",    "5inch DSI LCD (C)", 1024,  600, 1500, 48, 100, 100, 100, 10, 10, 10, CTL_NONE,  NOTBL, BRIDGE_C, 0xAB, T_180, true },
    { "7c",    "7inch DSI LCD (C)", 1024,  600, 1500, 48, 100, 100, 100, 10, 10, 10, CTL_NONE,  NOTBL, BRIDGE_C, 0xAB, T_180, true },
    { "7.9",   "7.9inch DSI LCD",    400, 1280, 1200, 48, 30,  40, 40, 10, 20, 20, CTL_NONE,    NOTBL, BRIDGE_C, 0xAB, T_180, true },
    { "7e",    "7inch DSI LCD (E)", 1280,  800, 1250, 80, 40, 156, 20, 40, 48, 40, CTL_NONE,    NOTBL, BRIDGE_C, 0xAB, T_180, true },
    { "8c",    "8inch DSI LCD (C)", 1280,  800, 1250, 80, 40, 156, 20, 40, 48, 40, CTL_NONE,    NOTBL, BRIDGE_C, 0xAB, T_180, true },
    { "10.1c", "10.1inch DSI LCD (C)", 1280, 800, 1250, 80, 40, 156, 20, 40, 48, 40, CTL_NONE,  NOTBL, BRIDGE_C, 0xAB, T_180, true },
    { "8.8",   "8.8inch DSI LCD",    480, 1920, 1500, 48, 50,  50, 50, 20, 20, 20, CTL_NONE,    NOTBL, BRIDGE_C, 0xAB, T_180, true },
    { "11.9",  "11.9inch DSI LCD",   320, 1480, 1500, 48, 60,  60, 60, 20, 20, 20, CTL_NONE,    NOTBL, BRIDGE_C, 0xAB, T_180, true },
    { "7h",    "7inch DSI LCD (H)", 1280,  720, 1500, 80, 64,  64, 64, 64, 64, 64, CTL_NONE,    NOTBL, BRIDGE_C, 0xAB, T_180, true },
    /* 800x480 at 25.98 MHz (htotal 849, vtotal 510): the official display's
       mode. The lane rate is a starting point; 'panel mbps' tunes it. */
    { "4.3",   "4.3inch DSI LCD (Pi 7in protocol)", 800, 480, 650, 26, 2, 46, 1, 2, 21, 7, CTL_NONE, NOTBL, BRIDGE_RPI, 0x86, T_ID, false, 1 },
};
#define N_PANELS (sizeof(CATALOG) / sizeof(CATALOG[0]))

/* No glass at all: a framebuffer nothing scans, so the TUI runs and its only
   screen is a terminal on another machine (ls_rterm: 'term serial', 'term
   net on'). Its size is the grid's: 'panel term <cols>x<rows>' saves one, in
   cells of the default font (10x17) with a cell's margin all round, so the
   default 1200x510 is 118x28. */
#define TERM_CW    10
#define TERM_CH    17
#define TERM_COLS  118
#define TERM_ROWS  28
#define TERM_COLS_MAX 200          /* 'panel term' and 'term size' stop here */
#define TERM_ROWS_MAX 60
/* Portrait is its own size, not the landscape one on its side: turned, a
   wide grid came out as a tall one no screen was laid out for. 510x1200 is
   the portrait the UI was drawn for (the T-Display's 52x70, near enough). */
#define TERM_PORTRAIT_W 510
#define TERM_PORTRAIT_H 1200
static ws_panel_t VIRTUAL = { .name = "term", .label = "no display: remote terminal only",
                              .w = (TERM_COLS + 2) * TERM_CW, .h = (TERM_ROWS + 2) * TERM_CH, .touch = T_ID };
static uint16_t s_term_land_w = (TERM_COLS + 2) * TERM_CW, s_term_land_h = (TERM_ROWS + 2) * TERM_CH;
/* The terminal-only view as one word, w << 16 | h: the UI task turns it while
   console commands read it, and two separate halves could pair a portrait
   width with a landscape height. */
static uint32_t s_term_wh;

static const ws_panel_t *find_panel(const char *name)
{
    for (size_t i = 0; name && i < N_PANELS; i++)
        if (!strcasecmp(name, CATALOG[i].name)) return &CATALOG[i];
    if (name && !strcasecmp(name, VIRTUAL.name)) return &VIRTUAL;
    return NULL;
}

/* ------------------------------------------------------------- settings -- */

#define NVS_NS     "ls_panel"
#define TMAP_SET   0x80

typedef struct {
    bool write;
    char model[12];
    uint8_t tmap;       /* TMAP_SET | swap | mirror x << 1 | mirror y << 2 */
    uint16_t mbps;      /* lane rate override, 0 = the panel's own */
    uint8_t tcols, trows; /* the terminal-only grid, 0 = TERM_COLS x TERM_ROWS */
} saved_t;

static esp_err_t saved_job(void *ctx)
{
    saved_t *s = ctx;
    nvs_handle_t h;
    esp_err_t e = nvs_open(NVS_NS, s->write ? NVS_READWRITE : NVS_READONLY, &h);
    if (e != ESP_OK) return e;
    if (s->write) {
        e = s->model[0] ? nvs_set_str(h, "model", s->model) : nvs_erase_key(h, "model");
        if (e == ESP_ERR_NVS_NOT_FOUND) e = ESP_OK;
        if (e == ESP_OK) e = s->tmap ? nvs_set_u8(h, "tmap", s->tmap) : nvs_erase_key(h, "tmap");
        if (e == ESP_ERR_NVS_NOT_FOUND) e = ESP_OK;
        if (e == ESP_OK) e = s->mbps ? nvs_set_u16(h, "mbps", s->mbps) : nvs_erase_key(h, "mbps");
        if (e == ESP_ERR_NVS_NOT_FOUND) e = ESP_OK;
        if (e == ESP_OK) e = s->tcols ? nvs_set_u8(h, "tcols", s->tcols) : nvs_erase_key(h, "tcols");
        if (e == ESP_ERR_NVS_NOT_FOUND) e = ESP_OK;
        if (e == ESP_OK) e = s->trows ? nvs_set_u8(h, "trows", s->trows) : nvs_erase_key(h, "trows");
        if (e == ESP_ERR_NVS_NOT_FOUND) e = ESP_OK;
        if (e == ESP_OK) e = nvs_commit(h);
    } else {
        size_t n = sizeof(s->model);
        if (nvs_get_str(h, "model", s->model, &n) != ESP_OK) s->model[0] = 0;
        if (nvs_get_u8(h, "tmap", &s->tmap) != ESP_OK) s->tmap = 0;
        if (nvs_get_u16(h, "mbps", &s->mbps) != ESP_OK) s->mbps = 0;
        if (nvs_get_u8(h, "tcols", &s->tcols) != ESP_OK) s->tcols = 0;
        if (nvs_get_u8(h, "trows", &s->trows) != ESP_OK) s->trows = 0;
    }
    nvs_close(h);
    return e;
}

static void saved_read(saved_t *s)
{
    memset(s, 0, sizeof(*s));
    if (ls_nvs_run(saved_job, s, 0) != ESP_OK) { s->model[0] = 0; s->tmap = 0; }
}

static esp_err_t saved_write(saved_t *s)
{
    s->write = true;
    return ls_nvs_run(saved_job, s, 0);
}

/* --------------------------------------------------------------- bridge -- */

static i2c_master_dev_handle_t s_bridge;

static esp_err_t bridge_open(void)
{
    if (s_bridge) return ESP_OK;
    esp_err_t e = ls_i2c_probe(LS_I2C_PRIMARY, BRIDGE_ADDR, 30);
    if (e != ESP_OK) return e;
    return ls_i2c_device(LS_I2C_PRIMARY, BRIDGE_ADDR, 100000, &s_bridge);
}

static esp_err_t bridge_write(uint8_t reg, uint8_t value)
{
    const uint8_t b[2] = { reg, value };
    return i2c_master_transmit(s_bridge, b, sizeof(b), 50);
}

/* The adapter's power-up, as each Waveshare driver sends it before any DSI
   traffic, then the second they wait for the panel to come up. */
static esp_err_t bridge_power_up(const ws_panel_t *p)
{
    static const uint8_t A[][2] = { {0x95, 0x11}, {0x95, 0x17}, {0x96, 0x00}, {0x96, 0xFF} };
    static const uint8_t C[][2] = { {0xC0, 0x01}, {0xC2, 0x01}, {0xAC, 0x01},
                                    {0xAB, 0x00}, {0xAA, 0x01}, {0xAD, 0x01} };
    const uint8_t (*seq)[2] = p->bridge == BRIDGE_A ? A : C;
    const size_t n = p->bridge == BRIDGE_A ? sizeof(A) / sizeof(A[0]) : sizeof(C) / sizeof(C[0]);
    for (size_t i = 0; i < n; i++) {
        esp_err_t e = bridge_write(seq[i][0], seq[i][1]);
        if (e != ESP_OK) return e;
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
    return ESP_OK;
}

static esp_err_t bridge_read(uint8_t reg, uint8_t *value)
{
    return i2c_master_transmit_receive(s_bridge, &reg, 1, value, 1, 50);
}

/* Official-display ATtiny: check the ID, power the bridge, wait for it. */
static esp_err_t rpi_power_up(void)
{
    uint8_t id = 0;
    esp_err_t e = bridge_read(0x80, &id);
    if (e != ESP_OK) return e;
    if (id != 0xDE && id != 0xC3) {
        ESP_LOGE(TAG, "0x45 ID 0x%02X is not a Pi 7in display controller (0xDE or 0xC3)", id);
        return ESP_ERR_NOT_FOUND;
    }
    if ((e = bridge_write(0x85, 0)) != ESP_OK) return e;   /* off first, as the Pi does */
    vTaskDelay(pdMS_TO_TICKS(30));
    if ((e = bridge_write(0x86, 0)) != ESP_OK) return e;
    if (id == 0xDE) {
        /* first firmware: one power register, then wait for the bridge */
        vTaskDelay(pdMS_TO_TICKS(95));
        if ((e = bridge_write(0x85, 1)) != ESP_OK) return e;
        vTaskDelay(pdMS_TO_TICKS(25));
        uint8_t portb = 0;
        for (int i = 0; i < 100 && !(portb & 1); i++) {
            if (bridge_read(0x82, &portb) != ESP_OK) portb = 0;
            if (!(portb & 1)) vTaskDelay(pdMS_TO_TICKS(1));
        }
        ESP_LOGI(TAG, "Pi display controller ID 0x%02X, power %s", id, (portb & 1) ? "up" : "not confirmed");
        return ESP_OK;
    }
    /* 0xC3 firmware, as the Pi kernel's attiny regulator drives it: ports
       instead of the power register. PORTA bit 2 orientation, PORTB bit 7
       LCD main power, PORTC bit 0 backlight LED enable, then PORTC bits 1-3
       release the touch, LCD and bridge resets; one bridge register (0x047C)
       is cleared through the controller's own write path. */
    static const uint8_t SEQ[][2] = {
        { 0x83, 0x00 }, { 0x81, 0x04 }, { 0x82, 0x80 }, { 0x83, 0x01 },
    };
    for (size_t i = 0; i < sizeof(SEQ) / sizeof(SEQ[0]); i++) {
        if ((e = bridge_write(SEQ[i][0], SEQ[i][1])) != ESP_OK) return e;
        vTaskDelay(pdMS_TO_TICKS(i == 3 ? 80 : 10));
    }
    if ((e = bridge_write(0x83, 0x0F)) != ESP_OK) return e;   /* LED on, resets released */
    vTaskDelay(pdMS_TO_TICKS(10));
    static const uint8_t BRIDGE_REG[][2] = { { 0x8D, 0x04 }, { 0x8C, 0x7C }, { 0x90, 0x00 }, { 0x91, 0x00 } };
    for (size_t i = 0; i < sizeof(BRIDGE_REG) / sizeof(BRIDGE_REG[0]); i++) {
        if ((e = bridge_write(BRIDGE_REG[i][0], BRIDGE_REG[i][1])) != ESP_OK) return e;
        vTaskDelay(pdMS_TO_TICKS(8));
    }
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_LOGI(TAG, "Pi display controller ID 0x%02X: ports set, backlight enabled, resets released", id);
    return ESP_OK;
}

/* A DSI generic long write (0x29), sent in LP mode through the host's generic
   FIFO: the TC358762 takes its registers this way, not as DCS writes. */
static esp_err_t dsi_generic_write(const uint8_t *p, size_t n)
{
    dsi_host_dev_t *host = MIPI_DSI_LL_GET_HOST(0);
    const int64_t deadline = esp_timer_get_time() + 50000;
    mipi_dsi_host_ll_set_gen_long_wr_speed_mode(host, MIPI_DSI_LL_TRANS_SPEED_LP);
    for (size_t i = 0; i < n; i += 4) {
        uint32_t w = 0;
        memcpy(&w, p + i, n - i < 4 ? n - i : 4);
        while (mipi_dsi_host_ll_gen_is_write_fifo_full(host))
            if (!ls_panel_id_wait(deadline)) return ESP_ERR_TIMEOUT;
        mipi_dsi_host_ll_gen_write_payload_fifo(host, w);
    }
    while (mipi_dsi_host_ll_gen_is_cmd_fifo_full(host))
        if (!ls_panel_id_wait(deadline)) return ESP_ERR_TIMEOUT;
    mipi_dsi_host_ll_gen_set_packet_header(host, 0, MIPI_DSI_DT_GENERIC_LONG_WRITE,
                                           (uint8_t)(n >> 8), (uint8_t)n);
    while (!mipi_dsi_host_ll_gen_is_cmd_fifo_empty(host))
        if (!ls_panel_id_wait(deadline)) return ESP_ERR_TIMEOUT;
    return ESP_OK;
}

static esp_err_t tc358762_write(uint16_t reg, uint32_t value)
{
    const uint8_t b[6] = { reg & 0xFF, reg >> 8, value & 0xFF, (value >> 8) & 0xFF,
                           (value >> 16) & 0xFF, value >> 24 };
    return dsi_generic_write(b, sizeof(b));
}

/* The bridge set-up the Pi sends: one lane, then PPI and DSI started. */
static esp_err_t tc358762_init(void)
{
    static const struct { uint16_t reg; uint32_t value; } SETUP[] = {
        { 0x0210, 0x03 },        /* DSI_LANEENABLE: clock + D0 */
        { 0x0164, 0x05 },        /* PPI_D0S_CLRSIPOCOUNT */
        { 0x0168, 0x05 },        /* PPI_D1S_CLRSIPOCOUNT */
        { 0x0144, 0x00 },        /* PPI_D0S_ATMR */
        { 0x0148, 0x00 },        /* PPI_D1S_ATMR */
        { 0x0114, 0x03 },        /* PPI_LPTXTIMECNT */
        { 0x0450, 0x00 },        /* SPICMR */
        { 0x0420, 0x00100150 },  /* LCDCTRL */
        { 0x0464, 0x040F },      /* SYSCTRL */
    };
    esp_err_t e;
    for (size_t i = 0; i < sizeof(SETUP) / sizeof(SETUP[0]); i++)
        if ((e = tc358762_write(SETUP[i].reg, SETUP[i].value)) != ESP_OK) return e;
    vTaskDelay(pdMS_TO_TICKS(100));
    if ((e = tc358762_write(0x0104, 0x01)) != ESP_OK) return e;   /* PPI_STARTPPI */
    if ((e = tc358762_write(0x0204, 0x01)) != ESP_OK) return e;   /* DSI_STARTDSI */
    vTaskDelay(pdMS_TO_TICKS(100));
    return ESP_OK;
}

static esp_err_t bridge_brightness(const ws_panel_t *p, unsigned percent)
{
    if (!s_bridge) return ESP_ERR_INVALID_STATE;
    const uint8_t v = (uint8_t)((percent * 255 + 50) / 100);
    return bridge_write(p->bl_reg, p->bridge == BRIDGE_C ? (uint8_t)(0xFF - v) : v);
}

/* ---------------------------------------------------------------- panel -- */

static const ws_panel_t *s_model;
static esp_lcd_dsi_bus_handle_t s_bus;
static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_handle_t s_panel;
static esp_ldo_channel_handle_t s_ldo;
static uint16_t *s_frames[2];
static const unsigned s_back = 1;
static uint32_t s_refresh_count, s_last_refresh_us, s_max_gap_us, s_late_frames;
static uint32_t s_frame_us;
static ls_panel_ws_touch_map_t s_tmap;
static bool s_tried;
static esp_err_t s_result = ESP_ERR_NOT_FOUND;

static bool IRAM_ATTR refresh_done(esp_lcd_panel_handle_t panel,
    esp_lcd_dpi_panel_event_data_t *event, void *ctx)
{
    (void)panel; (void)event; (void)ctx;
    const uint32_t now = (uint32_t)esp_timer_get_time(), previous = s_last_refresh_us;
    s_last_refresh_us = now;
    if (previous) {
        const uint32_t gap = now - previous;
        if (gap > __atomic_load_n(&s_max_gap_us, __ATOMIC_RELAXED))
            __atomic_store_n(&s_max_gap_us, gap, __ATOMIC_RELAXED);
        if (gap > s_frame_us * 3 / 2) __atomic_add_fetch(&s_late_frames, 1, __ATOMIC_RELAXED);
    }
    __atomic_add_fetch(&s_refresh_count, 1, __ATOMIC_RELEASE);
    return false;
}

static esp_err_t tx(uint8_t cmd, const uint8_t *data, size_t n)
{
    return esp_lcd_panel_io_tx_param(s_io, cmd, data, n);
}

static esp_err_t send_table(const uint8_t *t, size_t len)
{
    for (size_t i = 0; i + 3 <= len; ) {
        const uint8_t cmd = t[i], n = t[i + 1], delay = t[i + 2];
        if (i + 3 + n > len) return ESP_ERR_INVALID_SIZE;
        esp_err_t e = tx(cmd, n ? &t[i + 3] : NULL, n);
        if (e != ESP_OK) return e;
        if (delay) vTaskDelay(pdMS_TO_TICKS(delay));
        i += 3 + n;
    }
    return ESP_OK;
}

/* Logged, never required: a panel that does not answer a read still runs. */
static void log_ids(const char *what, uint8_t a, uint8_t b, uint8_t c)
{
    uint8_t x = 0, y = 0, z = 0;
    const bool ok = ls_panel_read_dcs(a, &x) && ls_panel_read_dcs(b, &y) && ls_panel_read_dcs(c, &z);
    if (ok) ESP_LOGI(TAG, "%s ID %02X %02X %02X", what, x, y, z);
    else ESP_LOGW(TAG, "%s ID read got no answer", what);
}

/* Each controller's own preamble ahead of its table, as Waveshare's driver
   sends it: 2 lanes, RGB565, no mirroring. */
static esp_err_t controller_init(const ws_panel_t *p)
{
    static const uint8_t madctl = 0x00, colmod = 0x55;
    esp_err_t e = tx(0x01, NULL, 0);                         /* software reset */
    if (e != ESP_OK) return e;
    vTaskDelay(pdMS_TO_TICKS(p->ctl == CTL_NONE ? 120 : 20));
    switch (p->ctl) {
    case CTL_ILI9881C: {
        static const uint8_t page1[] = { 0x98, 0x81, 0x01 }, page0[] = { 0x98, 0x81, 0x00 };
        static const uint8_t lanes = 0x03;                   /* PAD_CONTROL: 2 lanes */
        if ((e = tx(0xFF, page1, 3)) != ESP_OK) return e;
        log_ids("ILI9881C", 0x00, 0x01, 0x02);
        if ((e = tx(0xB7, &lanes, 1)) != ESP_OK) return e;
        if ((e = tx(0xFF, page0, 3)) != ESP_OK) return e;
        if ((e = tx(0x11, NULL, 0)) != ESP_OK) return e;
        vTaskDelay(pdMS_TO_TICKS(120));
        if ((e = tx(0x36, &madctl, 1)) != ESP_OK) return e;
        if ((e = tx(0x3A, &colmod, 1)) != ESP_OK) return e;
        break;
    }
    case CTL_JD9365: {
        static const uint8_t user_page = 0x00, lanes = 0x01;  /* DSI_INT0: 2 lanes */
        log_ids("JD9365", 0xDA, 0xDB, 0xDC);
        if ((e = tx(0xE0, &user_page, 1)) != ESP_OK) return e;
        if ((e = tx(0x36, &madctl, 1)) != ESP_OK) return e;
        if ((e = tx(0x3A, &colmod, 1)) != ESP_OK) return e;
        if ((e = tx(0x80, &lanes, 1)) != ESP_OK) return e;
        break;
    }
    case CTL_HX8394: {
        static const uint8_t lanes = 0x61;                   /* 0xBA: 2 lanes */
        if ((e = tx(0x11, NULL, 0)) != ESP_OK) return e;
        vTaskDelay(pdMS_TO_TICKS(120));
        if ((e = tx(0x36, &madctl, 1)) != ESP_OK) return e;
        if ((e = tx(0x3A, &colmod, 1)) != ESP_OK) return e;
        if ((e = tx(0xBA, &lanes, 1)) != ESP_OK) return e;
        break;
    }
    case CTL_ST7701: {
        /* ST7701 user page, RGB order and 16-bit pixels before vendor table. */
        static const uint8_t page[] = {0x77, 0x01, 0x00, 0x00, 0x00};
        if ((e = tx(0xFF, page, sizeof(page))) != ESP_OK) return e;
        if ((e = tx(0x36, &madctl, 1)) != ESP_OK) return e;
        if ((e = tx(0x3A, &colmod, 1)) != ESP_OK) return e;
        break;
    }
    case CTL_NONE: {
        /* The bridge has set the panel up; it only needs waking. */
        if ((e = tx(0x36, &madctl, 1)) != ESP_OK) return e;
        static const uint8_t wake[] = { 0x11, 0, 120, 0x29, 0, 20 };
        return send_table(wake, sizeof(wake));
    }
    }
    return send_table(p->table, p->table_len);
}

static void release_all(void)
{
    if (s_panel) { esp_lcd_panel_del(s_panel); s_panel = NULL; }
    if (s_io) { esp_lcd_panel_io_del(s_io); s_io = NULL; }
    if (s_bus) { esp_lcd_del_dsi_bus(s_bus); s_bus = NULL; }
    if (s_ldo) { esp_ldo_release_channel(s_ldo); s_ldo = NULL; }
}

static esp_err_t bring_up(const ws_panel_t *p, unsigned mbps)
{
    const bool rpi = p->bridge == BRIDGE_RPI;
    const unsigned lanes = p->lanes ? p->lanes : 2;
    if (!mbps) mbps = p->mbps;
    esp_err_t err;
#define TRY(call) do { err = (call); if (err != ESP_OK) { ESP_LOGE(TAG, "%s: %s failed: %s", p->name, #call, esp_err_to_name(err)); goto fail; } } while (0)
    if (bridge_open() != ESP_OK) {
        ESP_LOGW(TAG, "%s saved, but nothing answers at 0x%02X: no panel on the DSI connector",
                 p->name, BRIDGE_ADDR);
        return ESP_ERR_NOT_FOUND;
    }
#if CONFIG_ESP32P4_REV_MIN_FULL < 300
    if (p->rev3_only) {
        ESP_LOGE(TAG, "%s needs ESP32-P4 rev 3 or later (Waveshare)", p->name);
        return ESP_ERR_NOT_SUPPORTED;
    }
#endif
    if (rpi) TRY(rpi_power_up());
    else TRY(bridge_power_up(p));
    esp_ldo_channel_config_t ldo = { .chan_id = 3, .voltage_mv = 2500 };
    TRY(esp_ldo_acquire_channel(&ldo, &s_ldo));
    esp_lcd_dsi_bus_config_t bus = {
        .bus_id = 0, .num_data_lanes = lanes,
        .phy_clk_src = 0,                 /* 0: IDF picks the source this chip revision has */
        .lane_bit_rate_mbps = mbps,
    };
    TRY(esp_lcd_new_dsi_bus(&bus, &s_bus));
    esp_lcd_dbi_io_config_t io = { .virtual_channel = 0, .lcd_cmd_bits = 8, .lcd_param_bits = 8 };
    TRY(esp_lcd_new_panel_io_dbi(s_bus, &io, &s_io));
    if (rpi) TRY(tc358762_init());
    else TRY(controller_init(p));
    esp_lcd_dpi_panel_config_t dpi = {
        .dpi_clk_src = 0,
        .dpi_clock_freq_mhz = p->dpi_mhz,
        .in_color_format = LCD_COLOR_FMT_RGB565,
        /* the TC358762 takes 24-bit pixels; the DSI bridge widens them */
        .out_color_format = rpi ? LCD_COLOR_FMT_RGB888 : LCD_COLOR_FMT_RGB565,
        .num_fbs = 2,
        .video_timing = {
            .h_size = p->w, .v_size = p->h,
            .hsync_pulse_width = p->hsync, .hsync_back_porch = p->hbp, .hsync_front_porch = p->hfp,
            .vsync_pulse_width = p->vsync, .vsync_back_porch = p->vbp, .vsync_front_porch = p->vfp,
        },
    };
    TRY(esp_lcd_new_panel_dpi(s_bus, &dpi, &s_panel));
    TRY(esp_lcd_panel_init(s_panel));
    s_frame_us = (uint32_t)((uint64_t)(p->w + p->hsync + p->hbp + p->hfp) *
                            (p->h + p->vsync + p->vbp + p->vfp) / p->dpi_mhz);
    /* Dark until the TUI's first frame is up and it sets a level. */
    (void)bridge_brightness(p, 0);
    if (rpi) (void)bridge_write(0x81, 0x04);                 /* PORTA: default orientation */
    ESP_LOGI(TAG, "%s (Waveshare %s) %ux%u scanning, %u Mbps x%u, %u MHz",
             p->name, p->label, p->w, p->h, mbps, lanes, p->dpi_mhz);
    return ESP_OK;
fail:
    release_all();
    return err;
#undef TRY
}

esp_err_t ls_panel_test_start(void)
{
    /* The saved choice is read at the UI's start, after settings are up. */
    return ESP_OK;
}

static esp_err_t virtual_up(void)
{
    saved_t sv;
    saved_read(&sv);
    const int cols = sv.tcols ? sv.tcols : TERM_COLS, rows = sv.trows ? sv.trows : TERM_ROWS;
    s_term_land_w = VIRTUAL.w = (uint16_t)((cols + 2) * TERM_CW);
    s_term_land_h = VIRTUAL.h = (uint16_t)((rows + 2) * TERM_CH);
    __atomic_store_n(&s_term_wh, (uint32_t)VIRTUAL.w << 16 | VIRTUAL.h, __ATOMIC_RELEASE);
    /* One buffer that holds either view, at the biggest grid 'term size' can
       ask for, so a window that is resized never needs a new one;
       ls_panel_ws_term_view picks which. */
    size_t px = (size_t)((TERM_COLS_MAX + 2) * TERM_CW) * ((TERM_ROWS_MAX + 2) * TERM_CH);
    if (px < (size_t)TERM_PORTRAIT_W * TERM_PORTRAIT_H) px = (size_t)TERM_PORTRAIT_W * TERM_PORTRAIT_H;
    s_frames[s_back] = heap_caps_calloc(px, sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    if (!s_frames[s_back]) return s_result = ESP_ERR_NO_MEM;
    s_model = &VIRTUAL;
    s_tmap = VIRTUAL.touch;
    ESP_LOGI(TAG, "no display: the screen is served to a terminal ('term serial', 'term net on')");
    return s_result = ESP_OK;
}

esp_err_t ls_panel_start(void)
{
    if (s_frames[s_back]) return ESP_OK;
    if (s_tried) return s_result;
    s_tried = true;
    saved_t sv;
    saved_read(&sv);
    const ws_panel_t *p = find_panel(sv.model);
    bool detected = false;
    if (!strcmp(sv.model, "none")) return s_result = ESP_ERR_NOT_FOUND;
    if (p == &VIRTUAL) return virtual_up();
    if (!p) {
        ESP_LOGI(TAG, "no saved panel: scanning adapter and GT911");
        if (bridge_open() == ESP_OK) {
            /* TOUCH-A power is needed before the touch controller answers. */
            const ws_panel_t *candidate = find_panel("4a");
            uint8_t addr = 0;
            char id[5] = {0};
            uint16_t xr = 0, yr = 0;
            if (bridge_power_up(candidate) == ESP_OK &&
                ls_touch_gt911_info(&addr, id, &xr, &yr) == ESP_OK) {
                ESP_LOGI(TAG, "scan: GT%s range %ux%u", id, xr, yr);
                /* This experimental release targets 4-DSI-TOUCH-A. Other
                   equal-size panels cannot be distinguished by GT911 alone. */
                if ((xr == 480 && yr == 800) || (xr == 800 && yr == 480)) p = candidate;
                else {
                    unsigned matches = 0;
                    for (size_t i = 0; i < N_PANELS; i++) {
                        const ws_panel_t *q = &CATALOG[i];
#if CONFIG_ESP32P4_REV_MIN_FULL < 300
                        if (q->rev3_only) continue;
#endif
                        if ((q->w == xr && q->h == yr) || (q->w == yr && q->h == xr)) {
                            p = q; matches++;
                        }
                    }
                    if (matches != 1) p = NULL;
                }
            }
        }
        if (!p) {
            /* Nothing on the connector: run the UI for a terminal instead of
               not at all. 'panel none' turns that off, for the plain headless board. */
            ESP_LOGW(TAG, "panel scan: no supported unambiguous answer. Use panel scan / panel list / panel set <name>");
            return virtual_up();
        }
        detected = true;
        sv.tmap = 0;
        sv.mbps = 0;
    }
    s_tmap = (sv.tmap & TMAP_SET)
        ? (ls_panel_ws_touch_map_t){ sv.tmap & 1, (sv.tmap >> 1) & 1, (sv.tmap >> 2) & 1 }
        : p->touch;
    esp_err_t err = bring_up(p, sv.mbps);
    if (err != ESP_OK) {
        /* A saved panel that is not plugged in today: the terminal still works. */
        ESP_LOGW(TAG, "display init failed: %s; serving a terminal instead", esp_err_to_name(err));
        return virtual_up();
    }
    err = esp_lcd_dpi_panel_get_frame_buffer(s_panel, 2, (void **)&s_frames[0], (void **)&s_frames[1]);
    if (err != ESP_OK) {
        s_frames[0] = s_frames[1] = NULL;
        ESP_LOGE(TAG, "no framebuffers: %s", esp_err_to_name(err));
        release_all();
        return s_result = err;
    }
    const esp_lcd_dpi_panel_event_callbacks_t cb = { .on_refresh_done = refresh_done };
    if (esp_lcd_dpi_panel_register_event_callbacks(s_panel, &cb, NULL) != ESP_OK)
        ESP_LOGW(TAG, "refresh count unavailable");
    s_model = p;
    if (detected) {
        snprintf(sv.model, sizeof(sv.model), "%s", p->name);
        esp_err_t saved = saved_write(&sv);
        ESP_LOGI(TAG, "auto-selected %s; NVS save: %s", p->label, esp_err_to_name(saved));
    }
    return s_result = ESP_OK;
}

static void geometry(int *w, int *h)
{
    int gw = s_model->w, gh = s_model->h;
    if (s_model == &VIRTUAL) {
        const uint32_t v = __atomic_load_n(&s_term_wh, __ATOMIC_ACQUIRE);
        gw = (int)(v >> 16);
        gh = (int)(v & 0xFFFF);
    }
    if (w) *w = gw;
    if (h) *h = gh;
}

bool ls_panel_fb(ls_panel_fb_t *out)
{
    if (!out || !s_model || !s_frames[s_back]) return false;
    out->pixels = s_frames[s_back];
    geometry(&out->width, &out->height);
    return true;
}

void ls_panel_fb_present(void)
{
    if (s_model) ls_panel_fb_present_rows(0, s_model->h);
}

void ls_panel_fb_present_rows(int y0, int y1)
{
    if (!s_panel || !s_model || !s_frames[s_back]) return;
    if (y0 < 0) y0 = 0;
    if (y1 > s_model->h) y1 = s_model->h;
    if (y1 <= y0) return;
    esp_lcd_panel_draw_bitmap(s_panel, 0, y0, s_model->w, y1, s_frames[s_back]);
}

uint32_t ls_panel_refresh_count(void)
{
    return __atomic_load_n(&s_refresh_count, __ATOMIC_ACQUIRE);
}

void ls_panel_diagnostics(void)
{
    const uint32_t a = ls_panel_refresh_count();
    vTaskDelay(pdMS_TO_TICKS(1000));
    const uint32_t b = ls_panel_refresh_count();
    printf("panel: %s, %lu refreshes, %lu in the last second, framebuffer %s\n",
           s_model ? s_model->name : "none", (unsigned long)b, (unsigned long)(b - a),
           s_frames[s_back] ? "ready" : "missing");
    printf("panel: late=%lu max_gap=%lu us expected=%lu us\n",
           (unsigned long)__atomic_load_n(&s_late_frames, __ATOMIC_RELAXED),
           (unsigned long)__atomic_load_n(&s_max_gap_us, __ATOMIC_RELAXED),
           (unsigned long)s_frame_us);
}

static unsigned s_level;
static bool s_blanked;

esp_err_t ls_panel_set_brightness(unsigned percent)
{
    if (!s_model) return ESP_ERR_INVALID_STATE;
    if (s_model == &VIRTUAL) return ESP_OK;
    if (s_blanked) return ESP_OK;
    if (percent && percent < 5) percent = 5;
    if (percent > 100) percent = 100;
    s_level = percent;
    return bridge_brightness(s_model, percent);
}

esp_err_t ls_panel_blank(unsigned fade_ms)
{
    if (!s_model) return ESP_ERR_INVALID_STATE;
    if (s_model == &VIRTUAL || s_blanked) return ESP_OK;
    const unsigned steps = fade_ms >= 40 ? 8 : 1, from = s_level ? s_level : 5;
    for (unsigned i = 1; i < steps; i++) {
        ls_panel_set_brightness(from - from * i / steps);
        vTaskDelay(pdMS_TO_TICKS(fade_ms / steps));
    }
    esp_err_t e = bridge_brightness(s_model, 0);
    s_blanked = true;
    s_level = 0;
    if (s_model->bridge == BRIDGE_RPI) return e == ESP_OK ? bridge_write(0x85, 0) : e;
    if (e == ESP_OK && s_io) e = tx(0x28, NULL, 0);          /* display off */
    return e;
}

const char *ls_panel_ws_name(void) { return s_model ? s_model->name : NULL; }

bool ls_panel_ws_term_resize(int cols, int rows)
{
    if (s_model != &VIRTUAL || cols < 40 || rows < 12 || cols > TERM_COLS_MAX || rows > TERM_ROWS_MAX) return false;
    const uint16_t w = (uint16_t)((cols + 2) * TERM_CW), h = (uint16_t)((rows + 2) * TERM_CH);
    if (w == s_term_land_w && h == s_term_land_h) return false;
    s_term_land_w = VIRTUAL.w = w;
    s_term_land_h = VIRTUAL.h = h;
    __atomic_store_n(&s_term_wh, (uint32_t)w << 16 | h, __ATOMIC_RELEASE);
    saved_t sv;                         /* and the next start comes up the same size */
    saved_read(&sv);
    sv.tcols = (uint8_t)cols;
    sv.trows = (uint8_t)rows;
    saved_write(&sv);
    return true;
}

bool ls_panel_ws_term_view(bool portrait)
{
    if (s_model != &VIRTUAL) return false;
    const uint16_t w = portrait ? TERM_PORTRAIT_W : s_term_land_w;
    const uint16_t h = portrait ? TERM_PORTRAIT_H : s_term_land_h;
    __atomic_store_n(&s_term_wh, (uint32_t)w << 16 | h, __ATOMIC_RELEASE);
    return true;
}

bool ls_panel_ws_size(int *w, int *h)
{
    if (!s_model) return false;
    geometry(w, h);
    return true;
}

bool ls_panel_ws_touch_map(ls_panel_ws_touch_map_t *out)
{
    if (!s_model || !out) return false;
    *out = s_tmap;
    return true;
}

/* -------------------------------------------------------------- console -- */

static void print_list(void)
{
    printf("panel: Waveshare DSI panels this build drives (name  size  part):\n");
    for (size_t i = 0; i < N_PANELS; i++) {
        const ws_panel_t *p = &CATALOG[i];
        printf("  %-6s %4ux%-4u %s%s\n", p->name, p->w, p->h, p->label,
               p->rev3_only ? "  (P4 rev 3)" : "");
    }
    int vw = VIRTUAL.w, vh = VIRTUAL.h;              /* the landscape size, or the view in use */
    if (s_model == &VIRTUAL) geometry(&vw, &vh);
    printf("  %-6s %4ux%-4u %s\n", VIRTUAL.name, (unsigned)vw, (unsigned)vh, VIRTUAL.label);
}

static void print_scan(void)
{
    const bool bridge = ls_i2c_probe(LS_I2C_PRIMARY, BRIDGE_ADDR, 30) == ESP_OK;
    printf("panel: adapter at 0x%02X: %s\n", BRIDGE_ADDR, bridge ? "present" : "none");
    uint8_t addr = 0;
    char id[5] = {0};
    uint16_t xr = 0, yr = 0;
    if (ls_touch_gt911_info(&addr, id, &xr, &yr) != ESP_OK) {
        printf("panel: no GT911 touch at 0x5D or 0x14\n");
        return;
    }
    printf("panel: touch GT%s at 0x%02X, range %ux%u\n", id, addr, xr, yr);
    int matches = 0;
    for (size_t i = 0; i < N_PANELS; i++) {
        const ws_panel_t *p = &CATALOG[i];
        if ((p->w == xr && p->h == yr) || (p->w == yr && p->h == xr)) {
            printf("panel:   fits %s (%s)\n", p->name, p->label);
            matches++;
        }
    }
    if (!matches) printf("panel:   no listed panel has that size\n");
}

static int cmd_panel(int argc, char **argv)
{
    saved_t sv;
    saved_read(&sv);
    if (argc < 2 || !strcmp(argv[1], "status")) {
        printf("panel: saved %s, running %s", sv.model[0] ? sv.model : "none",
               s_model ? s_model->name : "none");
        if (s_model) {
            int gw, gh;
            geometry(&gw, &gh);
            printf(" %ux%u", (unsigned)gw, (unsigned)gh);
        }
        else if (s_tried && s_result != ESP_ERR_NOT_FOUND) printf(" (%s)", esp_err_to_name(s_result));
        printf("\npanel: touch map swap=%d mirror_x=%d mirror_y=%d%s\n", s_tmap.swap_xy,
               s_tmap.mirror_x, s_tmap.mirror_y, (sv.tmap & TMAP_SET) ? " (saved)" : "");
        if (s_model) ls_panel_diagnostics();
        puts("usage: panel [status|list|scan|set <name>|none|term <cols>x<rows>|touch <swap> <mx> <my>|touch default|mbps <n|default>|bright <0-100>]");
        return 0;
    }
    if (!strcmp(argv[1], "list")) { print_list(); return 0; }
    if (!strcmp(argv[1], "scan")) { print_scan(); return 0; }
    if (!strcmp(argv[1], "none")) {
        snprintf(sv.model, sizeof(sv.model), "none");
        esp_err_t e = saved_write(&sv);
        printf("panel: %s - reboot to stop using the display\n", e == ESP_OK ? "disabled" : esp_err_to_name(e));
        return e == ESP_OK ? 0 : 1;
    }
    if (!strcmp(argv[1], "set") && argc == 3) {
        const ws_panel_t *p = find_panel(argv[2]);
        if (!p) { printf("panel: '%s' is not listed; 'panel list'\n", argv[2]); return 1; }
#if CONFIG_ESP32P4_REV_MIN_FULL < 300
        if (p->rev3_only) { printf("panel: %s needs an ESP32-P4 rev 3 build\n", p->name); return 1; }
#endif
        snprintf(sv.model, sizeof(sv.model), "%s", p->name);
        sv.tmap = 0;                                         /* back to the panel's own map */
        esp_err_t e = saved_write(&sv);
        if (e != ESP_OK) { printf("panel: save failed: %s\n", esp_err_to_name(e)); return 1; }
        printf("panel: saved %s (Waveshare %s, %ux%u) - reboot to use it\n", p->name, p->label, p->w, p->h);
        return 0;
    }
    if (!strcmp(argv[1], "touch") && (argc == 3 || argc == 5)) {
        if (argc == 3 && !strcmp(argv[2], "default")) sv.tmap = 0;
        else if (argc == 5)
            sv.tmap = TMAP_SET | (atoi(argv[2]) ? 1 : 0) | (atoi(argv[3]) ? 2 : 0) | (atoi(argv[4]) ? 4 : 0);
        else { puts("usage: panel touch <swap 0|1> <mirror_x 0|1> <mirror_y 0|1> | panel touch default"); return 1; }
        esp_err_t e = saved_write(&sv);
        if (e == ESP_OK) {
            const ws_panel_t *p = s_model ? s_model : find_panel(sv.model);
            s_tmap = (sv.tmap & TMAP_SET)
                ? (ls_panel_ws_touch_map_t){ sv.tmap & 1, (sv.tmap >> 1) & 1, (sv.tmap >> 2) & 1 }
                : (p ? p->touch : (ls_panel_ws_touch_map_t)T_ID);
        }
        printf("panel: touch map %s, in use now\n", e == ESP_OK ? "saved" : esp_err_to_name(e));
        return e == ESP_OK ? 0 : 1;
    }
    if (!strcmp(argv[1], "reg") && (argc == 3 || argc == 4)) {
        /* bench poking of the panel adapter at 0x45: read, or write a byte */
        if (bridge_open() != ESP_OK) { puts("panel: nothing at 0x45"); return 1; }
        const uint8_t reg = (uint8_t)strtoul(argv[2], NULL, 0);
        esp_err_t e;
        if (argc == 4) e = bridge_write(reg, (uint8_t)strtoul(argv[3], NULL, 0));
        uint8_t v = 0;
        e = bridge_read(reg, &v);
        printf("panel: 0x45 reg 0x%02X = 0x%02X (%s)\n", reg, v, esp_err_to_name(e));
        return e == ESP_OK ? 0 : 1;
    }
    if (!strcmp(argv[1], "mbps") && argc == 3) {
        /* DSI lane rate for the saved panel; bridges such as the TC358762
           only lock inside a window, so this is tuned on the bench */
        const int v = strcmp(argv[2], "default") ? atoi(argv[2]) : 0;
        if (v && (v < 80 || v > 1500)) { puts("panel: mbps 80-1500, or default"); return 1; }
        sv.mbps = (uint16_t)v;
        esp_err_t e = saved_write(&sv);
        printf("panel: lane rate %s%s - reboot to use it\n", v ? argv[2] : "default",
               e == ESP_OK ? "" : " (save failed)");
        return e == ESP_OK ? 0 : 1;
    }
    if (!strcmp(argv[1], "bright") && argc == 3) {
        esp_err_t e = ls_panel_set_brightness((unsigned)atoi(argv[2]));
        printf("panel: brightness %s\n", esp_err_to_name(e));
        return e == ESP_OK ? 0 : 1;
    }
    if (!strcmp(argv[1], "term") && argc <= 3) {
        /* The terminal-only screen's grid. It also chooses that screen, as
           'panel set term' does: asking for a size is asking to use it. */
        if (argc == 2) {
            printf("panel: terminal grid %ux%u%s\n", sv.tcols ? sv.tcols : TERM_COLS,
                   sv.trows ? sv.trows : TERM_ROWS, sv.tcols ? " (saved)" : " (default)");
            puts("usage: panel term <cols>x<rows> (40-200 x 12-60) | panel term default");
            return 0;
        }
        int c = 0, r = 0;
        if (!strcmp(argv[2], "default")) sv.tcols = sv.trows = 0;
        else if (sscanf(argv[2], "%dx%d", &c, &r) == 2 && c >= 40 && c <= 200 && r >= 12 && r <= 60) {
            sv.tcols = (uint8_t)c;
            sv.trows = (uint8_t)r;
        } else {
            puts("panel: term <cols>x<rows>, 40-200 columns by 12-60 rows, or default");
            return 1;
        }
        snprintf(sv.model, sizeof(sv.model), "%s", VIRTUAL.name);
        esp_err_t e = saved_write(&sv);
        if (e != ESP_OK) { printf("panel: save failed: %s\n", esp_err_to_name(e)); return 1; }
        printf("panel: terminal grid %ux%u saved - reboot to use it\n", sv.tcols ? sv.tcols : TERM_COLS,
               sv.trows ? sv.trows : TERM_ROWS);
        return 0;
    }
    puts("usage: panel [status|list|scan|set <name>|none|term <cols>x<rows>|touch <swap> <mx> <my>|touch default|mbps <n|default>|bright <0-100>]");
    return 1;
}

void ls_panel_ws_register_console(void)
{
    static const esp_console_cmd_t cmd = {
        .command = "panel",
        .help = LS_TXT(con_ls_panel_ws_panel_help, "DSI display on the connector: status, list, scan, set <name>, none, term <cols>x<rows>, touch map, bright"),
        .hint = LS_TXT(con_ls_panel_ws_panel_hint, "[status|list|scan|set <name>|none|term <c>x<r>|touch ..|bright <n>]"),
        .func = &cmd_panel,
    };
    esp_console_cmd_register(&cmd);
}

#else
const char *ls_panel_ws_name(void) { return NULL; }
bool ls_panel_ws_size(int *w, int *h) { (void)w; (void)h; return false; }
bool ls_panel_ws_term_view(bool portrait) { (void)portrait; return false; }
bool ls_panel_ws_term_resize(int cols, int rows) { (void)cols; (void)rows; return false; }
bool ls_panel_ws_touch_map(ls_panel_ws_touch_map_t *out) { (void)out; return false; }
void ls_panel_ws_register_console(void) {}
#endif
