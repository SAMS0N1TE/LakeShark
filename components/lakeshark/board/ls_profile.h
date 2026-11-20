/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The board profile: what a board is wired as, what was found fitted at
   boot, and the few choices the owner makes about it.

   One image serves a board FAMILY (one PCB pin map, one flash layout, one
   co-processor transport). Inside a family the differences are found at
   boot: which panel answered on DSI, which LoRa part answered on SPI. A new
   family is a variants/<board>.h pin map plus drivers for parts no profile
   has used yet; a new SKU of a known family is one more ls_panel_desc_t or
   one more radio bit, never an #if in an app.

   Plain C, no SDK types: the host bench and a Linux port read the same
   structs. Only the NVS-backed preferences at the bottom need the device. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------- panels */

typedef enum {
    LS_PANEL_KIND_NONE = 0,
    LS_PANEL_KIND_RM69A10 = 1,      /* T-Display-P4 4.1" AMOLED */
    LS_PANEL_KIND_HI8561 = 2,       /* T-Display-P4 TFT */
    LS_PANEL_KIND_DSI_RUNTIME = 3,  /* a panel named on the console (ls_panel_ws.c) */
} ls_panel_kind_t;

typedef enum {
    LS_TOUCH_KIND_NONE = 0,
    LS_TOUCH_KIND_GT9895,
    LS_TOUCH_KIND_HI8561,
    LS_TOUCH_KIND_GT911,
} ls_touch_kind_t;

typedef enum {
    LS_BACKLIGHT_NONE = 0,
    LS_BACKLIGHT_DCS,       /* brightness is DCS 0x51 (self-lit panel) */
    LS_BACKLIGHT_PWM,       /* a PWM pin into the LED driver */
} ls_backlight_kind_t;

typedef struct {
    uint8_t  kind;                  /* ls_panel_kind_t */
    const char *name;               /* the driver's log tag: "rm69a10" */
    const char *glass;              /* what a person calls it: "AMOLED" */
    uint16_t w, h;                  /* native portrait pixels */
    uint16_t hsync, hbp, hfp, vsync, vbp, vfp;
    uint8_t  dsi_lanes;
    uint16_t dsi_mbps;
    uint8_t  clk_mhz, perf_clk_mhz; /* pixel clock, and under a capture load */
    uint16_t corner_r;              /* rounded glass, 0 for square */
    int16_t  cutout_cx, cutout_cy, cutout_r;    /* camera hole, r 0 for none */
    uint8_t  touch;                 /* ls_touch_kind_t */
    uint8_t  touch_addr;
    uint8_t  backlight;             /* ls_backlight_kind_t */
    int8_t   backlight_gpio;
    uint16_t backlight_pwm_hz;
} ls_panel_desc_t;

/* ------------------------------------------------------------- radios */

#define LS_RADIO_SX1262     (1u << 0)
#define LS_RADIO_LR2021     (1u << 1)
#define LS_RADIO_USB_SDR    (1u << 2)   /* RTL-SDR / HackRF on the USB host port */
#define LS_RADIO_CC1101     (1u << 3)   /* keyboard expansion (T-MixRF) */
#define LS_RADIO_NRF24      (1u << 4)
#define LS_RADIO_NFC        (1u << 5)

typedef enum {
    LS_COPROC_NONE = 0,
    LS_COPROC_C6_SDIO,
    LS_COPROC_C5_SDIO,
} ls_coproc_kind_t;

/* ------------------------------------------------------------ the board */

/* -1 in any pin field: not wired on this board. `xl` fields are I/O expander
   pins (0..17), the others SoC GPIOs. */
typedef struct {
    const char *id;                 /* short, stable: "tdp4" */
    const char *name;               /* LS_BOARD_NAME: BLE name, Flipper id */
    const char *platform;           /* "esp32p4"; a Linux port says "linux" */
    uint8_t  flash_mb;
    uint32_t app_slot_bytes;        /* one OTA slot, 0 when the build has none */

    uint32_t radios;                /* LS_RADIO_*: everything the image can drive */
    struct {                        /* the LoRa socket */
        int8_t sclk, mosi, miso, cs, busy;
        int8_t rst_gpio, irq_gpio;  /* a header module */
        int8_t rst_xl, irq_xl;      /* the T-Display socket */
    } lora;

    const ls_panel_desc_t *const *panels;   /* what may be fitted, probe order */
    uint8_t  n_panels;
    uint8_t  panel_forced;          /* ls_panel_kind_t, 0: read the ID at boot */

    struct { int8_t uart, rx, tx, wake_xl; uint32_t baud; } gps;
    struct {
        int8_t  expander_addr;      /* XL9535 on the primary bus, -1 none */
        int8_t  i2c_sda, i2c_scl, i2c2_sda, i2c2_scl;
        int8_t  gauge_addr, rtc_addr;
        int8_t  vbus_gpio;          /* USB host power switch */
    } power;
    struct {
        int8_t  codec_addr;         /* ES8311, -1 none */
        int8_t  mclk, bck, ws, dout, din;
        int8_t  pa_gpio, pa_xl;
    } audio;
    struct { int8_t clk, cmd, d0, d1, d2, d3, power_xl; uint16_t max_khz; } sd;
    struct { uint8_t kind; int8_t reset_gpio, enable_xl; } coproc;

    /* Updates. One folder for the family; `ota_legacy` says whether the
       hardware's old per-edition folder is read while that one is empty. */
    const char *ota_dir;            /* "/tdp4u/" */
    const char *ota_board;          /* "T-Display-P4-U" */
    bool     ota_legacy;
} ls_board_profile_t;

