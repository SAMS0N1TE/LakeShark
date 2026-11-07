/* See ls_pins.h. */
#include "ls_pins.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ls_board.h"
#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "nvs.h"
#include "ls_nvs_safe.h"
#endif

#define GPIO_MAX 54
#define NVS_NS   "ls_pins"

#ifndef LS_BOARD_LORA_RST_GPIO
#define LS_BOARD_LORA_RST_GPIO LS_PIN_NONE
#endif
#ifndef LS_BOARD_LORA_IRQ_GPIO
#define LS_BOARD_LORA_IRQ_GPIO LS_PIN_NONE
#endif

static const ls_pin_info_t INFO[LS_PIN_COUNT] = {
    [LS_PIN_GPS_RX]    = { "gps.rx",    "GPS",  "RX  (module TX)", false },
    [LS_PIN_GPS_TX]    = { "gps.tx",    "GPS",  "TX  (module RX)", true  },
    [LS_PIN_SPI_SCLK]  = { "lora.sclk", "LORA", "SCLK",            false },
    [LS_PIN_SPI_MOSI]  = { "lora.mosi", "LORA", "MOSI",            false },
    [LS_PIN_SPI_MISO]  = { "lora.miso", "LORA", "MISO",            false },
    [LS_PIN_LORA_CS]   = { "lora.cs",   "LORA", "NSS / CS",        false },
    [LS_PIN_LORA_BUSY] = { "lora.busy", "LORA", "BUSY",            false },
    [LS_PIN_LORA_RST]  = { "lora.rst",  "LORA", "RESET",           true  },
    [LS_PIN_LORA_IRQ]  = { "lora.irq",  "LORA", "IRQ  (DIO)",      true  },
    [LS_PIN_IMU_SDA]   = { "imu.sda",   "IMU",  "SDA",             false },
    [LS_PIN_IMU_SCL]   = { "imu.scl",   "IMU",  "SCL",             false },
    [LS_PIN_LINK_RX]   = { "link.rx",   "LINK", "RX  (Flipper TX)", false },
    [LS_PIN_LINK_TX]   = { "link.tx",   "LINK", "TX  (Flipper RX)", false },
};

static int board_default(ls_pin_id_t id)
{
    switch (id) {
#ifdef LS_BOARD_GPS_RX_GPIO
    case LS_PIN_GPS_RX:    return LS_BOARD_GPS_RX_GPIO;
    case LS_PIN_GPS_TX:    return LS_BOARD_GPS_TX_GPIO;
#endif
#ifdef LS_BOARD_SPI_SCLK_GPIO
    case LS_PIN_SPI_SCLK:  return LS_BOARD_SPI_SCLK_GPIO;
    case LS_PIN_SPI_MOSI:  return LS_BOARD_SPI_MOSI_GPIO;
    case LS_PIN_SPI_MISO:  return LS_BOARD_SPI_MISO_GPIO;
#endif
#ifdef LS_BOARD_LORA_CS_GPIO
    case LS_PIN_LORA_CS:   return LS_BOARD_LORA_CS_GPIO;
    case LS_PIN_LORA_BUSY: return LS_BOARD_LORA_BUSY_GPIO;
#endif
    case LS_PIN_LORA_RST:  return LS_BOARD_LORA_RST_GPIO;
    case LS_PIN_LORA_IRQ:  return LS_BOARD_LORA_IRQ_GPIO;
#if defined(LS_BOARD_IMU_SDA_GPIO)
    case LS_PIN_IMU_SDA:   return LS_BOARD_IMU_SDA_GPIO;
    case LS_PIN_IMU_SCL:   return LS_BOARD_IMU_SCL_GPIO;
#elif defined(LS_BOARD_I2C2_SDA_GPIO)
    case LS_PIN_IMU_SDA:   return LS_BOARD_I2C2_SDA_GPIO;
    case LS_PIN_IMU_SCL:   return LS_BOARD_I2C2_SCL_GPIO;
#endif
#ifdef LS_BOARD_LINK_RX_GPIO
    case LS_PIN_LINK_RX:   return LS_BOARD_LINK_RX_GPIO;
    case LS_PIN_LINK_TX:   return LS_BOARD_LINK_TX_GPIO;
#endif
    default:               return LS_PIN_NONE;
    }
}

