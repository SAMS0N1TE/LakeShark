/* The LoRa socket: a dispatcher over two backends.

   This file owns what is true of the socket whatever sits in it - the SPI
   device, BUSY, the reset pulse, the DMA packet buffer, and finding out which
   part answered - plus the arithmetic every board has. The commands live in
   ls_lora_sx126x.c (the SX1262, unchanged) and ls_lora_lr20xx.c. See
   ls_lora.h for why BUSY and DIO1 shape all of it. */
#include "ls_lora.h"
#include "ls_lora_priv.h"

#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "ls_board.h"
#include "ls_spi.h"
#include "ls_xl9535.h"

#if LS_HAS_LORA_LR20XX_PROBE
#include "ls_lora_lr20xx.h"
#endif

/* ls_lora_lr20xx.c builds its backend under this condition and no other. */
#if LS_HAS_LORA_LR20XX_PROBE && defined(LS_BOARD_LORA_CS_GPIO) && \
    defined(LS_BOARD_LORA_BUSY_GPIO) && defined(LS_BOARD_XL_RADIO_RST)
#define LR20XX_BACKEND_BUILT 1
#else
#define LR20XX_BACKEND_BUILT 0
#endif

/* ------------------------------------------------------------ ladder -- */

/* The SX126x LoRa bandwidth ladder, once. The sweep plan below is built on it
   and so is the SX126x backend; the LR20xx has its own filter table. */

const ls_lora_bw_rung_t ls_lora_sx126x_bw_ladder[] = {
    {   7810, 0x00 }, {  10420, 0x08 }, {  15630, 0x01 }, {  20830, 0x09 },
    {  31250, 0x02 }, {  41670, 0x0A }, {  62500, 0x03 }, { 125000, 0x04 },
    { 250000, 0x05 }, { 500000, 0x06 },
};
const int ls_lora_sx126x_bw_ladder_n =
    (int)(sizeof(ls_lora_sx126x_bw_ladder) / sizeof(ls_lora_sx126x_bw_ladder[0]));
#define BW_LADDER   ls_lora_sx126x_bw_ladder
#define BW_LADDER_N ls_lora_sx126x_bw_ladder_n

/* -------------------------------------------------------- sweep plan -- */

/* At 500 kHz, which is the case it was measured in and the case it
   stays the figure for. The plan below adds to it as the filter narrows. */
#define SCAN_SETTLE_US   300
#define SCAN_BW_WIDEST   500000u

/* Preserve the measured wideband acquisition interval in filter samples.
   The former additive 8/BW estimate read 41.67kHz before RSSI was ready. */
uint32_t ls_lora_scan_settle_us(uint32_t bw_hz)
{
    if (!bw_hz || bw_hz>SCAN_BW_WIDEST)bw_hz=SCAN_BW_WIDEST;
    return (SCAN_SETTLE_US*SCAN_BW_WIDEST+bw_hz-1)/bw_hz;
}

void ls_lora_scan_plan(uint32_t min_hz, uint32_t max_hz, int n,
                       ls_lora_scan_plan_t *out)
{
    if (!out) return;
    out->bw_hz = SCAN_BW_WIDEST;
    out->looks = 1;
    /* A single bin or an empty band has no spacing to plan from, and gets
       what every sweep got before: the widest filter and one look. */
    if (n > 1 && max_hz > min_hz) {
        const uint64_t span = (uint64_t)(max_hz - min_hz);
        const uint64_t gaps = (uint64_t)(n - 1);

        for (int r = 0; r < BW_LADDER_N; r++) {
            if ((uint64_t)BW_LADDER[r].hz * gaps >= span) {
                out->bw_hz = BW_LADDER[r].hz;
                break;
            }
        }
        const uint64_t covered = (uint64_t)out->bw_hz * gaps;
        if (covered < span) {
            uint64_t looks = (span + covered - 1) / covered;
            if (looks > LS_LORA_SCAN_LOOKS_MAX) looks = LS_LORA_SCAN_LOOKS_MAX;
            out->looks = (int)looks;
        }
    }
    out->settle_us = ls_lora_scan_settle_us(out->bw_hz);
}

uint32_t ls_lora_scan_look_hz(uint32_t min_hz, uint32_t max_hz, int n, int i,
                              int looks, int k)
{
    if (looks <= 1 || n <= 1 || max_hz <= min_hz)
        return ls_lora_scan_bin_hz(min_hz, max_hz, n, i);
    if (i < 0) i = 0;
    if (i >= n) i = n - 1;
    if (k < 0) k = 0;
    if (k >= looks) k = looks - 1;
    /* Bin i owns the slice from half a spacing below its centre to half a spacing above, and look k sits in the middle of the k-th of `looks` equal parts of it: */

    const int64_t span = (int64_t)(max_hz - min_hz);
    const int64_t num  = 2LL * looks * i - looks + 2LL * k + 1;
    const int64_t den  = 2LL * looks * (n - 1);
    int64_t off = (span * num) / den;
    if (off < 0)    off = 0;
    if (off > span) off = span;
    return min_hz + (uint32_t)off;
}

