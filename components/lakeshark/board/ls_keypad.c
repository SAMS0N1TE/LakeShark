/* See ls_keypad.h for why this bus is bit-banged. */
#include "ls_keypad.h"
#include "ls_keymap.h"

#include "ls_board.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include <stdio.h>
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ls_keypad";

#if defined(LS_BOARD_KEYBOARD_I2C_SDA_GPIO) && \
    defined(LS_BOARD_KEYBOARD_I2C_SCL_GPIO) && \
    defined(LS_BOARD_KEYBOARD_I2C_ADDR)

#define SDA_PIN  ((gpio_num_t)LS_BOARD_KEYBOARD_I2C_SDA_GPIO)
#define SCL_PIN  ((gpio_num_t)LS_BOARD_KEYBOARD_I2C_SCL_GPIO)
#define ADDR     LS_BOARD_KEYBOARD_I2C_ADDR

/* TCA8418 registers, datasheet table 6. */
#define REG_CFG          0x01
#define REG_INT_STAT     0x02
#define REG_KEY_LCK_EC   0x03
#define REG_KEY_EVENT_A  0x04
#define REG_KP_GPIO1     0x1D   /* rows 0..7   */
#define REG_KP_GPIO2     0x1E   /* cols 0..7   */
#define REG_KP_GPIO3     0x1F   /* cols 8..9   */

#define CFG_KE_IEN       0x01   /* key events raise INT */
#define INT_STAT_K_INT   0x01

/* A half period. 5 us is a hair under 100 kHz once the pin's own rise time is
   counted, and the part is specified to 400. Slow is free here: the driver
   only talks after an interrupt, a few bytes at a time. */
#define HALF_US 5

static bool s_present;

static bool    s_pins_ready;
static int64_t s_probe_at;
#define PROBE_PERIOD_US 500000

#define REPEAT_DELAY_US   400000
#define REPEAT_RATE_US     45000
static bool     s_held;
static uint8_t  s_held_row, s_held_col;
static int64_t  s_repeat_at;

static inline void scl_high(void) { gpio_set_level(SCL_PIN, 1); esp_rom_delay_us(HALF_US); }
static inline void scl_low(void)  { gpio_set_level(SCL_PIN, 0); esp_rom_delay_us(HALF_US); }
static inline void sda_set(int v) { gpio_set_level(SDA_PIN, v); esp_rom_delay_us(1); }
static inline int  sda_get(void)  { return gpio_get_level(SDA_PIN); }

/* Both lines are open drain with pull-ups: driving a 1 releases the line and
   lets the pull-up raise it, which is what makes a shared bus work at all. */
static void bus_init_pins(void)
{
    s_pins_ready = true;
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << SDA_PIN) | (1ULL << SCL_PIN),
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    gpio_set_level(SDA_PIN, 1);
    gpio_set_level(SCL_PIN, 1);
}

static void bus_start(void)
{
    sda_set(1); scl_high();
    sda_set(0); esp_rom_delay_us(HALF_US);
    scl_low();
}

static void bus_stop(void)
{
    sda_set(0); scl_high();
    sda_set(1); esp_rom_delay_us(HALF_US);
}

/* Returns true when the slave pulled SDA down for the ACK bit. */
static bool bus_write_byte(uint8_t value)
{
    for (int bit = 7; bit >= 0; bit--) {
        sda_set((value >> bit) & 1);
        scl_high();
        scl_low();
    }
    sda_set(1);              /* release for the acknowledge */
    scl_high();
    bool ack = sda_get() == 0;
    scl_low();
    return ack;
}

static uint8_t bus_read_byte(bool ack)
{
    uint8_t value = 0;
    sda_set(1);
    for (int bit = 7; bit >= 0; bit--) {
        scl_high();
        value = (uint8_t)((value << 1) | (sda_get() ? 1 : 0));
        scl_low();
    }
    sda_set(ack ? 0 : 1);
    scl_high();
    scl_low();
    sda_set(1);
    return value;
}

static bool reg_write(uint8_t reg, uint8_t value)
{
    bus_start();
    bool ok = bus_write_byte((uint8_t)(ADDR << 1)) &&
              bus_write_byte(reg) &&
              bus_write_byte(value);
    bus_stop();
    return ok;
}