static uint8_t default_imu_addr(void)
{
#ifdef LS_BOARD_IMU_I2C_ADDR
    return LS_BOARD_IMU_I2C_ADDR;
#else
    return 0x68;
#endif
}

/* Pins the board drives itself: never an add-on's. */
static const int OWN[] = {
#ifdef LS_BOARD_I2S_MCLK_GPIO
    LS_BOARD_I2S_MCLK_GPIO, LS_BOARD_I2S_BCK_GPIO, LS_BOARD_I2S_WS_GPIO,
    LS_BOARD_I2S_DOUT_GPIO, LS_BOARD_I2S_DIN_GPIO,
#endif
#ifdef LS_BOARD_PA_EN_GPIO
    LS_BOARD_PA_EN_GPIO,
#endif
#ifdef LS_BOARD_C6_EN_GPIO
    LS_BOARD_C6_EN_GPIO,
#endif
#ifdef LS_BOARD_SDIO_CLK_GPIO
    LS_BOARD_SDIO_CLK_GPIO, LS_BOARD_SDIO_CMD_GPIO, LS_BOARD_SDIO_D0_GPIO,
    LS_BOARD_SDIO_D1_GPIO, LS_BOARD_SDIO_D2_GPIO, LS_BOARD_SDIO_D3_GPIO,
#endif
#ifdef CONFIG_ESP_HOSTED_PRIV_SDIO_PIN_CLK_SLOT_1
    CONFIG_ESP_HOSTED_PRIV_SDIO_PIN_CLK_SLOT_1, CONFIG_ESP_HOSTED_PRIV_SDIO_PIN_CMD_SLOT_1,
    CONFIG_ESP_HOSTED_PRIV_SDIO_PIN_D0_SLOT_1,
#endif
#ifdef CONFIG_ESP_HOSTED_PRIV_SDIO_PIN_D1_4BIT_BUS_SLOT_1
    CONFIG_ESP_HOSTED_PRIV_SDIO_PIN_D1_4BIT_BUS_SLOT_1, CONFIG_ESP_HOSTED_PRIV_SDIO_PIN_D2_4BIT_BUS_SLOT_1,
    CONFIG_ESP_HOSTED_PRIV_SDIO_PIN_D3_4BIT_BUS_SLOT_1,
#endif
#ifdef LS_BOARD_SD_PWR_GPIO
    LS_BOARD_SD_PWR_GPIO,
#endif
#ifdef LS_BOARD_LCD_BL_GPIO
    LS_BOARD_LCD_BL_GPIO,
#endif
    -1,
};

static int8_t  s_now[LS_PIN_COUNT];
static int8_t  s_next[LS_PIN_COUNT];
static uint8_t s_imu_now, s_imu_next;
static bool    s_init, s_loaded;

static void init_defaults(void)
{
    if (s_init) return;
    for (int i = 0; i < LS_PIN_COUNT; i++)
        s_now[i] = s_next[i] = (int8_t)board_default((ls_pin_id_t)i);
    s_imu_now = s_imu_next = default_imu_addr();
    s_init = true;
}

bool ls_pins_runtime(void)
{
#if defined(LS_BOARD_PINS_RUNTIME) && LS_BOARD_PINS_RUNTIME
    return true;
#else
    return false;
#endif
}

#ifdef ESP_PLATFORM
static esp_err_t load_fn(void *ctx)
{
    (void)ctx;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;        /* nothing saved */
    if (err != ESP_OK) return err;
    for (int i = 0; i < LS_PIN_COUNT; i++) {
        int8_t v;
        if (nvs_get_i8(h, INFO[i].key, &v) == ESP_OK && v >= LS_PIN_NONE && v <= GPIO_MAX)
            s_next[i] = v;
    }
    uint8_t a;
    if (nvs_get_u8(h, "imu.addr", &a) == ESP_OK && a > 0x07 && a < 0x78) s_imu_next = a;
    nvs_close(h);
    return ESP_OK;
}