/* Arithmetic only, so every board has it. It sat inside the fitted-radio
   half, and rec_watch_runtime.c calls it on every board: the five boards
   without an SX1262 stopped linking. An LR20xx has a filter table of its own,
   used only while one is bound; every other board, with a part or without,
   gets the SX126x ladder below. */
uint32_t ls_lora_fsk_bw_snap(uint32_t hz)
{
#if LR20XX_BACKEND_BUILT
    if (ls_lora_chip() == LS_LORA_CHIP_LR20XX) return lr20xx_fsk_rx_bw(hz).hz;
#endif
    static const uint32_t RUNG[] = {
        4800,5800,7300,9700,11700,14600,19500,23400,29300,39000,46900,
        58600,78200,93800,117300,156200,187200,234300,312000,373600,467000
    };
    for (size_t i = 0; i < sizeof(RUNG)/sizeof(RUNG[0]); i++)
        if (RUNG[i] >= hz) return RUNG[i];
    return RUNG[sizeof(RUNG)/sizeof(RUNG[0]) - 1];
}

/* See ls_lora.h. Both pure, both used once per bin. They describe the SX126x
   synthesiser step and the evenly spaced bin grid, with or without a part on
   the board, so they stay out of the conditional. */
uint32_t ls_lora_freq_steps(uint32_t hz)
{
    return (uint32_t)(((uint64_t)hz << 25) / 32000000ull);
}

uint32_t ls_lora_scan_bin_hz(uint32_t min_hz, uint32_t max_hz, int n, int i)
{
    if (n <= 0) return min_hz;
    if (i < 0) i = 0;
    if (i >= n) i = n - 1;
    if (n == 1 || max_hz <= min_hz) return min_hz;

    const uint64_t span = (uint64_t)(max_hz - min_hz);
    return min_hz + (uint32_t)((span * (uint64_t)i) / (uint64_t)(n - 1));
}

#if defined(LS_BOARD_LORA_CS_GPIO) && defined(LS_BOARD_LORA_BUSY_GPIO) && \
    defined(LS_BOARD_XL_RADIO_RST)

#define CS_PIN    ((gpio_num_t)LS_BOARD_LORA_CS_GPIO)
#define BUSY_PIN  ((gpio_num_t)LS_BOARD_LORA_BUSY_GPIO)

static const char *TAG = "ls_lora";

static spi_device_handle_t s_dev;
static bool s_present;

/* ---------------------------------------------------------- SPI lock -- */

/* One device handle, several tasks: the console (diagnostics, scans), the
   ADS-B pump (the Mode S session), the mesh. The IDF driver asserts when two
   tasks transmit on one handle at once, and an LR20xx read is two frames with
   a BUSY wait between them that no other frame may land in. This lock is held
   across a whole command sequence, from the dispatcher's public entry points,
   and again by each backend around the commands it issues. Static storage,
   made on first use under a spinlock, so there is no start-up order to get
   wrong and nothing to allocate. */
static StaticSemaphore_t s_lock_buf;
static SemaphoreHandle_t s_lock;
static portMUX_TYPE      s_lock_init = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t      s_lock_owner;     /* written by the holder only */
static unsigned          s_lock_depth;     /* ditto */
static unsigned          s_unguarded;

static SemaphoreHandle_t lock_handle(void)
{
    SemaphoreHandle_t h = __atomic_load_n(&s_lock, __ATOMIC_ACQUIRE);
    if (!h) {
        portENTER_CRITICAL(&s_lock_init);
        h = s_lock;
        if (!h) {
            h = xSemaphoreCreateRecursiveMutexStatic(&s_lock_buf);
            __atomic_store_n(&s_lock, h, __ATOMIC_RELEASE);
        }
        portEXIT_CRITICAL(&s_lock_init);
    }
    return h;
}

bool ls_lora_hw_locked_by_me(void)
{
    return __atomic_load_n(&s_lock_owner, __ATOMIC_ACQUIRE) == xTaskGetCurrentTaskHandle();
}

void ls_lora_hw_lock(void)
{
    SemaphoreHandle_t h = lock_handle();
    if (xSemaphoreTakeRecursive(h, portMAX_DELAY) != pdTRUE) return;
    s_lock_depth++;
    __atomic_store_n(&s_lock_owner, xTaskGetCurrentTaskHandle(), __ATOMIC_RELEASE);
}

