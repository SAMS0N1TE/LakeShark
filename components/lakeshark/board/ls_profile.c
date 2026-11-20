/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The board profile - see ls_profile.h. The table half is plain C over the
   variant header's pin map; the preferences half is the device's. */
#include "ls_profile.h"
#include "ls_board.h"
#if defined(CONFIG_LS_BOARD_T_DISPLAY_P4)
#include "ls_xl9535.h"
#endif

#if defined(ESP_PLATFORM) && LS_HAS_LORA
#include "ls_lora.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------- panels */

#if defined(CONFIG_LS_BOARD_T_DISPLAY_P4)
static const ls_panel_desc_t PANEL_RM69A10 = {
    .kind = LS_PANEL_KIND_RM69A10, .name = "rm69a10", .glass = "AMOLED",
    .w = LS_TDP4_AMOLED_H_RES, .h = LS_TDP4_AMOLED_V_RES,
    .hsync = LS_TDP4_AMOLED_HSYNC, .hbp = LS_TDP4_AMOLED_HBP, .hfp = LS_TDP4_AMOLED_HFP,
    .vsync = LS_TDP4_AMOLED_VSYNC, .vbp = LS_TDP4_AMOLED_VBP, .vfp = LS_TDP4_AMOLED_VFP,
    .dsi_lanes = LS_BOARD_LCD_DSI_LANES, .dsi_mbps = LS_BOARD_LCD_DSI_MBPS,
    .clk_mhz = LS_TDP4_AMOLED_DPI_CLK_MHZ, .perf_clk_mhz = LS_TDP4_AMOLED_DPI_PERF_CLK_MHZ,
    .corner_r = LS_TDP4_AMOLED_CORNER_R,
    .touch = LS_TOUCH_KIND_GT9895, .touch_addr = LS_TDP4_AMOLED_TOUCH_ADDR,
    .backlight = LS_BACKLIGHT_DCS, .backlight_gpio = -1,
};
static const ls_panel_desc_t PANEL_HI8561 = {
    .kind = LS_PANEL_KIND_HI8561, .name = "hi8561", .glass = "TFT",
    .w = LS_TDP4_TFT_H_RES, .h = LS_TDP4_TFT_V_RES,
    .hsync = LS_TDP4_TFT_HSYNC, .hbp = LS_TDP4_TFT_HBP, .hfp = LS_TDP4_TFT_HFP,
    .vsync = LS_TDP4_TFT_VSYNC, .vbp = LS_TDP4_TFT_VBP, .vfp = LS_TDP4_TFT_VFP,
    .dsi_lanes = LS_BOARD_LCD_DSI_LANES, .dsi_mbps = LS_BOARD_LCD_DSI_MBPS,
    .clk_mhz = LS_TDP4_TFT_DPI_CLK_MHZ, .perf_clk_mhz = LS_TDP4_TFT_DPI_PERF_CLK_MHZ,
    .corner_r = LS_TDP4_TFT_CORNER_R,
    .cutout_cx = LS_TDP4_TFT_CUTOUT_CX, .cutout_cy = LS_TDP4_TFT_CUTOUT_CY,
    .cutout_r = LS_TDP4_TFT_CUTOUT_R,
    .touch = LS_TOUCH_KIND_HI8561, .touch_addr = LS_TDP4_TFT_TOUCH_ADDR,
    .backlight = LS_BACKLIGHT_PWM, .backlight_gpio = LS_BOARD_LCD_BL_GPIO,
    .backlight_pwm_hz = LS_BOARD_LCD_BL_PWM_MAX_HZ,
};
/* Probe order with nothing remembered: the AMOLED first, as every board
   before the TFT SKU was brought up. */
static const ls_panel_desc_t *const PANELS[] = { &PANEL_RM69A10, &PANEL_HI8561 };
#define N_PANELS 2
#else
static const ls_panel_desc_t *const PANELS[1] = { NULL };
#define N_PANELS 0
#endif

/* --------------------------------------------------------------- board */