typedef struct { const char *key; int8_t v; bool erase_all; bool u8; } write_t;

static esp_err_t write_fn(void *ctx)
{
    const write_t *w = ctx;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    if (w->erase_all) err = nvs_erase_all(h);
    else if (w->u8) err = nvs_set_u8(h, w->key, (uint8_t)w->v);
    else err = nvs_set_i8(h, w->key, w->v);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}
#endif

void ls_pins_load(void)
{
    init_defaults();
    if (s_loaded || !ls_pins_runtime()) { s_loaded = true; return; }
#ifdef ESP_PLATFORM
    const esp_err_t err = ls_nvs_run(load_fn, NULL, 0);
    if (err == ESP_ERR_NVS_NOT_INITIALIZED) return;          /* try again later */
    if (err != ESP_OK) ESP_LOGW("ls_pins", "saved pins unreadable (%s): defaults", esp_err_to_name(err));
#endif
    memcpy(s_now, s_next, sizeof(s_now));
    s_imu_now = s_imu_next;
    s_loaded = true;
}

bool ls_pins_ready(void) { if (!s_loaded) ls_pins_load(); return s_loaded; }

int ls_pin(ls_pin_id_t id)
{
    if (id < 0 || id >= LS_PIN_COUNT) return LS_PIN_NONE;
    if (!s_loaded) ls_pins_load();
    return s_now[id];
}

int ls_pin_next(ls_pin_id_t id)
{
    if (id < 0 || id >= LS_PIN_COUNT) return LS_PIN_NONE;
    if (!s_loaded) ls_pins_load();
    return s_next[id];
}

int ls_pin_default(ls_pin_id_t id)
{
    return id >= 0 && id < LS_PIN_COUNT ? board_default(id) : LS_PIN_NONE;
}

const ls_pin_info_t *ls_pin_info(ls_pin_id_t id)
{
    return id >= 0 && id < LS_PIN_COUNT ? &INFO[id] : NULL;
}

int ls_pin_find(const char *key)
{
    if (!key) return -1;
    for (int i = 0; i < LS_PIN_COUNT; i++) if (!strcmp(INFO[i].key, key)) return i;
    return -1;
}

uint8_t ls_pins_imu_addr(void)      { if (!s_loaded) ls_pins_load(); return s_imu_now; }
uint8_t ls_pins_imu_addr_next(void) { if (!s_loaded) ls_pins_load(); return s_imu_next; }

static bool is_board_bus(int g)
{
#if defined(LS_BOARD_I2C_SDA_GPIO)
    return g == LS_BOARD_I2C_SDA_GPIO || g == LS_BOARD_I2C_SCL_GPIO;
#else
    (void)g; return false;
#endif
}

bool ls_pin_check(ls_pin_id_t id, int g, char *why, size_t n)
{
    if (!why || !n) { static char sink[4]; why = sink; n = sizeof(sink); }
    if (id < 0 || id >= LS_PIN_COUNT) { snprintf(why, n, "no such pin"); return false; }
    if (!ls_pins_runtime()) { snprintf(why, n, "this board's pins are fixed"); return false; }
    if (!s_loaded) ls_pins_load();
    if (g == LS_PIN_NONE) {
        if (!INFO[id].optional) { snprintf(why, n, "%s is needed", INFO[id].key); return false; }
        return true;
    }
    if (g < 0 || g > GPIO_MAX) { snprintf(why, n, "GPIO 0-%d", GPIO_MAX); return false; }
    for (int i = 0; OWN[i] >= 0; i++)
        if (OWN[i] == g) { snprintf(why, n, "GPIO%d is the board's own", g); return false; }
#if defined(LS_BOARD_VBUS_EN_GPIO)
    if (LS_BOARD_VBUS_EN_GPIO >= 0 && g == LS_BOARD_VBUS_EN_GPIO) {
        snprintf(why, n, "GPIO%d is the USB power switch", g);
        return false;
    }
#endif
    const bool imu_line = id == LS_PIN_IMU_SDA || id == LS_PIN_IMU_SCL;
    if (is_board_bus(g) && !imu_line) { snprintf(why, n, "GPIO%d is the board I2C bus", g); return false; }
    for (int i = 0; i < LS_PIN_COUNT; i++) {
        if (i == (int)id || s_next[i] != g) continue;
        /* the IMU may sit on the board bus pair, nothing else shares */
        snprintf(why, n, "GPIO%d is %s", g, INFO[i].key);
        return false;
    }
    return true;
}