static bool reg_read(uint8_t reg, uint8_t *out)
{
    bus_start();
    if (!bus_write_byte((uint8_t)(ADDR << 1)) || !bus_write_byte(reg)) {
        bus_stop();
        return false;
    }
    bus_start();                                  /* repeated start */
    if (!bus_write_byte((uint8_t)((ADDR << 1) | 1))) {
        bus_stop();
        return false;
    }
    *out = bus_read_byte(false);                  /* NACK ends the read */
    bus_stop();
    return true;
}

static bool probe(void)
{
    bus_start();
    bool ack = bus_write_byte((uint8_t)(ADDR << 1));
    bus_stop();
    return ack;
}

esp_err_t ls_keypad_start(void)
{
    /* Idempotent: this is now called from board bring-up so the Flipper
       link can ask whether the keyboard owns the 2x8 header, and again from
       the UI. A second probe would be harmless but pointless. */
    if (s_present) return ESP_OK;
    bus_init_pins();
    if (!probe()) {
        s_present = false;
        ESP_LOGI(TAG, "no keypad at 0x%02x on SDA %d / SCL %d",
                 ADDR, (int)SDA_PIN, (int)SCL_PIN);
        return ESP_ERR_NOT_FOUND;
    }

    const uint8_t rows = (uint8_t)((1u << LS_BOARD_KEYPAD_ROWS) - 1u);
    reg_write(REG_KP_GPIO1, rows);
    reg_write(REG_KP_GPIO2, 0xFF);
    reg_write(REG_KP_GPIO3,
              (uint8_t)(LS_BOARD_KEYPAD_COLS > 8
                            ? (1u << (LS_BOARD_KEYPAD_COLS - 8)) - 1u : 0u));

    uint8_t count = 0;
    if (reg_read(REG_KEY_LCK_EC, &count)) {
        for (int i = 0; i < (count & 0x0F); i++) {
            uint8_t discard;
            reg_read(REG_KEY_EVENT_A, &discard);
        }
    }
    reg_write(REG_INT_STAT, 0x0F);
    reg_write(REG_CFG, CFG_KE_IEN);
    s_held = false;

    s_present = true;
    ESP_LOGI(TAG, "TCA8418 up: %dx%d matrix, SDA %d / SCL %d",
             LS_BOARD_KEYPAD_COLS, LS_BOARD_KEYPAD_ROWS,
             (int)SDA_PIN, (int)SCL_PIN);
    return ESP_OK;
}

bool ls_keypad_present(void) { return s_present; }

void ls_keypad_tick(void)
{
    const int64_t now = esp_timer_get_time();
    if (now < s_probe_at) return;
    s_probe_at = now + PROBE_PERIOD_US;

    if (!s_pins_ready) bus_init_pins();

    /* One address byte and the ACK that follows it. On a 100 kHz bit-banged
       bus that is about ninety microseconds, twice a second. */
    const bool ack = probe();
    if (ack == s_present) return;

    if (!ack) {
        s_present = false;
        s_held = false;
        ESP_LOGI(TAG, "keypad detached");
        return;
    }

    /* Absent to present is not a flag change: the part has just come out of
       reset with an unclaimed matrix, no interrupt configuration and a queue
       that may hold the events from being plugged in. It needs the whole
       bring-up, so clear the flag start uses to decide it has nothing to do
       and let start do it. */
    s_present = false;
    ls_keypad_start();
}