void ls_lora_hw_unlock(void)
{
    if (!ls_lora_hw_locked_by_me()) return;      /* not ours to give */
    if (--s_lock_depth == 0)
        __atomic_store_n(&s_lock_owner, (TaskHandle_t)NULL, __ATOMIC_RELEASE);
    xSemaphoreGiveRecursive(lock_handle());
}

unsigned ls_lora_hw_unguarded(void) { return __atomic_load_n(&s_unguarded, __ATOMIC_RELAXED); }

esp_err_t ls_lora_hw_transmit(spi_transaction_t *t)
{
    if (!ls_lora_hw_locked_by_me()) {
        if (__atomic_fetch_add(&s_unguarded, 1, __ATOMIC_RELAXED) == 0)
            ESP_LOGW(TAG, "SPI transaction outside a locked sequence");
    }
    ls_lora_hw_lock();
    const esp_err_t err = spi_device_transmit(s_dev, t);
    ls_lora_hw_unlock();
    return err;
}

/* Run `expr` with the lock held and return its value. */
#define LOCKED_RET(type, expr)                                                \
    do { ls_lora_hw_lock(); type r_ = (expr); ls_lora_hw_unlock(); return r_; } while (0)

/* Until a part has answered, calls go to the SX126x backend exactly as they
   did when that was the only code in this file; a part that identifies as
   something else rebinds, and ls_lora_stop() unbinds. */
static const ls_lora_ops_t *s_ops = &ls_lora_sx126x_ops;
static ls_lora_chip_t s_chip = LS_LORA_CHIP_NONE;

/* SPI wants DMA-capable memory for a transfer this size and DMA cannot reach
   PSRAM, so this is internal RAM and there is no choice about that. 259 bytes
   carries opcode + offset + a full 255-byte payload in one transaction;
   splitting it would save RAM and put a BUSY wait in the middle of a buffer
   write, which is the more expensive mistake. One buffer, not one per
   direction: the driver is single-threaded by contract. */
static uint8_t *s_pkt;

spi_device_handle_t ls_lora_hw_dev(void) { return s_dev; }
uint8_t *ls_lora_hw_pkt(void) { return s_pkt; }
int ls_lora_hw_busy_level(void) { return gpio_get_level(BUSY_PIN); }

/* The part holds BUSY high while it works and ignores anything clocked in
   meanwhile. 1 ms is generous for every command used here; the datasheet's
   worst case is the crystal start after a cold reset, which is handled
   separately below with a longer budget. */
/* Spin briefly, then yield. */
bool ls_lora_hw_wait_not_busy(int timeout_ms)
{
    for (int i = 0; i < 256; i++) {
        if (!gpio_get_level(BUSY_PIN)) return true;
        esp_rom_delay_us(1);
    }
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (gpio_get_level(BUSY_PIN)) {
        if (esp_timer_get_time() > deadline) return false;
        vTaskDelay(1);
    }
    return true;
}

/* The reset pin as the expander reads it back, not as we last wrote it:
   "high", "low", or why it could not be read. */
static const char *rst_readback(void)
{
    bool level = false;
    const esp_err_t err = ls_xl9535_get(LS_BOARD_XL_RADIO_RST, &level);
    if (err != ESP_OK) return esp_err_to_name(err);
    return level ? "high" : "low";
}