/* What each build fixes. -1: found at boot or the owner's choice. */
#if defined(CONFIG_LS_LR2021_ONLY) && CONFIG_LS_LR2021_ONLY
#define FIXED_LR   1
#define FIXED_USB  LS_PREF_OFF
#define FIXED_NET  LS_PREF_OFF
#define OTA_DIR    "/tdp4lr/"
#define OTA_BOARD  "T-Display-P4-LR"
#elif defined(CONFIG_LS_TDP4_UNIVERSAL) && CONFIG_LS_TDP4_UNIVERSAL
#define FIXED_LR   -1
#define FIXED_USB  -1
#define FIXED_NET  -1
#define OTA_DIR    "/tdp4u/"
#define OTA_BOARD  "T-Display-P4-U"
#define OTA_LEGACY true
#elif defined(CONFIG_LS_TDP4_PANEL_HI8561)
#define OTA_DIR    "/tdp4tft/"
#define OTA_BOARD  "T-Display-P4-TFT"
#elif defined(CONFIG_LS_EDITION_NET) && CONFIG_LS_EDITION_NET
#define FIXED_NET  LS_PREF_ON
#define OTA_DIR    "/tdp4net/"
#define OTA_BOARD  "T-Display-P4-NET"
#else
#define OTA_DIR    "/tdp4/"
#define OTA_BOARD  "T-Display-P4"
#endif
#ifndef FIXED_LR
#define FIXED_LR   0
#endif
#ifndef FIXED_USB
#define FIXED_USB  LS_PREF_ON
#endif
#ifndef FIXED_NET
#define FIXED_NET  LS_PREF_OFF
#endif
#ifndef OTA_LEGACY
#define OTA_LEGACY false
#endif

#define PIN(x) ((int8_t)(x))
#ifndef LS_BOARD_PANEL_FORCED
#define LS_BOARD_PANEL_FORCED 0
#endif