bool ls_keypad_read(ls_keypad_event_t *out)
{
    if (!s_present || !out) return false;

    uint8_t count = 0;
    if (!reg_read(REG_KEY_LCK_EC, &count)) return false;
    if ((count & 0x0F) == 0) {
        /* Nothing new from the part. If a key is still down and its moment has
           come, hand back a repeat that looks exactly like a fresh press. */
        int64_t now = esp_timer_get_time();
        if (s_held && now >= s_repeat_at) {
            s_repeat_at = now + REPEAT_RATE_US;
            out->row = s_held_row;
            out->col = s_held_col;
            out->pressed = true;
            return true;
        }
        return false;
    }

    uint8_t event = 0;
    if (!reg_read(REG_KEY_EVENT_A, &event) || (event & 0x7F) == 0) return false;

    /* Bit 7 is press or release; the low seven are a 1-based key number that
       counts along a row of ten regardless of how many columns are wired. */
    uint8_t key = (uint8_t)((event & 0x7F) - 1);
    out->pressed = (event & 0x80) != 0;
    out->row = (uint8_t)(key / 10);
    out->col = (uint8_t)(key % 10);

    if (out->pressed) {
        /* Only arm the repeat for keys where a second event means more of the
           same thing. Repeating a toggle is not a faster toggle, it is a
           coin flip: CAPS flips its flag on every press, so holding it used
           to leave the flag wherever the release landed. */
        const ls_keymap_entry_t *k = ls_keymap_lookup(out->row, out->col);
        if (ls_keymap_repeats(k)) {
            s_held = true;
            s_held_row = out->row;
            s_held_col = out->col;
            s_repeat_at = esp_timer_get_time() + REPEAT_DELAY_US;
        } else {
            s_held = false;
        }
    } else if (s_held && out->row == s_held_row && out->col == s_held_col) {
        s_held = false;
    }

    reg_write(REG_INT_STAT, INT_STAT_K_INT);
    return true;
}

static bool     s_bl_on;
static uint32_t s_bl_freq = 20000;
/* The duty resolution is not a constant, because the frequency and the resolution trade against each other. */

static int      s_bl_res  = 10;       /* duty resolution bits in use */
static int      s_bl_pct  = 90;

esp_err_t ls_keypad_backlight(bool on)
{
#if defined(LS_BOARD_KEYPAD_BL_GPIO) && (LS_BOARD_KEYPAD_BL_GPIO >= 0)
    /* PWM at 20 kHz, not a static level. */

    static bool configured;
    if (!configured) {
        esp_err_t err = ESP_FAIL;
        /* Widest resolution the wanted frequency allows. Ten bits is
           a thousand steps of brightness, far more than anyone needs, so
           giving bits back to reach a frequency costs nothing real. */
        for (int res = 10; res >= 6 && err != ESP_OK; res--) {
            ledc_timer_config_t timer = {
                .speed_mode      = LEDC_LOW_SPEED_MODE,
                .duty_resolution = (ledc_timer_bit_t)res,
                .timer_num       = LEDC_TIMER_1,
                .freq_hz         = s_bl_freq,
                .clk_cfg         = LEDC_AUTO_CLK,
            };
            err = ledc_timer_config(&timer);
            if (err == ESP_OK) s_bl_res = res;
        }
        if (err != ESP_OK) return err;
        ledc_channel_config_t channel = {
            .gpio_num   = LS_BOARD_KEYPAD_BL_GPIO,
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel    = LEDC_CHANNEL_1,
            .timer_sel  = LEDC_TIMER_1,
            .duty       = 0,
            .hpoint     = 0,
        };
        err = ledc_channel_config(&channel);
        if (err != ESP_OK) return err;
        configured = true;
    }
    /* Not 100% so there is always an edge: at full duty the pin never
       toggles and the converter is back to free-running. */
    const uint32_t full = (1u << s_bl_res) - 1u;
    uint32_t duty = (uint32_t)((uint64_t)full * (uint32_t)s_bl_pct / 100u);
    if (duty >= full && full) duty = full - 1;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, on ? duty : 0u);
    s_bl_on = on;
    return ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