static bool ensure_pkt(void)
{
    if (!s_pkt) {
        s_pkt = heap_caps_malloc(259, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (!s_pkt) {
            ESP_LOGE(TAG, "no DMA memory for the packet buffer");
            return false;
        }
    }
    return true;
}

/* How many times the part is reset and looked at before it is called absent,
   and how long each waits after BUSY falls: the first is the datasheet's, the
   rest are for a part that has not finished starting. */
#define START_ATTEMPTS 4
static const unsigned START_SETTLE_MS[START_ATTEMPTS] = { 10, 30, 60, 120 };

/* One NRESET pulse (an expander pin) and the wait for the part to come up.
   BUSY rises as it starts and falls when it is ready; a level read the moment
   NRESET is released can be the low from before it started, so the rise is
   waited for first (a part that never raises it has already finished, as the
   SX126x does) and the fall after it, then `settle_ms` more. The lock is held. */
static esp_err_t reset_part_locked(unsigned settle_ms)
{
    /* LS-1240: reset is an expander pin; when I2C0 fails (LS-1239) the
       part is never reset and the error below blamed MISO. Keep going, but
       report it. */
    const esp_err_t rst_lo = ls_xl9535_out(LS_BOARD_XL_RADIO_RST, false);
    vTaskDelay(pdMS_TO_TICKS(2));
    const esp_err_t rst_hi = ls_xl9535_out(LS_BOARD_XL_RADIO_RST, true);
    if (rst_lo != ESP_OK || rst_hi != ESP_OK) {
        ESP_LOGE(TAG, "reset not pulsed: expander %s, drive low %s, release %s"
                 " - the part is in whatever state the last boot left it",
                 ls_xl9535_ready() ? "up" : "DOWN", esp_err_to_name(rst_lo),
                 esp_err_to_name(rst_hi));
    }

    for (int i = 0; i < 50 && !gpio_get_level(BUSY_PIN); i++) esp_rom_delay_us(100);

    /* Cold start runs the 32 MHz crystal up; the datasheet allows several
       milliseconds and BUSY stays high throughout. */
    if (!ls_lora_hw_wait_not_busy(100)) {
        ESP_LOGE(TAG, "busy never fell after reset - part unpowered or absent"
                 " (expander %s, reset release %s, RST reads %s)",
                 ls_xl9535_ready() ? "up" : "DOWN", esp_err_to_name(rst_hi),
                 rst_readback());
        return ESP_ERR_TIMEOUT;
    }
    if (settle_ms) vTaskDelay(pdMS_TO_TICKS(settle_ms));
    return ESP_OK;
}

/* A reset and the wait for the part to come back, for a backend whose part
   has stopped taking commands. Lock held by the caller or taken here. */
esp_err_t ls_lora_hw_reset(void)
{
    ls_lora_hw_lock();
    const esp_err_t err = reset_part_locked(START_SETTLE_MS[1]);
    ls_lora_hw_unlock();
    return err;
}

/* Which part answered, binding its backend. NOT_FOUND when none did; the
   caller resets and asks again before believing that. `last` says whether
   this is the final look, so that only it logs as an error. */
static esp_err_t identify_locked(bool last)
{
#define IDENT_LOG(...) do { if (last) ESP_LOGE(TAG, __VA_ARGS__); else ESP_LOGW(TAG, __VA_ARGS__); } while (0)

#if LS_HAS_LORA_LR20XX_PROBE
    /* An LR20xx first. On one, the SX126x probe below would read the 16-bit
       status word of a part that rejected its opcodes and call it an SX1262. */
    {
        lr20xx_info_t info;
        if (lr20xx_probe(&info) == ESP_OK) {
            if (!ensure_pkt()) return ESP_ERR_NO_MEM;
            lr20xx_bind(&info);
            s_ops = &ls_lora_lr20xx_ops;
            s_chip = LS_LORA_CHIP_LR20XX;
            s_present = true;
            ESP_LOGI(TAG, "%s up: FW %u.%u, stat 0x%04X (chip mode %u)",
                     ls_lora_chip_name(), (unsigned)info.fw_major,
                     (unsigned)info.fw_minor, (unsigned)info.stat,
                     (unsigned)(info.stat & 7));
            return ESP_OK;
        }
    }
#endif

    uint8_t st = 0;
    const esp_err_t err = ls_lora_sx126x_probe(&st);
    if (err != ESP_OK) return err;
    if (st == 0x00 || st == 0xFF) {
        /* A floating MISO reads as one of these depending on the pull, and
           both mean nothing is driving the bus. Reporting "not found" is more
           useful than reporting a status of zero as though it were real. */
        IDENT_LOG("status 0x%02X - nothing is driving MISO (BUSY %d,"
                 " expander %s, RST reads %s)", st,
                 gpio_get_level(BUSY_PIN), ls_xl9535_ready() ? "up" : "DOWN",
                 rst_readback());
        return ESP_ERR_NOT_FOUND;
    }
    if (st & 0x81) {
        /* Bits 7 and 0 of an SX126x status are reserved and read zero. Set,
           it is something else answering - quite possibly an LR20xx that did
           not identify - and it is not reported as an SX1262. */
        IDENT_LOG("status 0x%02X is not an SX126x status (reserved bits"
                 " set) and no LR20xx answered GetVersion - part not identified"
                 " (BUSY %d, expander %s, RST reads %s)", st,
                 gpio_get_level(BUSY_PIN), ls_xl9535_ready() ? "up" : "DOWN",
                 rst_readback());
        return ESP_ERR_NOT_FOUND;
    }

    if (!ensure_pkt()) return ESP_ERR_NO_MEM;

    s_ops = &ls_lora_sx126x_ops;
    s_chip = LS_LORA_CHIP_SX126X;
    s_present = true;
    ESP_LOGI(TAG, "SX1262 up: status 0x%02X (mode %u, cmd %u)",
             st, (unsigned)((st >> 4) & 7), (unsigned)((st >> 1) & 7));
    return ESP_OK;
}

static esp_err_t start_locked(void)
{
    if (s_present) return ESP_OK;

    esp_err_t err = ls_spi_bus(LS_SPI_RADIO);
    if (err != ESP_OK) return err;

    gpio_config_t busy = {
        .pin_bit_mask = 1ULL << BUSY_PIN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&busy);

    /* Hardware chip select is right for this part: unlike the CC1101 it has
       no handshake that needs CS held across a wait. 2 MHz is well inside
       the 16 MHz limit and leaves the wiring no excuse. */
    /* Register once. ls_lora_stop() clears s_present so the next start
       re-runs the reset and the liveness check, and without this guard it
       would also register a second device every time. The IDF allows three
       per host and four parts share this one, so a stop/start cycle would
       quietly consume the budget the other radios need. */
    if (!s_dev) {
        err = ls_spi_device(LS_SPI_RADIO, LS_BOARD_LORA_CS_GPIO, 0,
                            2 * 1000 * 1000, 1, &s_dev);
        if (err != ESP_OK) return err;
    }

    /* ls_board_hw parks the radio in reset at boot, deliberately. Releasing
       it is this driver's job and nobody else's, so a board with no LoRa
       consumer leaves the part quiet. */
    /* DIO1 is only ever read, so make it an input once here rather
       than hoping it defaulted that way. The XL9535 powers up with every pin
       an input, but ls_board_hw touches this expander and a later change
       there should not silently turn the radio's interrupt line into an
       output. */
    ls_xl9535_set_dir(LS_BOARD_XL_RADIO_DIO1, false);

    /* A part that does not identify straight after a reset is reset again and
       given longer: the first frames of a part still starting come back as
       nonsense, which was read as "nothing in the socket" for the whole
       session. */
    err = ESP_ERR_NOT_FOUND;
    for (unsigned attempt = 0; attempt < START_ATTEMPTS; attempt++) {
        const bool last = attempt + 1 == START_ATTEMPTS;
        if (attempt) {
            ESP_LOGW(TAG, "part not identified (%s), reset and look again (%u/%u)",
                     esp_err_to_name(err), attempt + 1, (unsigned)START_ATTEMPTS);
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        err = reset_part_locked(START_SETTLE_MS[attempt]);
        if (err != ESP_OK) return err;      /* BUSY never fell: nothing to look at */
        err = identify_locked(last);
        if (err != ESP_ERR_NOT_FOUND) return err;
    }
    return err;
}

/* The reset pulse, the identification and the bind are one sequence: held
   whole, so a second task's start waits and then finds the part present. */
esp_err_t ls_lora_start(void)
{
    LOCKED_RET(esp_err_t, start_locked());
}

/* A part that can be used as a LoRa radio, which is what every caller of this
   means by "present" (mesh, waterfall, field, diagnostics screens): one whose
   backend has the LoRa packet path, an SX126x or an LR20xx. ls_lora_chip()
   says which is bound. */
bool ls_lora_present(void) { return s_present && (s_ops->caps() & LS_LORA_CAP_LORA) != 0; }

void ls_lora_stop(void)
{
    /* The SPI device is left registered on purpose: the bus is shared and
       tearing it down would disturb the other parts on it. Forgetting that
       the part answered is the whole job, so the next start re-runs the
       reset and the identification, and a module that was swapped meanwhile
       is not left bound to the wrong backend. */
    ls_lora_hw_lock();
    if (s_ops->stop) s_ops->stop();
    s_present = false;
    s_chip = LS_LORA_CHIP_NONE;
    s_ops = &ls_lora_sx126x_ops;
    ls_lora_hw_unlock();
}

ls_lora_chip_t ls_lora_chip(void) { return s_chip; }

const char *ls_lora_chip_name(void)
{
    if (s_chip == LS_LORA_CHIP_NONE) return "none";
    return s_ops->name_fn ? s_ops->name_fn() : s_ops->name;
}

uint32_t ls_lora_caps(void) { return s_present ? s_ops->caps() : 0; }

/* Everything below that reaches the chip runs with the SPI lock held, and
   reads s_ops after taking it so a start or stop cannot rebind it mid-call.
   What only reads state the dispatcher or a backend keeps in RAM (the
   configuration, the receive/scan/FSK flags, the airtime sum, the scan profile)
   does not touch the bus and does not take it. */
esp_err_t ls_lora_status(uint8_t *out) { LOCKED_RET(esp_err_t, s_ops->status(out)); }

esp_err_t ls_lora_read_reg(uint16_t addr, uint8_t *buf, size_t len)
{ LOCKED_RET(esp_err_t, s_ops->read_reg(addr, buf, len)); }

void ls_lora_cfg_default(ls_lora_cfg_t *out)
{
    if (!out) return;
    /* US915. The image calibration band is 902-928 because that is what the
       vendor driver calibrates for this board; calibrating for the wrong band
       costs several dB of sensitivity and nothing announces it. */
    out->freq_hz     = 910525000u;
    /* SF7, which is what the stock preset actually uses. */

    out->sf          = 7;
    out->bw_hz       = 62500;
    out->cr          = 5;
    out->power_dbm   = 22;
    /* 32 symbols at SF8, not 8. */

    out->preamble    = (out->sf <= 8) ? 32 : 16;
    out->sync_word   = 0x12;
    out->crc_on      = true;
    out->invert_iq   = false;
    out->cal_min_mhz = 902;
    out->cal_max_mhz = 928;
}

const ls_lora_cfg_t *ls_lora_cfg(void) { return s_ops->cfg(); }

esp_err_t ls_lora_configure(const ls_lora_cfg_t *cfg)
{
    /* Bring the part up first so the right backend is bound before it is
       asked to configure anything. */
    ls_lora_hw_lock();
    esp_err_t e = s_present ? ESP_OK : start_locked();
    if (e == ESP_OK) e = s_ops->configure(cfg);
    ls_lora_hw_unlock();
    return e;
}

esp_err_t ls_lora_receive(void) { LOCKED_RET(esp_err_t, s_ops->receive()); }
bool ls_lora_is_receiving(void) { return s_ops->is_receiving(); }
esp_err_t ls_lora_send(const uint8_t *data, size_t len)
{ LOCKED_RET(esp_err_t, s_ops->send(data, len)); }
/* Polls the part's IRQ word and clears it, so it is a bus sequence. */
bool ls_lora_send_done(void) { LOCKED_RET(bool, s_ops->send_done()); }

int ls_lora_poll(uint8_t *buf, size_t max, float *rssi_dbm, float *snr_db)
{ LOCKED_RET(int, s_ops->poll(buf, max, rssi_dbm, snr_db)); }

esp_err_t ls_lora_rssi_inst(float *dbm) { LOCKED_RET(esp_err_t, s_ops->rssi_inst(dbm)); }
uint32_t ls_lora_airtime_ms(int len) { return s_ops->airtime_ms(len); }

esp_err_t ls_lora_fsk_begin(const ls_fsk_cfg_t *cfg)
{ LOCKED_RET(esp_err_t, s_ops->fsk_begin(cfg)); }
int ls_lora_fsk_poll(uint8_t *buf, size_t size, float *rssi_dbm)
{ LOCKED_RET(int, s_ops->fsk_poll(buf, size, rssi_dbm)); }
esp_err_t ls_lora_fsk_send(const uint8_t *data, size_t len)
{ LOCKED_RET(esp_err_t, s_ops->fsk_send(data, len)); }
esp_err_t ls_lora_fsk_receive(void) { LOCKED_RET(esp_err_t, s_ops->fsk_receive()); }
esp_err_t ls_lora_fsk_end(void) { LOCKED_RET(esp_err_t, s_ops->fsk_end()); }
bool ls_lora_fsk_active(void) { return s_ops->fsk_active(); }
esp_err_t ls_lora_fsk_retune(uint32_t freq_hz)
{
    ls_lora_hw_lock();
    const esp_err_t e = (!s_present || !s_ops->fsk_retune) ? ESP_ERR_NOT_SUPPORTED
                                                           : s_ops->fsk_retune(freq_hz);
    ls_lora_hw_unlock();
    return e;
}
int ls_lora_fsk_stream_read(uint8_t *buf, size_t size, bool *restarted)
{
    if (restarted) *restarted = false;
    ls_lora_hw_lock();
    const int n = (!s_present || !s_ops->fsk_stream_read) ? -1
                                                          : s_ops->fsk_stream_read(buf, size, restarted);
    ls_lora_hw_unlock();
    return n;
}

esp_err_t ls_lora_scan_begin(uint32_t min_hz, uint32_t max_hz)
{ LOCKED_RET(esp_err_t, s_ops->scan_begin(min_hz, max_hz)); }
/* A sweep is every look of a row: many passes. The backend takes the lock one
   pass at a time, so the other tasks wait at most a pass, not a sweep. */
int ls_lora_scan_sweep(float *dbm, int n) { return s_ops->scan_sweep(dbm, n); }
int ls_lora_scan_pass(float *dbm, int n, bool *row_done)
{ LOCKED_RET(int, s_ops->scan_pass(dbm, n, row_done)); }
esp_err_t ls_lora_scan_end(void) { LOCKED_RET(esp_err_t, s_ops->scan_end()); }
bool ls_lora_scanning(void) { return s_ops->scanning(); }
void ls_lora_scan_profile(ls_lora_scan_prof_t *out) { s_ops->scan_profile(out); }

esp_err_t ls_lora_modes_begin(uint32_t freq_hz, int gain_step)
{
    ls_lora_hw_lock();
    const esp_err_t e = (!s_present || !s_ops->modes_begin) ? ESP_ERR_NOT_SUPPORTED
                                                            : s_ops->modes_begin(freq_hz, gain_step);
    ls_lora_hw_unlock();
    return e;
}

int ls_lora_modes_poll(uint8_t *buf, size_t size, float *rssi_dbm)
{
    ls_lora_hw_lock();
    const int n = (!s_present || !s_ops->modes_poll) ? -1
                                                     : s_ops->modes_poll(buf, size, rssi_dbm);
    ls_lora_hw_unlock();
    return n;
}

esp_err_t ls_lora_modes_set_gain(int gain_step)
{
    ls_lora_hw_lock();
    const esp_err_t e = (!s_present || !s_ops->modes_set_gain) ? ESP_ERR_NOT_SUPPORTED
                                                               : s_ops->modes_set_gain(gain_step);
    ls_lora_hw_unlock();
    return e;
}

esp_err_t ls_lora_modes_end(void)
{
    ls_lora_hw_lock();
    const esp_err_t e = (!s_present || !s_ops->modes_end) ? ESP_OK : s_ops->modes_end();
    ls_lora_hw_unlock();
    return e;
}

bool ls_lora_modes_active(void)
{
    return s_present && s_ops->modes_active && s_ops->modes_active();
}

esp_err_t ls_lora_modes_tuning(ls_lora_modes_tuning_t *out)
{
    ls_lora_hw_lock();
    const esp_err_t e = (!s_present || !s_ops->modes_tuning) ? ESP_ERR_NOT_SUPPORTED
                                                             : s_ops->modes_tuning(out);
    ls_lora_hw_unlock();
    return e;
}

esp_err_t ls_lora_modes_set_boost(int boost)
{
    ls_lora_hw_lock();
    const esp_err_t e = (!s_present || !s_ops->modes_set_boost) ? ESP_ERR_NOT_SUPPORTED
                                                                : s_ops->modes_set_boost(boost);
    ls_lora_hw_unlock();
    return e;
}

esp_err_t ls_lora_modes_set_bw(uint32_t hz, uint32_t *chosen_hz)
{
    ls_lora_hw_lock();
    const esp_err_t e = (!s_present || !s_ops->modes_set_bw) ? ESP_ERR_NOT_SUPPORTED
                                                             : s_ops->modes_set_bw(hz, chosen_hz);
    ls_lora_hw_unlock();
    return e;
}

esp_err_t ls_lora_modes_set_threshold(int level_db, bool automatic)
{
    ls_lora_hw_lock();
    const esp_err_t e = (!s_present || !s_ops->modes_set_thresh) ? ESP_ERR_NOT_SUPPORTED
                                                                 : s_ops->modes_set_thresh(level_db, automatic);
    ls_lora_hw_unlock();
    return e;
}

esp_err_t ls_lora_modes_read_threshold(int *raw)
{
    ls_lora_hw_lock();
    const esp_err_t e = (!s_present || !s_ops->modes_read_thresh) ? ESP_ERR_NOT_SUPPORTED
                                                                  : s_ops->modes_read_thresh(raw);
    ls_lora_hw_unlock();
    return e;
}

esp_err_t ls_lora_ook_begin(const ls_ook_cfg_t *cfg)
{
    ls_lora_hw_lock();
    const esp_err_t e = (!s_present || !s_ops->ook_begin) ? ESP_ERR_NOT_SUPPORTED : s_ops->ook_begin(cfg);
    ls_lora_hw_unlock();
    return e;
}

int ls_lora_ook_poll(uint8_t *buf, size_t size, float *rssi_dbm)
{
    ls_lora_hw_lock();
    const int n = (!s_present || !s_ops->ook_poll) ? -1 : s_ops->ook_poll(buf, size, rssi_dbm);
    ls_lora_hw_unlock();
    return n;
}

esp_err_t ls_lora_ook_end(void)
{
    ls_lora_hw_lock();
    const esp_err_t e = (!s_present || !s_ops->ook_end) ? ESP_OK : s_ops->ook_end();
    ls_lora_hw_unlock();
    return e;
}

bool ls_lora_ook_active(void)
{
    return s_present && s_ops->ook_active && s_ops->ook_active();
}

void ls_lora_diagnostics(void)
{
    if (!s_present) {
        esp_err_t err = ls_lora_start();
        printf("lora: start %s\n", esp_err_to_name(err));
        if (err != ESP_OK) {
            printf("lora: cs=%d busy=%d rst=expander reset held? check the "
                   "3V3 rail and that ls_board_hw ran\n",
                   (int)CS_PIN, (int)BUSY_PIN);
            return;
        }
    }
    printf("lora: chip %s, caps 0x%02X\n", ls_lora_chip_name(),
           (unsigned)ls_lora_caps());
    s_ops->diagnostics();
}

#else  /* board declares no LoRa */

esp_err_t ls_lora_start(void) { return ESP_ERR_NOT_SUPPORTED; }
void      ls_lora_stop(void) { }
bool      ls_lora_present(void) { return false; }
ls_lora_chip_t ls_lora_chip(void) { return LS_LORA_CHIP_NONE; }
const char *ls_lora_chip_name(void) { return "none"; }
uint32_t  ls_lora_caps(void) { return 0; }
esp_err_t ls_lora_status(uint8_t *out) { (void)out; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t ls_lora_read_reg(uint16_t a, uint8_t *b, size_t l)
{ (void)a; (void)b; (void)l; return ESP_ERR_NOT_SUPPORTED; }
void ls_lora_diagnostics(void) { printf("lora: board declares none\n"); }
void ls_lora_cfg_default(ls_lora_cfg_t *o) { if (o) memset(o, 0, sizeof(*o)); }
esp_err_t ls_lora_configure(const ls_lora_cfg_t *c) { (void)c; return ESP_ERR_NOT_SUPPORTED; }
const ls_lora_cfg_t *ls_lora_cfg(void) { return NULL; }
esp_err_t ls_lora_receive(void) { return ESP_ERR_NOT_SUPPORTED; }
bool ls_lora_is_receiving(void) { return false; }
esp_err_t ls_lora_send(const uint8_t *d, size_t l) { (void)d; (void)l; return ESP_ERR_NOT_SUPPORTED; }
bool ls_lora_send_done(void) { return true; }
int ls_lora_poll(uint8_t *b, size_t m, float *r, float *n)
{ (void)b; (void)m; (void)r; (void)n; return 0; }
esp_err_t ls_lora_rssi_inst(float *d) { (void)d; return ESP_ERR_NOT_SUPPORTED; }
uint32_t ls_lora_airtime_ms(int len) { (void)len; return 0; }

/* The sweep itself cannot exist without a radio. */
esp_err_t ls_lora_fsk_begin(const ls_fsk_cfg_t *c)
{ (void)c; return ESP_ERR_NOT_SUPPORTED; }
int ls_lora_fsk_poll(uint8_t *b, size_t n, float *r)
{ (void)b; (void)n; (void)r; return -1; }
esp_err_t ls_lora_fsk_send(const uint8_t *d, size_t l)
{ (void)d; (void)l; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t ls_lora_fsk_receive(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t ls_lora_fsk_end(void) { return ESP_ERR_NOT_SUPPORTED; }
bool ls_lora_fsk_active(void) { return false; }
esp_err_t ls_lora_fsk_retune(uint32_t f) { (void)f; return ESP_ERR_NOT_SUPPORTED; }
int ls_lora_fsk_stream_read(uint8_t *b, size_t n, bool *r)
{ (void)b; (void)n; if (r) *r = false; return -1; }
esp_err_t ls_lora_scan_begin(uint32_t a, uint32_t b)
{ (void)a; (void)b; return ESP_ERR_NOT_SUPPORTED; }
int ls_lora_scan_sweep(float *d, int n) { (void)d; (void)n; return 0; }
int ls_lora_scan_pass(float *d, int n, bool *done)
{ (void)d; (void)n; if (done) *done = false; return 0; }
esp_err_t ls_lora_scan_end(void) { return ESP_ERR_NOT_SUPPORTED; }
bool ls_lora_scanning(void) { return false; }
esp_err_t ls_lora_modes_begin(uint32_t f, int g) { (void)f; (void)g; return ESP_ERR_NOT_SUPPORTED; }
int ls_lora_modes_poll(uint8_t *b, size_t n, float *r) { (void)b; (void)n; (void)r; return -1; }
esp_err_t ls_lora_modes_set_gain(int g) { (void)g; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t ls_lora_modes_end(void) { return ESP_OK; }
bool ls_lora_modes_active(void) { return false; }
esp_err_t ls_lora_modes_tuning(ls_lora_modes_tuning_t *o) { (void)o; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t ls_lora_modes_set_boost(int b) { (void)b; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t ls_lora_modes_set_bw(uint32_t h, uint32_t *c) { (void)h; (void)c; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t ls_lora_modes_set_threshold(int l, bool a) { (void)l; (void)a; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t ls_lora_modes_read_threshold(int *r) { (void)r; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t ls_lora_ook_begin(const ls_ook_cfg_t *c) { (void)c; return ESP_ERR_NOT_SUPPORTED; }
int ls_lora_ook_poll(uint8_t *b, size_t n, float *r) { (void)b; (void)n; (void)r; return -1; }
esp_err_t ls_lora_ook_end(void) { return ESP_OK; }
bool ls_lora_ook_active(void) { return false; }
void ls_lora_scan_profile(ls_lora_scan_prof_t *o)
{ if (o) { const ls_lora_scan_prof_t z = { 0, 0, 0, 0, 0 }; *o = z; } }

#endif