/* This image's board. Never NULL. */
const ls_board_profile_t *ls_profile(void);

/* A panel descriptor of this board by kind, NULL when it has none. */
const ls_panel_desc_t *ls_profile_panel_by_kind(int kind);

/* ------------------------------------------------- what the boot found */

/* The panel that answered, NULL before one has (and on a headless board). */
const ls_panel_desc_t *ls_profile_panel(void);
void ls_profile_note_panel(const ls_panel_desc_t *panel);

/* The LoRa part that answered: 0 none, LS_RADIO_SX1262 or LS_RADIO_LR2021. */
uint32_t ls_profile_radio(void);
void ls_profile_note_radio(uint32_t radio);

/* True when the board runs as LakeShark LR: an LR2021 answered. The brand,
   the version line and the old update folder follow it. */
bool ls_profile_is_lr(void);

/* ------------------------------------------------------- owner choices */

typedef enum { LS_PREF_AUTO = 0, LS_PREF_ON = 1, LS_PREF_OFF = 2 } ls_pref_t;

typedef struct {
    uint8_t panel;      /* ls_panel_kind_t to force, 0 auto */
    uint8_t panel_seen; /* the kind that answered last boot: probed first */
    uint8_t usb;        /* ls_pref_t: the USB host port */
    uint8_t net;        /* ls_pref_t: network-edition capture buffers */
} ls_profile_prefs_t;

/* Probe order for the panels. A build that fixes the panel leaves one
   entry. Otherwise the owner's choice goes first, else the kind seen last
   boot, then the rest in the profile's order: a wrong choice is a missed ID
   read, not a dark screen. Returns how many were written. */
int ls_profile_panel_order(const ls_board_profile_t *b, int build_forced, int owner_forced,
                           int last_seen, const ls_panel_desc_t **out, int cap);

/* Whether the USB host starts. Auto keeps it off on an LR2021 board, where
   the stack's ~34 KB of internal RAM goes to the LR2021's I/Q path instead,
   and on for every other board with the port. */
bool ls_profile_usb_wanted(const ls_board_profile_t *b, int pref, uint32_t radio);

/* The pre-universal edition this hardware belongs to: its update folder and
   manifest board name. NET wins over an SX radio (it was an SX AMOLED build),
   the TFT panel over both (its image carries both radios). */
void ls_profile_legacy_edition(int panel_kind, uint32_t radio, bool net,
                               const char **dir, const char **board);

/* The old edition folder the universal image may read for updates this boot,
   and its manifest board name; NULL when it may not: the panel, or on the
   AMOLED the radio, did not answer (unknown would name the SX AMOLED image),
   or what answered picks another folder than the one remembered from before
   this boot. That folder is remembered for the next boot, which may then. */
const char *ls_profile_legacy_dir(const char **board);

/* Sizes that were build options of the network edition. */
typedef struct { uint32_t capture_slots; uint16_t promisc_snap, promisc_per_s; } ls_profile_net_t;
void ls_profile_net_sizes(bool net, ls_profile_net_t *out);

/* Device side. The prefs are read once (ls_profile_boot, after NVS is up)
   and written through the NVS worker. */
void ls_profile_boot(void);
const ls_profile_prefs_t *ls_profile_prefs(void);
bool ls_profile_usb_host(void);     /* decided when the backend starts */
void ls_profile_decide_usb(void);
bool ls_profile_net(void);
int  ls_profile_set_pref(const char *key, int value);   /* "panel" "usb" "net"; 0 ok */
void ls_profile_remember_panel(int kind);
/* One line per fact, for the `board` command, DIAG and reports. */
int  ls_profile_describe(char *out, size_t cap);
int  ls_profile_command(int argc, char **argv);

#ifdef __cplusplus
}
#endif