static const ls_board_profile_t BOARD = {
#if defined(CONFIG_LS_BOARD_T_DISPLAY_P4)
    .id = "tdp4",
#elif defined(CONFIG_LS_BOARD_P4_WIFI6)
    .id = "p4wifi6",
#elif defined(CONFIG_LS_BOARD_P4_TOUCH_LCD_4B)
    .id = "p4lcd4b",
#elif defined(CONFIG_LS_BOARD_P4_TOUCH_LCD_43)
    .id = "p4lcd43",
#else
    .id = "p4nano",
#endif
    .name = LS_BOARD_NAME,
    .platform = "esp32p4",
#ifdef LS_BOARD_FLASH_MB
    .flash_mb = LS_BOARD_FLASH_MB,
#endif
#if defined(CONFIG_LS_BOARD_T_DISPLAY_P4)
    .app_slot_bytes = 0x480000,
#endif
    .radios = 0
#if LS_HAS_LORA && LS_HAS_SX126X
        | LS_RADIO_SX1262
#endif
#if LS_HAS_LORA_LR20XX_PROBE
        | LS_RADIO_LR2021
#endif
#if LS_HAS_USB_SDR
        | LS_RADIO_USB_SDR
#endif
#ifdef LS_BOARD_MIX_CC_CS
        | LS_RADIO_CC1101 | LS_RADIO_NRF24 | LS_RADIO_NFC
#endif
        ,
    .lora = {
#ifdef LS_BOARD_SPI_SCLK_GPIO
        .sclk = LS_BOARD_SPI_SCLK_GPIO, .mosi = LS_BOARD_SPI_MOSI_GPIO, .miso = LS_BOARD_SPI_MISO_GPIO,
#else
        .sclk = -1, .mosi = -1, .miso = -1,
#endif
#ifdef LS_BOARD_LORA_CS_GPIO
        .cs = LS_BOARD_LORA_CS_GPIO,
#else
        .cs = -1,
#endif
#ifdef LS_BOARD_LORA_BUSY_GPIO
        .busy = LS_BOARD_LORA_BUSY_GPIO,
#else
        .busy = -1,
#endif
#ifdef LS_BOARD_LORA_RST_GPIO
        .rst_gpio = LS_BOARD_LORA_RST_GPIO,
#else
        .rst_gpio = -1,
#endif
#ifdef LS_BOARD_LORA_IRQ_GPIO
        .irq_gpio = LS_BOARD_LORA_IRQ_GPIO,
#else
        .irq_gpio = -1,
#endif
#ifdef LS_BOARD_XL_RADIO_RST
        .rst_xl = PIN(LS_BOARD_XL_RADIO_RST), .irq_xl = PIN(LS_BOARD_XL_RADIO_DIO1),
#else
        .rst_xl = -1, .irq_xl = -1,
#endif
    },
    .panels = PANELS, .n_panels = N_PANELS, .panel_forced = LS_BOARD_PANEL_FORCED,
    .gps = {
#ifdef LS_BOARD_GPS_UART_NUM
        .uart = LS_BOARD_GPS_UART_NUM, .rx = LS_BOARD_GPS_RX_GPIO, .tx = LS_BOARD_GPS_TX_GPIO,
        .baud = LS_BOARD_GPS_BAUD,
#else
        .uart = -1, .rx = -1, .tx = -1,
#endif
#ifdef LS_BOARD_XL_GPS_WAKE
        .wake_xl = PIN(LS_BOARD_XL_GPS_WAKE),
#else
        .wake_xl = -1,
#endif
    },
    .power = {
#if LS_HAS_IO_EXPANDER
        .expander_addr = LS_BOARD_IO_EXPANDER_ADDR,
#else
        .expander_addr = -1,
#endif
        .i2c_sda = LS_BOARD_I2C_SDA_GPIO, .i2c_scl = LS_BOARD_I2C_SCL_GPIO,
#if LS_HAS_I2C2
        .i2c2_sda = LS_BOARD_I2C2_SDA_GPIO, .i2c2_scl = LS_BOARD_I2C2_SCL_GPIO,
#else
        .i2c2_sda = -1, .i2c2_scl = -1,
#endif
#if LS_HAS_GAUGE
        .gauge_addr = LS_BOARD_GAUGE_I2C_ADDR,
#else
        .gauge_addr = -1,
#endif
#if LS_HAS_RTC
        .rtc_addr = LS_BOARD_RTC_I2C_ADDR,
#else
        .rtc_addr = -1,
#endif
#if LS_HAS_VBUS_CTRL
        .vbus_gpio = LS_BOARD_VBUS_EN_GPIO,
#else
        .vbus_gpio = -1,
#endif
    },
    .audio = {
#ifdef LS_BOARD_CODEC_I2C_ADDR
        .codec_addr = LS_BOARD_CODEC_I2C_ADDR,
#else
        .codec_addr = -1,
#endif
        .mclk = LS_BOARD_I2S_MCLK_GPIO, .bck = LS_BOARD_I2S_BCK_GPIO, .ws = LS_BOARD_I2S_WS_GPIO,
        .dout = LS_BOARD_I2S_DOUT_GPIO, .din = LS_BOARD_I2S_DIN_GPIO,
        .pa_gpio = LS_BOARD_PA_EN_GPIO,
#ifdef LS_BOARD_XL_AUDIO_PWR_EN
        .pa_xl = PIN(LS_BOARD_XL_AUDIO_PWR_EN),
#else
        .pa_xl = -1,
#endif
    },
    .sd = {
        .clk = LS_BOARD_SDIO_CLK_GPIO, .cmd = LS_BOARD_SDIO_CMD_GPIO,
        .d0 = LS_BOARD_SDIO_D0_GPIO, .d1 = LS_BOARD_SDIO_D1_GPIO,
        .d2 = LS_BOARD_SDIO_D2_GPIO, .d3 = LS_BOARD_SDIO_D3_GPIO,
#ifdef LS_BOARD_XL_SD_PWR_EN
        .power_xl = PIN(LS_BOARD_XL_SD_PWR_EN),
#else
        .power_xl = -1,
#endif
#ifdef CONFIG_LS_SD_MAX_FREQ_KHZ
        .max_khz = CONFIG_LS_SD_MAX_FREQ_KHZ,
#endif
    },
    .coproc = {
#if defined(CONFIG_SLAVE_IDF_TARGET_ESP32C5)
        .kind = LS_COPROC_C5_SDIO,
#elif LS_HAS_C6
        .kind = LS_COPROC_C6_SDIO,
#endif
        .reset_gpio = LS_BOARD_C6_EN_GPIO,
#ifdef LS_BOARD_XL_C6_EN
        .enable_xl = PIN(LS_BOARD_XL_C6_EN),
#else
        .enable_xl = -1,
#endif
    },
#if defined(CONFIG_LS_BOARD_T_DISPLAY_P4)
    .ota_dir = OTA_DIR, .ota_board = OTA_BOARD, .ota_legacy = OTA_LEGACY,
#endif
};

const ls_board_profile_t *ls_profile(void) { return &BOARD; }

const ls_panel_desc_t *ls_profile_panel_by_kind(int kind)
{
    for (int i = 0; i < BOARD.n_panels; i++)
        if (BOARD.panels[i] && BOARD.panels[i]->kind == kind) return BOARD.panels[i];
    return NULL;
}

/* ------------------------------------------------------ pure decisions */