static bool save(const char *key, int8_t v, bool u8, bool erase_all)
{
#ifdef ESP_PLATFORM
    write_t w = { key, v, erase_all, u8 };
    const esp_err_t err = ls_nvs_run(write_fn, &w, 0);
    if (err != ESP_OK) ESP_LOGW("ls_pins", "save %s: %s", key ? key : "reset", esp_err_to_name(err));
    return err == ESP_OK;
#else
    (void)key; (void)v; (void)u8; (void)erase_all;
    return true;
#endif
}

bool ls_pin_set(ls_pin_id_t id, int g, char *why, size_t n)
{
    if (!ls_pin_check(id, g, why, n)) return false;
    if (!save(INFO[id].key, (int8_t)g, false, false)) {
        if (why && n) snprintf(why, n, "could not save");
        return false;
    }
    s_next[id] = (int8_t)g;
    return true;
}

/* The whole table at once, so two parts can swap pins. */
bool ls_pins_apply(const int *table, uint8_t imu_addr, char *why, size_t n)
{
    if (!ls_pins_runtime()) { snprintf(why, n, "this board's pins are fixed"); return false; }
    if (!s_loaded) ls_pins_load();
    int8_t keep[LS_PIN_COUNT];
    memcpy(keep, s_next, sizeof(keep));
    /* Check each against the new table, not the saved one. */
    for (int i = 0; i < LS_PIN_COUNT; i++) s_next[i] = (int8_t)table[i];
    for (int i = 0; i < LS_PIN_COUNT; i++) {
        if (!ls_pin_check((ls_pin_id_t)i, table[i], why, n)) {
            memcpy(s_next, keep, sizeof(keep));
            return false;
        }
    }
    if (imu_addr < 0x08 || imu_addr > 0x77) {
        memcpy(s_next, keep, sizeof(keep));
        snprintf(why, n, "IMU address 0x08-0x77");
        return false;
    }
    for (int i = 0; i < LS_PIN_COUNT; i++) {
        if (keep[i] == table[i]) continue;
        if (!save(INFO[i].key, (int8_t)table[i], false, false)) {
            snprintf(why, n, "could not save %s", INFO[i].key);
            return false;
        }
    }
    if (imu_addr != s_imu_next) {
        if (!save("imu.addr", (int8_t)imu_addr, true, false)) { snprintf(why, n, "could not save imu.addr"); return false; }
        s_imu_next = imu_addr;
    }
    return true;
}

bool ls_pins_check_in(const int *table, ls_pin_id_t id, char *why, size_t n)
{
    if (id < 0 || id >= LS_PIN_COUNT) return false;
    if (!s_loaded) ls_pins_load();
    int8_t keep[LS_PIN_COUNT];
    memcpy(keep, s_next, sizeof(keep));
    for (int i = 0; i < LS_PIN_COUNT; i++) s_next[i] = (int8_t)table[i];
    const bool ok = ls_pin_check(id, table[id], why, n);
    memcpy(s_next, keep, sizeof(keep));
    return ok;
}