#else
    (void)on;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t ls_keypad_backlight_tune(uint32_t freq_hz, int duty_1024)
{
#if defined(LS_BOARD_KEYPAD_BL_GPIO) && (LS_BOARD_KEYPAD_BL_GPIO >= 0)
    if (duty_1024 < 0)    duty_1024 = 0;
    if (duty_1024 > 1023) duty_1024 = 1023;

    if (freq_hz < 100)     freq_hz = 100;
    if (freq_hz > 1000000) freq_hz = 1000000;

    s_bl_pct = duty_1024 * 100 / 1023;
    s_bl_freq = freq_hz;

    const esp_log_level_t was = esp_log_level_get("ledc");
    esp_log_level_set("ledc", ESP_LOG_NONE);

    esp_err_t err = ESP_FAIL;
    for (int res = 10; res >= 6 && err != ESP_OK; res--) {
        ledc_timer_config_t timer = {
            .speed_mode      = LEDC_LOW_SPEED_MODE,
            .duty_resolution = (ledc_timer_bit_t)res,
            .timer_num       = LEDC_TIMER_1,
            .freq_hz         = s_bl_freq,
            .clk_cfg         = LEDC_AUTO_CLK,
        };
        err = ledc_timer_config(&timer);
        if (err == ESP_OK) s_bl_res = res;
    }

    esp_log_level_set("ledc", was);
    if (err != ESP_OK) {
        /* Nothing fitted, which IS worth saying. */
        ESP_LOGW(TAG, "backlight: %lu Hz is not reachable at any duty "
                      "resolution from 10 bits down to 6",
                 (unsigned long)s_bl_freq);
        return err;
    }

    return ls_keypad_backlight(s_bl_on);
#else
    (void)freq_hz; (void)duty_1024;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

uint32_t ls_keypad_backlight_freq(void)
{
#if defined(LS_BOARD_KEYPAD_BL_GPIO) && (LS_BOARD_KEYPAD_BL_GPIO >= 0)
    /* What LEDC actually settled on, not what it was asked for. */
    return ledc_get_freq(LEDC_LOW_SPEED_MODE, LEDC_TIMER_1);
#else
    return 0;
#endif
}

int ls_keypad_backlight_duty(void)
{
#if defined(LS_BOARD_KEYPAD_BL_GPIO) && (LS_BOARD_KEYPAD_BL_GPIO >= 0)
    return s_bl_on ? s_bl_pct : 0;
#else
    return 0;
#endif
}

void ls_keypad_diagnostics(void)
{
    bus_init_pins();
    bool ack = probe();
    printf("keypad addr=0x%02x sda=%d scl=%d ack=%s present=%s matrix=%dx%d\n",
           ADDR, (int)SDA_PIN, (int)SCL_PIN, ack ? "yes" : "no",
           s_present ? "yes" : "no",
           LS_BOARD_KEYPAD_COLS, LS_BOARD_KEYPAD_ROWS);
    if (!ack) {
        /*"0x34 did not answer" cannot tell a missing keyboard from a
           wrong pin or a broken bit-bang. Sweep the bus: the keyboard also
           carries an XL9555 at 0x20, so anything at all replying proves the
           wires and this driver, and silence proves the lead is out. */
        printf("keypad: 0x34 silent, scanning bus...\n");
        int found = 0;
        for (uint8_t a = 0x08; a < 0x78; a++) {
            bus_start();
            bool hit = bus_write_byte((uint8_t)(a << 1));
            bus_stop();
            if (hit) { printf("  device at 0x%02x\n", a); found++; }
        }
        if (found)
            printf("keypad: bus works, %d device(s), but no TCA8418 at 0x34.\n"
                   "        Pins and driver are fine; the part is not there.\n", found);
        else
            printf("keypad: nothing on the bus at all. The 1x4 lead is out, or\n"
                   "        it is on the wrong connector. Keyboard I2C is the\n"
                   "        pair carrying IO46/IO45; the other 1x4 is IO48/IO47.\n"
                   "        The matrix runs on host 3V3, so an empty battery\n"
                   "        bay is not the reason.\n");
        return;
    }
    uint8_t cfg = 0, ec = 0;
    reg_read(REG_CFG, &cfg);
    reg_read(REG_KEY_LCK_EC, &ec);
    printf("keypad cfg=0x%02x queued=%u\n", (unsigned)cfg, (unsigned)(ec & 0x0F));
}

#else  /* board declares no keypad */

esp_err_t ls_keypad_start(void) { return ESP_ERR_NOT_SUPPORTED; }
bool ls_keypad_present(void) { return false; }
void ls_keypad_tick(void) { }
bool ls_keypad_read(ls_keypad_event_t *out) { (void)out; return false; }
esp_err_t ls_keypad_backlight(bool on) { (void)on; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t ls_keypad_backlight_tune(uint32_t f, int d)
{ (void)f; (void)d; return ESP_ERR_NOT_SUPPORTED; }
uint32_t ls_keypad_backlight_freq(void) { return 0; }
int ls_keypad_backlight_duty(void) { return 0; }
void ls_keypad_diagnostics(void) { printf("keypad: board declares none\n"); }

#endif