int ls_profile_panel_order(const ls_board_profile_t *b, int build_forced, int owner_forced,
                           int last_seen, const ls_panel_desc_t **out, int cap)
{
    if (!b || !out || cap < 1) return 0;
    int n = 0;
    /* A build that fixes the panel asks that one and no other. A kind this
       board does not have is ignored. */
    for (int i = 0; build_forced && i < b->n_panels; i++)
        if (b->panels[i]->kind == build_forced) { out[0] = b->panels[i]; return 1; }
    /* The owner's choice goes first but the rest are still asked: a wrong
       'board panel' costs one missed ID read, never a dark screen. */
    const int first = owner_forced ? owner_forced : last_seen;
    for (int i = 0; i < b->n_panels && n < cap; i++)
        if (b->panels[i]->kind == first) out[n++] = b->panels[i];
    for (int i = 0; i < b->n_panels && n < cap; i++)
        if (b->panels[i]->kind != first) out[n++] = b->panels[i];
    return n;
}

bool ls_profile_usb_wanted(const ls_board_profile_t *b, int pref, uint32_t radio)
{
    if (!b || !(b->radios & LS_RADIO_USB_SDR)) return false;
    if (pref == LS_PREF_ON) return true;
    if (pref == LS_PREF_OFF) return false;
    return radio != LS_RADIO_LR2021;
}

void ls_profile_legacy_edition(int panel_kind, uint32_t radio, bool net,
                               const char **dir, const char **board)
{
    const char *d = "/tdp4/", *n = "T-Display-P4";
    if (panel_kind == LS_PANEL_KIND_HI8561) { d = "/tdp4tft/"; n = "T-Display-P4-TFT"; }
    else if (net && radio != LS_RADIO_LR2021) { d = "/tdp4net/"; n = "T-Display-P4-NET"; }
    else if (radio == LS_RADIO_LR2021)      { d = "/tdp4lr/";  n = "T-Display-P4-LR"; }
    if (dir) *dir = d;
    if (board) *board = n;
}

#ifndef CONFIG_LS_LAP_CAPTURE_SLOTS
#define CONFIG_LS_LAP_CAPTURE_SLOTS 256
#endif
#ifndef CONFIG_LS_HOSTED_PROMISC_SNAP
#define CONFIG_LS_HOSTED_PROMISC_SNAP 512
#endif
#ifndef CONFIG_LS_HOSTED_PROMISC_MAX_PER_S
#define CONFIG_LS_HOSTED_PROMISC_MAX_PER_S 250
#endif

void ls_profile_net_sizes(bool net, ls_profile_net_t *out)
{
    if (!out) return;
    out->capture_slots = CONFIG_LS_LAP_CAPTURE_SLOTS;
    out->promisc_snap = CONFIG_LS_HOSTED_PROMISC_SNAP;
    out->promisc_per_s = CONFIG_LS_HOSTED_PROMISC_MAX_PER_S;
    if (!net) return;
    /* boards/t_display_p4_net.defaults, as numbers. */
    if (out->capture_slots < 4096) out->capture_slots = 4096;
    if (out->promisc_snap < 1024) out->promisc_snap = 1024;
    if (out->promisc_per_s < 1000) out->promisc_per_s = 1000;
}

/* ------------------------------------------------------- what was found */

static const ls_panel_desc_t *s_panel;
static uint32_t s_radio;
static ls_profile_prefs_t s_prefs;
static bool s_usb_host, s_usb_decided;

const ls_panel_desc_t *ls_profile_panel(void) { return s_panel; }
void ls_profile_note_panel(const ls_panel_desc_t *panel) { s_panel = panel; }
uint32_t ls_profile_radio(void)
{
#if defined(ESP_PLATFORM) && LS_HAS_LORA
    /* The part that answered this boot, kept through a park; a header module
       that only answered a later reset is picked up when it does. */
    if (!s_radio) {
        const ls_lora_chip_t c = ls_lora_fitted();
        s_radio = c == LS_LORA_CHIP_LR20XX ? LS_RADIO_LR2021 : c == LS_LORA_CHIP_SX126X ? LS_RADIO_SX1262 : 0;
    }
#endif
    return s_radio;
}
void ls_profile_note_radio(uint32_t radio) { s_radio = radio; }
const ls_profile_prefs_t *ls_profile_prefs(void) { return &s_prefs; }

bool ls_profile_is_lr(void)
{
    return FIXED_LR < 0 ? ls_profile_radio() == LS_RADIO_LR2021 : FIXED_LR;
}

bool ls_profile_net(void)
{
    return (FIXED_NET < 0 ? s_prefs.net : FIXED_NET) == LS_PREF_ON;
}