bool ls_pin_board_own(int g)
{
    if (g < 0) return false;
    for (int i = 0; OWN[i] >= 0; i++) if (OWN[i] == g) return true;
#if defined(LS_BOARD_VBUS_EN_GPIO)
    if (LS_BOARD_VBUS_EN_GPIO >= 0 && g == LS_BOARD_VBUS_EN_GPIO) return true;
#endif
    return is_board_bus(g);
}

bool ls_pins_set_imu_addr(uint8_t a)
{
    if (!ls_pins_runtime() || a < 0x08 || a > 0x77) return false;
    if (!s_loaded) ls_pins_load();
    if (!save("imu.addr", (int8_t)a, true, false)) return false;
    s_imu_next = a;
    return true;
}

bool ls_pins_reset(void)
{
    if (!ls_pins_runtime()) return false;
    if (!s_loaded) ls_pins_load();
    if (!save(NULL, 0, false, true)) return false;
    for (int i = 0; i < LS_PIN_COUNT; i++) s_next[i] = (int8_t)board_default((ls_pin_id_t)i);
    s_imu_next = default_imu_addr();
    return true;
}

bool ls_pins_pending(void)
{
    if (!s_loaded) ls_pins_load();
    return memcmp(s_now, s_next, sizeof(s_now)) != 0 || s_imu_now != s_imu_next;
}

/* ---------------------------------------------------------------- console */

static void pin_word(int g, char *out, size_t n)
{
    if (g < 0) snprintf(out, n, "none");
    else snprintf(out, n, "%d", g);
}

static void print_table(void)
{
    printf("%-10s %-5s %-5s %-5s\n", "pin", "now", "next", "board");
    for (int i = 0; i < LS_PIN_COUNT; i++) {
        char a[8], b[8], c[8];
        pin_word(ls_pin((ls_pin_id_t)i), a, sizeof(a));
        pin_word(ls_pin_next((ls_pin_id_t)i), b, sizeof(b));
        pin_word(ls_pin_default((ls_pin_id_t)i), c, sizeof(c));
        printf("%-10s %-5s %-5s %-5s\n", INFO[i].key, a, b, c);
    }
    printf("%-10s 0x%02X  0x%02X  0x%02X\n", "imu.addr", ls_pins_imu_addr(),
           ls_pins_imu_addr_next(), default_imu_addr());
    if (!ls_pins_runtime()) printf("this board's pins are fixed.\n");
    else if (ls_pins_pending()) printf("changes apply at the next boot ('reboot').\n");
}

int ls_pins_cmd(int argc, char **argv)
{
    if (argc < 2) { print_table(); return 0; }
    if (!strcmp(argv[1], "set") && argc == 4) {
        const int id = ls_pin_find(argv[2]);
        if (id < 0) { printf("pin: unknown '%s' (see 'pin')\n", argv[2]); return 1; }
        int g = LS_PIN_NONE;
        if (strcmp(argv[3], "none")) {
            char *end = NULL;
            g = (int)strtol(argv[3], &end, 10);
            if (end == argv[3] || *end) { printf("pin: want a GPIO number or none\n"); return 1; }
        }
        char why[64];
        if (!ls_pin_set((ls_pin_id_t)id, g, why, sizeof(why))) { printf("pin: %s\n", why); return 1; }
        printf("pin: %s = %s at the next boot\n", argv[2], argv[3]);
        return 0;
    }
    if (!strcmp(argv[1], "imu") && argc == 3) {
        const long a = strtol(argv[2], NULL, 0);
        if (!ls_pins_set_imu_addr((uint8_t)a)) { printf("pin: address 0x08-0x77\n"); return 1; }
        printf("pin: imu.addr = 0x%02X at the next boot\n", (unsigned)a);
        return 0;
    }
    if (!strcmp(argv[1], "reset") && argc == 2) {
        if (!ls_pins_reset()) { printf("pin: could not reset\n"); return 1; }
        printf("pin: board defaults at the next boot\n");
        return 0;
    }
    printf("usage: pin | pin set <key> <gpio|none> | pin imu <addr> | pin reset\n");
    return 1;
}