void ls_profile_decide_usb(void)
{
    s_usb_host = ls_profile_usb_wanted(&BOARD, FIXED_USB < 0 ? s_prefs.usb : FIXED_USB, ls_profile_radio());
    s_usb_decided = true;
}

bool ls_profile_usb_host(void)
{
    if (!s_usb_decided) ls_profile_decide_usb();
    return s_usb_host;
}

static const char *pref_name(int v)
{
    return v == LS_PREF_ON ? "on" : v == LS_PREF_OFF ? "off" : "auto";
}

int ls_profile_describe(char *out, size_t cap)
{
    const ls_panel_desc_t *p = s_panel;
    const uint32_t s_radio = ls_profile_radio();
    const char *dir = BOARD.ota_dir ? BOARD.ota_dir : "-", *old = "-";
    if (BOARD.ota_legacy)
        ls_profile_legacy_edition(p ? p->kind : 0, s_radio, ls_profile_net(), &old, NULL);
    return snprintf(out, cap,
        "board: %s (%s, %u MB flash)\n"
        "radio: %s%s\n"
        "panel: %s %ux%u%s\n"
        "usb host: %s (setting %s)\n"
        "net buffers: %s (setting %s)\n"
        "updates: %s%s%s\n",
        BOARD.name, BOARD.id, (unsigned)BOARD.flash_mb,
        s_radio == LS_RADIO_LR2021 ? "LR2021" : s_radio == LS_RADIO_SX1262 ? "SX1262" : "none found",
        ls_profile_is_lr() ? " (LakeShark LR)" : "",
        p ? p->glass : "none found", p ? p->w : 0, p ? p->h : 0,
        BOARD.panel_forced ? " (fixed by the build)" : s_prefs.panel ? " ('board panel' asks it first)" : "",
        ls_profile_usb_host() ? "on" : "off", FIXED_USB < 0 ? pref_name(s_prefs.usb) : "fixed by the build",
        ls_profile_net() ? "large" : "standard", FIXED_NET < 0 ? pref_name(s_prefs.net) : "fixed by the build",
        dir, BOARD.ota_legacy ? ", then " : "", BOARD.ota_legacy ? old : "");
}

/* --------------------------------------------------------- device side */

#ifdef ESP_PLATFORM
#include "nvs.h"
#include "ls_nvs_safe.h"

#define NS "ls_board"

/* The editions before the universal image by update folder. "led" in NVS is
   1 + the index of the one this hardware was found to be (0 none). */
static const char *const OLD_DIRS[4] = { "/tdp4/", "/tdp4lr/", "/tdp4tft/", "/tdp4net/" };
static uint8_t s_led;           /* as remembered before this boot */
static bool s_led_kept;         /* ... in NVS, not only read from the URL */

static esp_err_t prefs_load(void *ctx)
{
    (void)ctx;
    nvs_handle_t h;
    bool have_usb = false, have_net = false;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        (void)nvs_get_u8(h, "panel", &s_prefs.panel);
        (void)nvs_get_u8(h, "seen", &s_prefs.panel_seen);
        have_usb = nvs_get_u8(h, "usb", &s_prefs.usb) == ESP_OK;
        have_net = nvs_get_u8(h, "net", &s_prefs.net) == ESP_OK;
        s_led_kept = nvs_get_u8(h, "led", &s_led) == ESP_OK;
        nvs_close(h);
    }
    const bool prefs_set = (have_usb && have_net) || FIXED_USB >= 0 || FIXED_NET >= 0;
    if (prefs_set && (s_led_kept || !OTA_LEGACY)) return ESP_OK;
    /* First boot of a universal image on a board that ran one edition: the
       update folder it saved says which. A standard image on an LR2021 board
       had its USB port, the network edition had its buffers; keep both. */
    char url[200];
    size_t n = sizeof(url);
    if (nvs_open("ls_ota", NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_str(h, "url", url, &n) == ESP_OK) {
            for (uint8_t i = 0; !s_led_kept && i < 4; i++)
                if (strstr(url, OLD_DIRS[i])) s_led = (uint8_t)(i + 1);
            if (!have_net && strstr(url, "/tdp4net/")) s_prefs.net = LS_PREF_ON;
            if (!have_usb && (strstr(url, "/tdp4/") || strstr(url, "/tdp4net/") || strstr(url, "/tdp4tft/")))
                s_prefs.usb = LS_PREF_ON;
        }
        nvs_close(h);
    }
    if (prefs_set) return ESP_OK;
    /* Kept now: the update code moves that URL to the new folder later. */
    if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
        if (!have_usb) (void)nvs_set_u8(h, "usb", s_prefs.usb);
        if (!have_net) (void)nvs_set_u8(h, "net", s_prefs.net);
        (void)nvs_commit(h);
        nvs_close(h);
    }
    return ESP_OK;
}

typedef struct { const char *key; uint8_t value; } put_t;

static esp_err_t prefs_put(void *ctx)
{
    const put_t *p = ctx;
    nvs_handle_t h;
    esp_err_t e = nvs_open(NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    e = nvs_set_u8(h, p->key, p->value);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e;
}

void ls_profile_boot(void)
{
    (void)ls_nvs_run(prefs_load, NULL, 3072);
}

void ls_profile_remember_panel(int kind)
{
    if (kind == s_prefs.panel_seen) return;
    put_t p = { "seen", (uint8_t)kind };
    if (ls_nvs_run(prefs_put, &p, 3072) == ESP_OK) s_prefs.panel_seen = (uint8_t)kind;
}

const char *ls_profile_legacy_dir(const char **board)
{
    const int kind = s_panel ? s_panel->kind : 0;
    const uint32_t radio = ls_profile_radio();
    if (kind != LS_PANEL_KIND_HI8561 && (kind != LS_PANEL_KIND_RM69A10 || !radio)) return NULL;
    const char *dir, *name;
    ls_profile_legacy_edition(kind, radio, ls_profile_net(), &dir, &name);
    uint8_t ed = 0;
    for (uint8_t i = 0; i < 4; i++)
        if (!strcmp(dir, OLD_DIRS[i])) ed = (uint8_t)(i + 1);
    static bool noted;      /* once a boot; RAM keeps the boot's value */
    if (!noted && (!s_led_kept || ed != s_led)) {
        noted = true;
        put_t p = { "led", ed };
        (void)ls_nvs_run(prefs_put, &p, 3072);
    }
    if (ed != s_led) return NULL;
    if (board) *board = name;
    return dir;
}

int ls_profile_set_pref(const char *key, int value)
{
    uint8_t *slot = !strcmp(key, "panel") ? &s_prefs.panel : !strcmp(key, "usb") ? &s_prefs.usb
                  : !strcmp(key, "net") ? &s_prefs.net : NULL;
    if (!slot) return -1;
    put_t p = { key, (uint8_t)value };
    if (ls_nvs_run(prefs_put, &p, 3072) != ESP_OK) return -1;
    *slot = (uint8_t)value;
    return 0;
}

int ls_profile_command(int argc, char **argv)
{
    static const char USAGE[] = "usage: board | board usb auto|on|off | board net on|off | board panel auto|amoled|tft\n";
    if (argc >= 3) {
        const char *k = argv[1], *v = argv[2];
        int value = -1;
        if (!strcmp(k, "panel"))
            value = !strcmp(v, "auto") ? 0 : !strcmp(v, "amoled") ? LS_PANEL_KIND_RM69A10
                  : !strcmp(v, "tft") ? LS_PANEL_KIND_HI8561 : -1;
        else if (!strcmp(k, "usb") || !strcmp(k, "net"))
            value = !strcmp(v, "auto") ? LS_PREF_AUTO : !strcmp(v, "on") ? LS_PREF_ON
                  : !strcmp(v, "off") ? LS_PREF_OFF : -1;
        if (value < 0) { fputs(USAGE, stdout); return 1; }
        if ((!strcmp(k, "usb") && FIXED_USB >= 0) || (!strcmp(k, "net") && FIXED_NET >= 0) ||
            (!strcmp(k, "panel") && BOARD.panel_forced)) {
            printf("board: this build fixes %s; the universal image has the setting\n", k);
            return 1;
        }
        if (ls_profile_set_pref(k, value) != 0) { printf("board: could not save %s\n", k); return 1; }
        printf("board: %s saved, restart to apply\n", k);
        return 0;
    }
    if (argc != 1) { fputs(USAGE, stdout); return 1; }
    char text[420];
    ls_profile_describe(text, sizeof(text));
    fputs(text, stdout);
    return 0;
}
#else
void ls_profile_boot(void) {}
void ls_profile_remember_panel(int kind) { s_prefs.panel_seen = (uint8_t)kind; }
const char *ls_profile_legacy_dir(const char **board) { (void)board; return NULL; }
int ls_profile_set_pref(const char *key, int value) { (void)key; (void)value; return -1; }
int ls_profile_command(int argc, char **argv) { (void)argc; (void)argv; return 1; }
#endif
