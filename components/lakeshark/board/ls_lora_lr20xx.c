/* The LR20xx backend of ls_lora.c: the command layer, the receive-only Mode S
   session, and LoRa packets, FSK sessions and the RSSI sweep with the same
   semantics as ls_lora_sx126x.c. Only ls_lora_read_reg answers
   ESP_ERR_NOT_SUPPORTED. See ls_lora_lr20xx.h for the protocol notes and the
   transmit rule. */
#include "ls_lora.h"
#include "ls_lora_priv.h"
#include "ls_lora_lr20xx.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ls_board.h"
#include "ls_board_hw.h"
#include "ls_xl9535.h"

#if LS_HAS_LORA_LR20XX_PROBE && defined(LS_BOARD_LORA_CS_GPIO) && \
    defined(LS_BOARD_LORA_BUSY_GPIO) && defined(LS_BOARD_XL_RADIO_RST)

static const char *TAG = "ls_lora_lr";

#define FRAME_MAX 24   /* opcode + args, or Stat + up to 14 data bytes, with room */

static uint16_t s_stat;
static uint32_t s_irq;
static bool     s_bound;
static lr20xx_info_t s_info;
/* The frequency last accepted by SetRfFrequency, and whether the LF PA has
   been programmed since the part was last tuned to where it may not transmit.
   lr20xx_set_tx reads both and nothing else decides. */
static uint32_t s_rf_hz;
static bool     s_pa_lf;
/* A fixed receive gain step is in the part (SetAgcGain with a nonzero step), so
   whoever wants the AGC back knows it has to say so. A reset clears it. */
static bool     s_agc_forced;
static bool s_exp_active, s_exp_reset;
static lr20xx_engine_t s_exp_engine;
static unsigned s_exp_preset, s_exp_channel, s_exp_fec;

/* ------------------------------------------------------------------ pure -- */

bool lr20xx_stat_plausible(uint16_t stat)
{
    if (stat & 0xF000u) return false;           /* bits 15:12 are defined 0 */
    if (stat & 0x0008u) return false;           /* rfu */
    if (LR20XX_STAT_RESET_SRC(stat) > 2) return false;
    if (LR20XX_STAT_MODE(stat) > LR20XX_MODE_TX) return false;
    return true;
}

lr20xx_rx_path_t lr20xx_path_for_hz(uint32_t hz)
{
    return hz >= LR20XX_HF_PATH_MIN_HZ ? LR20XX_PATH_HF : LR20XX_PATH_LF;
}

uint16_t lr20xx_calib_fe_word(uint32_t hz, lr20xx_rx_path_t path)
{
    const uint32_t steps = hz / 4000000u;
    if (steps == 0 || steps > 0x7FFFu) return 0;
    return (uint16_t)(steps | (path == LR20XX_PATH_HF ? 0x8000u : 0u));
}

float lr20xx_rssi_dbm(uint8_t b0, uint8_t b1)
{
    const unsigned raw = ((unsigned)b0 << 1) | ((unsigned)(b1 >> 7) & 1u);
    return -(float)raw / 2.0f;
}

/* ---------------------------------------------------------------- frames -- */

uint16_t lr20xx_last_stat(void) { return s_stat; }
uint32_t lr20xx_last_irq(void)  { return s_irq; }

/* The last few frames, opcode and the Stat each one clocked back: Stat in a
   command's own frame is the status of the command before it, so a failed
   check can say which command the chip refused. */
#define TRACE_N 6
static struct { uint16_t op, stat; } s_trace[TRACE_N];
static unsigned s_trace_n;

static void trace_dump(void)
{
    char line[TRACE_N * 16 + 1] = "";
    size_t o = 0;
    for (unsigned i = 0; i < TRACE_N && i < s_trace_n; i++) {
        const unsigned k = (s_trace_n - 1 - i) % TRACE_N;
        o += (size_t)snprintf(line + o, sizeof(line) - o, " %04X>%04X",
                              (unsigned)s_trace[k].op, (unsigned)s_trace[k].stat);
    }
    ESP_LOGW(TAG, "frames, newest first (op>stat it returned):%s", line);
}

/* The part restarted under the driver: some frame's Stat carried a reset
   source, or the driver reset it itself. GetStatus clears the source, and the
   checks after every command are GetStatus, so a restart seen by one of them
   was lost to the poll that would have reprogrammed the part: it kept going on
   a part that had forgotten its packet type and refused everything. Whoever
   reprograms the part takes the flag. */
static bool s_reset_seen;
/* Commands refused or timed out one after another, cleared by one the part
   accepted. A run of WEDGE_RUN is a part that has stopped taking commands. */
#define WEDGE_RUN 3
static unsigned s_fail_run;

static bool take_reset(uint16_t stat)
{
    const bool r = s_reset_seen || (lr20xx_stat_plausible(stat) && LR20XX_STAT_RESET_SRC(stat) != 0);
    s_reset_seen = false;
    if (r) s_agc_forced = false;
    return r;
}

static esp_err_t frame(uint8_t *io, size_t n)
{
    spi_device_handle_t dev = ls_lora_hw_dev();
    if (!dev) return ESP_ERR_INVALID_STATE;
    const uint16_t op = n >= 2 ? (uint16_t)((io[0] << 8) | io[1]) : 0;
    spi_transaction_t t = { .length = n * 8, .tx_buffer = io, .rx_buffer = io };
    esp_err_t err = ls_lora_hw_transmit(&t);
    if (err == ESP_OK && n >= 2) {
        s_stat = (uint16_t)((io[0] << 8) | io[1]);
        if (lr20xx_stat_plausible(s_stat) && LR20XX_STAT_RESET_SRC(s_stat) != 0) s_reset_seen = true;
        s_trace[s_trace_n % TRACE_N].op = op;
        s_trace[s_trace_n % TRACE_N].stat = s_stat;
        s_trace_n++;
    }
    return err;
}

/* Every command below holds the SPI lock from its BUSY wait to its last frame
   (ls_lora_priv.h). A read is two frames with a BUSY wait between them, and a
   frame from another task in that gap is read back as the answer; the Stat the
   composite commands judge afterwards is s_stat, which any frame overwrites. */
#define LOCKED_RET(type, expr)                                                \
    do { ls_lora_hw_lock(); type r_ = (expr); ls_lora_hw_unlock(); return r_; } while (0)

static esp_err_t write_locked(uint16_t op, const uint8_t *args, size_t n)
{
    if (1 + 1 + n > FRAME_MAX || (n && !args)) return ESP_ERR_INVALID_SIZE;
    if (!ls_lora_hw_wait_not_busy(20)) {
        ESP_LOGW(TAG, "busy stuck high before 0x%04X", (unsigned)op);
        s_fail_run++;
        return ESP_ERR_TIMEOUT;
    }
    uint8_t buf[FRAME_MAX];
    const size_t len = 2 + n;
    buf[0] = (uint8_t)(op >> 8);
    buf[1] = (uint8_t)op;
    if (n) memcpy(&buf[2], args, n);
    esp_err_t err = frame(buf, len);
    if (err == ESP_OK && len >= 6)
        s_irq = ((uint32_t)buf[2] << 24) | ((uint32_t)buf[3] << 16) |
                ((uint32_t)buf[4] << 8) | buf[5];
    return err;
}

esp_err_t lr20xx_write(uint16_t op, const uint8_t *args, size_t n)
{
    LOCKED_RET(esp_err_t, write_locked(op, args, n));
}

static esp_err_t read_locked(uint16_t op, const uint8_t *args, size_t n,
                             uint8_t *out, size_t m)
{
    if (2 + n > FRAME_MAX || 2 + m > FRAME_MAX || m > 14 || (n && !args))
        return ESP_ERR_INVALID_SIZE;
    if (!ls_lora_hw_wait_not_busy(20)) {
        ESP_LOGW(TAG, "busy stuck high before read 0x%04X", (unsigned)op);
        s_fail_run++;
        return ESP_ERR_TIMEOUT;
    }
    uint8_t buf[FRAME_MAX];
    buf[0] = (uint8_t)(op >> 8);
    buf[1] = (uint8_t)op;
    if (n) memcpy(&buf[2], args, n);
    esp_err_t err = frame(buf, 2 + n);
    if (err != ESP_OK) return err;

    /* The step that is easy to forget: the part is preparing the answer and
       BUSY says so. Clocking the second frame early reads nothing. */
    if (!ls_lora_hw_wait_not_busy(20)) {
        ESP_LOGW(TAG, "busy stuck high after the first frame of read 0x%04X",
                 (unsigned)op);
        s_fail_run++;
        return ESP_ERR_TIMEOUT;
    }
    memset(buf, 0, 2 + m);
    err = frame(buf, 2 + m);
    if (err == ESP_OK && out && m) memcpy(out, &buf[2], m);
    return err;
}

esp_err_t lr20xx_read(uint16_t op, const uint8_t *args, size_t n,
                      uint8_t *out, size_t m)
{
    LOCKED_RET(esp_err_t, read_locked(op, args, n, out, m));
}

/* -------------------------------------------------------------- commands -- */

static esp_err_t get_status_locked(lr20xx_status_t *out)
{
    if (!ls_lora_hw_wait_not_busy(20)) {
        ESP_LOGW(TAG, "busy stuck high before GetStatus");
        s_fail_run++;
        return ESP_ERR_TIMEOUT;
    }
    uint8_t buf[6] = { LR20XX_OP_GET_STATUS >> 8, (uint8_t)LR20XX_OP_GET_STATUS, 0, 0, 0, 0 };
    esp_err_t err = frame(buf, sizeof(buf));
    if (err != ESP_OK) return err;
    s_irq = ((uint32_t)buf[2] << 24) | ((uint32_t)buf[3] << 16) |
            ((uint32_t)buf[4] << 8) | buf[5];
    if (out) { out->stat = s_stat; out->irq = s_irq; }
    return ESP_OK;
}

esp_err_t lr20xx_get_status(lr20xx_status_t *out)
{
    LOCKED_RET(esp_err_t, get_status_locked(out));
}

esp_err_t lr20xx_get_version(uint8_t *major, uint8_t *minor)
{
    uint8_t v[2] = { 0, 0 };
    esp_err_t err = lr20xx_read(LR20XX_OP_GET_VERSION, NULL, 0, v, sizeof(v));
    if (err != ESP_OK) return err;
    if (major) *major = v[0];
    if (minor) *minor = v[1];
    return ESP_OK;
}

esp_err_t lr20xx_check_last_ok(void)
{
    lr20xx_status_t st;
    esp_err_t err = lr20xx_get_status(&st);
    if (err != ESP_OK) return err;
    if (!lr20xx_stat_plausible(st.stat) || LR20XX_STAT_CMD(st.stat) != LR20XX_CMD_OK) {
        ESP_LOGW(TAG, "last command not accepted: stat 0x%04X (cmd status %u)",
                 (unsigned)st.stat, (unsigned)LR20XX_STAT_CMD(st.stat));
        trace_dump();
        s_fail_run++;
        return ESP_ERR_INVALID_RESPONSE;
    }
    s_fail_run = 0;
    return ESP_OK;
}

static esp_err_t probe_locked(lr20xx_info_t *info)
{
    lr20xx_status_t st;
    esp_err_t err = lr20xx_get_status(&st);
    if (err != ESP_OK) return err;
    /* All zeros and all ones are a bus nobody drives. Zeros are a "plausible"
       Stat, so they are refused here and not left to the version check alone. */
    if (st.stat == 0x0000 || !lr20xx_stat_plausible(st.stat)) return ESP_ERR_NOT_FOUND;

    uint8_t major = 0, minor = 0;
    err = lr20xx_get_version(&major, &minor);
    if (err != ESP_OK) return err;
    if (!lr20xx_stat_plausible(lr20xx_last_stat())) return ESP_ERR_NOT_FOUND;
    /* LR2021 is 1.24; LR2012 and LR2022 are both 2.0 (datasheet 6.7.2). A
       different minor on major 1 is still an LR2021, only a firmware the
       datasheet does not list. */
    if (major != 1 && major != 2) return ESP_ERR_NOT_FOUND;

    if (info) {
        info->fw_major = major;
        info->fw_minor = minor;
        info->stat = st.stat;
        info->lr2021 = major == 1;
    }
    if (major == 1 && minor != 0x18)
        ESP_LOGW(TAG, "LR2021 firmware 1.%u, not the 1.24 the datasheet lists",
                 (unsigned)minor);
    return ESP_OK;
}

esp_err_t lr20xx_probe(lr20xx_info_t *info)
{
    LOCKED_RET(esp_err_t, probe_locked(info));
}

void lr20xx_bind(const lr20xx_info_t *info)
{
    if (info) s_info = *info;
    s_bound = true;
    s_reset_seen = false;       /* the probe's GetStatus read the reset source; nothing is configured yet */
    s_fail_run = 0;
    /* Start the IRQ poll from nothing pending. */
    (void)lr20xx_clear_irq(LR20XX_IRQ_ALL);
}

esp_err_t lr20xx_set_standby(bool xosc)
{
    const uint8_t mode = xosc ? 1 : 0;
    return lr20xx_write(LR20XX_OP_SET_STANDBY, &mode, 1);
}

esp_err_t lr20xx_set_rf_frequency(uint32_t hz)
{
    if (hz < LR20XX_FREQ_MIN_HZ || hz > LR20XX_FREQ_MAX_HZ) return ESP_ERR_INVALID_ARG;
    const uint8_t a[4] = { (uint8_t)(hz >> 24), (uint8_t)(hz >> 16),
                           (uint8_t)(hz >> 8), (uint8_t)hz };
    /* Forgotten before the frame goes out, so a frame that fails cannot leave
       a transmit armed for a frequency the part may now be on. */
    if (hz >= LR20XX_TX_FREQ_LIMIT_HZ) s_pa_lf = false;
    s_rf_hz = 0;
    const esp_err_t err = lr20xx_write(LR20XX_OP_SET_RF_FREQUENCY, a, sizeof(a));
    if (err == ESP_OK) s_rf_hz = hz;
    return err;
}

esp_err_t lr20xx_set_rx_path(lr20xx_rx_path_t path, uint8_t boost)
{
    if ((int)path != LR20XX_PATH_LF && (int)path != LR20XX_PATH_HF) return ESP_ERR_INVALID_ARG;
    if (boost > 7) return ESP_ERR_INVALID_ARG;
    const uint8_t a[2] = { (uint8_t)path, boost };
    return lr20xx_write(LR20XX_OP_SET_RX_PATH, a, sizeof(a));
}

esp_err_t lr20xx_calib_fe(const uint16_t words[3])
{
    if (!words) return ESP_ERR_INVALID_ARG;
    const uint8_t a[6] = { (uint8_t)(words[0] >> 8), (uint8_t)words[0],
                           (uint8_t)(words[1] >> 8), (uint8_t)words[1],
                           (uint8_t)(words[2] >> 8), (uint8_t)words[2] };
    return lr20xx_write(LR20XX_OP_CALIB_FE, a, sizeof(a));
}

esp_err_t lr20xx_calib_fe_hz(uint32_t hz, lr20xx_rx_path_t path)
{
    const uint16_t w = lr20xx_calib_fe_word(hz, path);
    if (!w) return ESP_ERR_INVALID_ARG;
    const uint16_t words[3] = { w, 0, 0 };
    return lr20xx_calib_fe(words);
}

esp_err_t lr20xx_set_rx(uint32_t timeout)
{
    if (timeout > 0xFFFFFFu) return ESP_ERR_INVALID_ARG;
    const uint8_t a[3] = { (uint8_t)(timeout >> 16), (uint8_t)(timeout >> 8), (uint8_t)timeout };
    return lr20xx_write(LR20XX_OP_SET_RX, a, sizeof(a));
}

static esp_err_t get_rssi_inst_locked(float *dbm)
{
    uint8_t d[2] = { 0, 0 };
    esp_err_t err = lr20xx_read(LR20XX_OP_GET_RSSI_INST, NULL, 0, d, sizeof(d));
    if (err != ESP_OK) return err;
    /* The answer comes back behind the Stat of the read. Refuse a bus that
       reads as nothing, and a part that says the read failed. */
    const uint16_t s = lr20xx_last_stat();
    if (!lr20xx_stat_plausible(s) || s == 0x0000 || LR20XX_STAT_CMD(s) < LR20XX_CMD_OK)
        return ESP_ERR_INVALID_RESPONSE;
    if (dbm) *dbm = lr20xx_rssi_dbm(d[0], d[1]);
    return ESP_OK;
}

esp_err_t lr20xx_get_rssi_inst(float *dbm)
{
    LOCKED_RET(esp_err_t, get_rssi_inst_locked(dbm));
}

esp_err_t lr20xx_get_errors(uint16_t *errors)
{
    uint8_t d[2] = { 0, 0 };
    esp_err_t err = lr20xx_read(LR20XX_OP_GET_ERRORS, NULL, 0, d, sizeof(d));
    if (err == ESP_OK && errors) *errors = (uint16_t)((d[0] << 8) | d[1]);
    return err;
}

esp_err_t lr20xx_clear_errors(void)
{
    return lr20xx_write(LR20XX_OP_CLEAR_ERRORS, NULL, 0);
}

esp_err_t lr20xx_clear_irq(uint32_t mask)
{
    const uint8_t a[4] = { (uint8_t)(mask >> 24), (uint8_t)(mask >> 16),
                           (uint8_t)(mask >> 8), (uint8_t)mask };
    return lr20xx_write(LR20XX_OP_CLEAR_IRQ, a, sizeof(a));
}

esp_err_t lr20xx_get_and_clear_irq(uint32_t *irq)
{
    uint8_t d[4] = { 0, 0, 0, 0 };
    esp_err_t err = lr20xx_read(LR20XX_OP_GET_CLEAR_IRQ, NULL, 0, d, sizeof(d));
    if (err == ESP_OK && irq)
        *irq = ((uint32_t)d[0] << 24) | ((uint32_t)d[1] << 16) | ((uint32_t)d[2] << 8) | d[3];
    return err;
}

esp_err_t lr20xx_irq_pending(bool *pending, uint32_t *irq)
{
    lr20xx_status_t st;
    esp_err_t err = lr20xx_get_status(&st);
    if (err != ESP_OK) return err;
    if (!lr20xx_stat_plausible(st.stat)) return ESP_ERR_INVALID_RESPONSE;
    if (pending) *pending = LR20XX_STAT_IRQ(st.stat) != 0;
    if (irq) *irq = st.irq;
    return ESP_OK;
}

esp_err_t lr20xx_tune_rx(uint32_t hz, int boost)
{
    if (hz < LR20XX_FREQ_MIN_HZ || hz > LR20XX_FREQ_MAX_HZ) return ESP_ERR_INVALID_ARG;
    if (boost < -1 || boost > 7) return ESP_ERR_INVALID_ARG;
    const lr20xx_rx_path_t path = lr20xx_path_for_hz(hz);
    const uint8_t b = boost >= 0 ? (uint8_t)boost : (path == LR20XX_PATH_HF ? 4 : 0);

    esp_err_t err = lr20xx_calib_fe_hz(hz, path);
    if (err == ESP_OK) err = lr20xx_check_last_ok();
    if (err == ESP_OK) err = lr20xx_set_rf_frequency(hz);
    if (err == ESP_OK) err = lr20xx_check_last_ok();
    if (err == ESP_OK) err = lr20xx_set_rx_path(path, b);
    if (err == ESP_OK) err = lr20xx_check_last_ok();
    return err;
}

/* ----------------------------------------- DIO, FIFO and OOK commands ----- */

esp_err_t lr20xx_set_dio_function(uint8_t dio, uint8_t func, uint8_t pull)
{
    if (dio < 5 || dio > 11 || func > 0xF || pull > 3) return ESP_ERR_INVALID_ARG;
    const uint8_t a[2] = { dio, (uint8_t)((func << 4) | pull) };
    return lr20xx_write(LR20XX_OP_SET_DIO_FUNCTION, a, sizeof(a));
}

esp_err_t lr20xx_set_dio_rf_switch(uint8_t dio, uint8_t config)
{
    if (dio < 5 || dio > 11 || (config & 0xE0)) return ESP_ERR_INVALID_ARG;
    const uint8_t a[2] = { dio, config };
    return lr20xx_write(LR20XX_OP_SET_DIO_RFSW, a, sizeof(a));
}

esp_err_t lr20xx_set_dio_irq(uint8_t dio, uint32_t irq_mask)
{
    if (dio < 5 || dio > 11) return ESP_ERR_INVALID_ARG;
    const uint8_t a[5] = { dio, (uint8_t)(irq_mask >> 24), (uint8_t)(irq_mask >> 16),
                           (uint8_t)(irq_mask >> 8), (uint8_t)irq_mask };
    return lr20xx_write(LR20XX_OP_SET_DIO_IRQ, a, sizeof(a));
}

esp_err_t lr20xx_wait_ready(int timeout_ms)
{
    return ls_lora_hw_wait_not_busy(timeout_ms) ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t lr20xx_calibrate(uint8_t blocks)
{
    if (blocks & 0x90) return ESP_ERR_INVALID_ARG;   /* rfu bits 7 and 4 */
    return lr20xx_write(LR20XX_OP_CALIBRATE, &blocks, 1);
}

esp_err_t lr20xx_set_fallback_standby_rc(void)
{
    const uint8_t mode = LR20XX_FALLBACK_STBY_RC;
    return lr20xx_write(LR20XX_OP_SET_FALLBACK, &mode, 1);
}

esp_err_t lr20xx_set_packet_type_ook(void)
{
    const uint8_t type = LR20XX_PKT_TYPE_OOK;
    return lr20xx_write(LR20XX_OP_SET_PACKET_TYPE, &type, 1);
}

esp_err_t lr20xx_set_agc_gain(uint8_t step)
{
    if (step > LR20XX_MODES_GAIN_MAX) return ESP_ERR_INVALID_ARG;
    const esp_err_t err = lr20xx_write(LR20XX_OP_SET_AGC_GAIN, &step, 1);
    if (err == ESP_OK) s_agc_forced = step != 0;
    return err;
}

esp_err_t lr20xx_clear_rx_fifo(void)
{
    return lr20xx_write(LR20XX_OP_CLEAR_RX_FIFO, NULL, 0);
}

static esp_err_t get_rx_fifo_level_locked(uint16_t *level)
{
    uint8_t d[2] = { 0, 0 };
    esp_err_t err = lr20xx_read(LR20XX_OP_GET_RX_FIFO_LVL, NULL, 0, d, sizeof(d));
    if (err != ESP_OK) return err;
    const uint16_t s = lr20xx_last_stat();
    if (!lr20xx_stat_plausible(s) || s == 0x0000 || LR20XX_STAT_CMD(s) < LR20XX_CMD_OK)
        return ESP_ERR_INVALID_RESPONSE;
    if (level) *level = (uint16_t)((d[0] << 8) | d[1]);
    return ESP_OK;
}

esp_err_t lr20xx_get_rx_fifo_level(uint16_t *level)
{
    LOCKED_RET(esp_err_t, get_rx_fifo_level_locked(level));
}

#define FIFO_READ_MAX 257   /* the 259-byte packet buffer less the two Stat bytes */

static esp_err_t read_rx_fifo_locked(uint8_t *dst, size_t n)
{
    uint8_t *pkt = ls_lora_hw_pkt();
    if (!pkt) return ESP_ERR_INVALID_STATE;
    if (!dst || n == 0 || n > FIFO_READ_MAX) return ESP_ERR_INVALID_ARG;
    if (!ls_lora_hw_wait_not_busy(20)) {
        ESP_LOGW(TAG, "busy stuck high before the FIFO read");
        s_fail_run++;
        return ESP_ERR_TIMEOUT;
    }
    memset(pkt, 0, 2 + n);
    pkt[0] = (uint8_t)(LR20XX_OP_READ_RX_FIFO >> 8);
    pkt[1] = (uint8_t)LR20XX_OP_READ_RX_FIFO;
    esp_err_t err = frame(pkt, 2 + n);
    if (err != ESP_OK) return err;
    memcpy(dst, &pkt[2], n);
    return ESP_OK;
}

/* The packet buffer is the one DMA buffer, so it is held from the fill to the
   copy out. */
esp_err_t lr20xx_read_rx_fifo(uint8_t *dst, size_t n)
{
    LOCKED_RET(esp_err_t, read_rx_fifo_locked(dst, n));
}

/* 6.2.2: Addr(3), Mask(4), Data(4). Only the bits of Mask change. */
esp_err_t lr20xx_write_reg_mask32(uint32_t addr, uint32_t mask, uint32_t data)
{
    if (addr > 0xFFFFFFu) return ESP_ERR_INVALID_ARG;
    const uint8_t a[11] = { (uint8_t)(addr >> 16), (uint8_t)(addr >> 8), (uint8_t)addr,
                            (uint8_t)(mask >> 24), (uint8_t)(mask >> 16), (uint8_t)(mask >> 8), (uint8_t)mask,
                            (uint8_t)(data >> 24), (uint8_t)(data >> 16), (uint8_t)(data >> 8), (uint8_t)data };
    return lr20xx_write(LR20XX_OP_WRITE_REG_MASK, a, sizeof(a));
}

/* 6.2.3 with Len 1: Addr(3), Len(1); the answer is Stat then four data bytes. */
esp_err_t lr20xx_read_reg32(uint32_t addr, uint32_t *value)
{
    if (addr > 0xFFFFFFu) return ESP_ERR_INVALID_ARG;
    const uint8_t a[4] = { (uint8_t)(addr >> 16), (uint8_t)(addr >> 8), (uint8_t)addr, 1 };
    uint8_t d[4] = { 0, 0, 0, 0 };
    ls_lora_hw_lock();
    esp_err_t err = lr20xx_read(LR20XX_OP_READ_REG, a, sizeof(a), d, sizeof(d));
    const uint16_t s = lr20xx_last_stat();
    ls_lora_hw_unlock();
    if (err != ESP_OK) return err;
    if (!lr20xx_stat_plausible(s) || s == 0x0000 || LR20XX_STAT_CMD(s) < LR20XX_CMD_OK)
        return ESP_ERR_INVALID_RESPONSE;
    if (value) *value = ((uint32_t)d[0] << 24) | ((uint32_t)d[1] << 16) | ((uint32_t)d[2] << 8) | d[3];
    return ESP_OK;
}

esp_err_t lr20xx_ook_set_modulation(uint32_t bitrate_bps, uint8_t pulse_shape,
                                    uint8_t rx_bw, uint8_t depth)
{
    if (bitrate_bps == 0 || (bitrate_bps & 0x80000000u) || depth > 1)
        return ESP_ERR_INVALID_ARG;
    const uint8_t a[7] = { (uint8_t)(bitrate_bps >> 24), (uint8_t)(bitrate_bps >> 16),
                           (uint8_t)(bitrate_bps >> 8), (uint8_t)bitrate_bps,
                           pulse_shape, rx_bw, depth };
    return lr20xx_write(LR20XX_OP_SET_OOK_MOD, a, sizeof(a));
}

esp_err_t lr20xx_ook_set_packet_fixed(uint16_t pre_len_tx, uint16_t payload_len,
                                      uint8_t crc_type)
{
    if (crc_type > 4) return ESP_ERR_INVALID_ARG;
    /* byte 4: rfu(3:0) | addr_comp(1:0) = 0 | pkt_format(1:0) = 0, fixed length;
       byte 7: Crc(3:0) in the high nibble, Manchester(3:0) = 0, off. */
    const uint8_t a[6] = { (uint8_t)(pre_len_tx >> 8), (uint8_t)pre_len_tx, 0x00,
                           (uint8_t)(payload_len >> 8), (uint8_t)payload_len,
                           (uint8_t)(crc_type << 4) };
    return lr20xx_write(LR20XX_OP_SET_OOK_PKT, a, sizeof(a));
}

esp_err_t lr20xx_ook_set_sync_none(void)
{
    /* Syncword(31:0) = 0, bit_order 1 (MSB first, as RadioLib's ADS-B setup),
       nb_bits(6:0) = 0: nothing is matched after the preamble pattern. */
    const uint8_t a[5] = { 0, 0, 0, 0, 0x80 };
    return lr20xx_write(LR20XX_OP_SET_OOK_SYNC, a, sizeof(a));
}

esp_err_t lr20xx_ook_set_detector(uint16_t pattern, uint8_t length_bits, uint8_t repeats)
{
    if (length_bits < 1 || length_bits > 16 || repeats > 31) return ESP_ERR_INVALID_ARG;
    /* The part answers a parameter error, measured on an LR2021 FW 1.24 over
       50 patterns, unless the length is even and the first two chips (bits 0
       and 1) differ; refused here so the caller hears why at once. */
    if ((length_bits & 1u) || !((pattern ^ (pattern >> 1)) & 1u)) return ESP_ERR_INVALID_ARG;
    /* byte 4 pattern_length(3:0) is the length minus one; byte 6 is sw_is_raw 0,
       sfd_kind 0 (falling edge) and sfd_length 0, all required for ADS-B. */
    const uint8_t a[5] = { (uint8_t)(pattern >> 8), (uint8_t)pattern,
                           (uint8_t)(length_bits - 1), repeats, 0x00 };
    return lr20xx_write(LR20XX_OP_SET_OOK_DETECTOR, a, sizeof(a));
}

static esp_err_t get_ook_packet_status_locked(lr20xx_ook_pkt_status_t *out)
{
    uint8_t d[6] = { 0 };
    esp_err_t err = lr20xx_read(LR20XX_OP_GET_OOK_PKT_STAT, NULL, 0, d, sizeof(d));
    if (err != ESP_OK) return err;
    const uint16_t s = lr20xx_last_stat();
    if (!lr20xx_stat_plausible(s) || s == 0x0000 || LR20XX_STAT_CMD(s) < LR20XX_CMD_OK)
        return ESP_ERR_INVALID_RESPONSE;
    if (out) {
        /* Table 16-9: pkt_len(2), rssi_avg(8:1), RssiHigh(8:1), then a byte
           with rssi_avg(0) in bit 2 and RssiHigh(0) in bit 0, then Lqi. */
        out->pkt_len = (uint16_t)((d[0] << 8) | d[1]);
        out->rssi_avg_dbm  = -(float)(((unsigned)d[2] << 1) | ((d[4] >> 2) & 1u)) / 2.0f;
        out->rssi_high_dbm = -(float)(((unsigned)d[3] << 1) | (d[4] & 1u)) / 2.0f;
        out->lqi = d[5];
    }
    return ESP_OK;
}

esp_err_t lr20xx_get_ook_packet_status(lr20xx_ook_pkt_status_t *out)
{
    LOCKED_RET(esp_err_t, get_ook_packet_status_locked(out));
}

lr20xx_rx_bw_t lr20xx_wide_rx_bw(uint32_t min_hz)
{
    /* Table 11-4, the four rungs at 2 MHz and above, narrowest first. */
    static const lr20xx_rx_bw_t ladder[] = {
        { 2222222u, 192 }, { 2666667u, 128 }, { 2857143u, 64 }, { 3076923u, 0 },
    };
    for (size_t i = 0; i < sizeof(ladder) / sizeof(ladder[0]); i++)
        if (ladder[i].hz >= min_hz) return ladder[i];
    return ladder[sizeof(ladder) / sizeof(ladder[0]) - 1];
}

/* Table 11-4, the rungs from 3076.9 kHz to 512.8 kHz, widest first. */
const lr20xx_rx_bw_t lr20xx_ook_rx_bw_table[] = {
    { 3076923u, 0 },   { 2857143u, 64 },  { 2666667u, 128 }, { 2222222u, 192 },
    { 1333333u, 136 }, { 1111111u, 200 }, { 888889u, 144 },  { 769231u, 24 },
    { 740741u, 208 },  { 714286u, 88 },   { 666667u, 152 },  { 615385u, 32 },
    { 571429u, 96 },   { 555556u, 216 },  { 533333u, 160 },  { 512821u, 17 },
};
const int lr20xx_ook_rx_bw_table_n =
    (int)(sizeof(lr20xx_ook_rx_bw_table) / sizeof(lr20xx_ook_rx_bw_table[0]));

lr20xx_rx_bw_t lr20xx_ook_rx_bw_nearest(uint32_t hz)
{
    int best = 0;
    uint32_t best_d = UINT32_MAX;
    for (int i = 0; i < lr20xx_ook_rx_bw_table_n; i++) {
        const uint32_t w = lr20xx_ook_rx_bw_table[i].hz;
        const uint32_t d = w > hz ? w - hz : hz - w;
        if (d < best_d) { best_d = d; best = i; }      /* strictly less: a tie keeps the wider */
    }
    return lr20xx_ook_rx_bw_table[best];
}

/* The interrupts the session reads. RxDone drives it; the rest are how a
   dropped or failed reception shows up. */
#define MODES_IRQ_MASK (LR20XX_IRQ_RX_DONE | LR20XX_IRQ_TIMEOUT | LR20XX_IRQ_CRC_ERROR | \
                        LR20XX_IRQ_LEN_ERROR | LR20XX_IRQ_CMD_ERROR | LR20XX_IRQ_ERROR)

static esp_err_t configure_dios_mask(uint32_t irq_mask)
{
    esp_err_t err = ESP_OK;
#ifdef LS_BOARD_LR20XX_RFSW_TABLE
    static const struct { uint8_t dio, config; } rfsw[] = { LS_BOARD_LR20XX_RFSW_TABLE };
    /* Nothing transmits on the HF path, so a table that routes Tx HF to the
       antenna is a board header error, refused before a single frame goes
       out. Tx LF is the transmit state this driver uses. */
    for (size_t i = 0; i < sizeof(rfsw) / sizeof(rfsw[0]); i++)
        if (rfsw[i].config & LR20XX_RFSW_TX_HF) {
            ESP_LOGE(TAG, "RF switch table sets the HF transmit state on DIO%u", (unsigned)rfsw[i].dio);
            return ESP_ERR_INVALID_ARG;
        }
    for (size_t i = 0; i < sizeof(rfsw) / sizeof(rfsw[0]) && err == ESP_OK; i++) {
        err = lr20xx_set_dio_function(rfsw[i].dio, LR20XX_DIO_FUNC_RF_SWITCH, LR20XX_DIO_PULL_AUTO);
        if (err == ESP_OK) err = lr20xx_check_last_ok();
        if (err == ESP_OK) err = lr20xx_set_dio_rf_switch(rfsw[i].dio, rfsw[i].config);
        if (err == ESP_OK) err = lr20xx_check_last_ok();
    }
#endif
#ifdef LS_BOARD_LR20XX_IRQ_DIO
    if (err == ESP_OK)
        err = lr20xx_set_dio_function(LS_BOARD_LR20XX_IRQ_DIO, LR20XX_DIO_FUNC_IRQ, LR20XX_DIO_PULL_NONE);
    if (err == ESP_OK) err = lr20xx_check_last_ok();
    if (err == ESP_OK) err = lr20xx_set_dio_irq(LS_BOARD_LR20XX_IRQ_DIO, irq_mask);
    if (err == ESP_OK) err = lr20xx_check_last_ok();
#else
    (void)irq_mask;
#endif
    return err;
}

esp_err_t lr20xx_configure_dios(void)
{
    return configure_dios_mask(MODES_IRQ_MASK);
}

/* ------------------------------ LoRa, GFSK and LF transmit commands ------ */

esp_err_t lr20xx_set_packet_type(uint8_t type)
{
    return lr20xx_write(LR20XX_OP_SET_PACKET_TYPE, &type, 1);
}

/* RadioLib's LR2021 table, narrowest first. 101.5625, 203.125, 406.25 and
   812.5 kHz are rounded to the nearest hertz; the airtime they give is within
   a microsecond a symbol. */
const lr20xx_lora_bw_t lr20xx_lora_bw_table[] = {
    {   31250, 0x02 }, {   41667, 0x0A }, {   62500, 0x03 }, {   83333, 0x0B },
    {  101563, 0x0C }, {  125000, 0x04 }, {  203125, 0x0D }, {  250000, 0x05 },
    {  406250, 0x0E }, {  500000, 0x06 }, {  812500, 0x0F }, { 1000000, 0x07 },
};
const int lr20xx_lora_bw_table_n =
    (int)(sizeof(lr20xx_lora_bw_table) / sizeof(lr20xx_lora_bw_table[0]));

lr20xx_lora_bw_t lr20xx_lora_bw_nearest(uint32_t hz)
{
    int best = 0;
    uint32_t best_d = UINT32_MAX;
    for (int i = 0; i < lr20xx_lora_bw_table_n; i++) {
        const uint32_t w = lr20xx_lora_bw_table[i].hz;
        const uint32_t d = w > hz ? w - hz : hz - w;
        if (d < best_d) { best_d = d; best = i; }      /* strictly less: a tie keeps the narrower */
    }
    return lr20xx_lora_bw_table[best];
}

bool lr20xx_lora_ldro(uint8_t sf, uint32_t bw_hz)
{
    /* 2^SF / BW >= 16 ms, kept in integers so it agrees exactly with the
       decision the airtime sum makes. */
    if (!bw_hz || sf > 12) return false;
    return ((uint64_t)1 << sf) * 1000000ull >= 16000ull * bw_hz;
}

esp_err_t lr20xx_lora_set_modulation(uint8_t sf, uint8_t bw_code, uint8_t cr, bool ldro)
{
    if (sf < 5 || sf > 12 || bw_code > 0x0F || cr < 1 || cr > 4) return ESP_ERR_INVALID_ARG;
    const uint8_t a[2] = { (uint8_t)((sf << 4) | bw_code), (uint8_t)((cr << 4) | (ldro ? 1 : 0)) };
    return lr20xx_write(LR20XX_OP_SET_LORA_MOD, a, sizeof(a));
}

esp_err_t lr20xx_lora_set_packet(uint16_t preamble, uint8_t payload_len, bool implicit,
                                 bool crc_on, bool invert_iq)
{
    const uint8_t a[4] = { (uint8_t)(preamble >> 8), (uint8_t)preamble, payload_len,
                           (uint8_t)((implicit ? 4 : 0) | (crc_on ? 2 : 0) | (invert_iq ? 1 : 0)) };
    return lr20xx_write(LR20XX_OP_SET_LORA_PKT, a, sizeof(a));
}

esp_err_t lr20xx_lora_set_syncword(uint8_t sync_word)
{
    return lr20xx_write(LR20XX_OP_SET_LORA_SYNC, &sync_word, 1);
}

/* A read whose Stat says it was answered. */
static bool read_stat_ok(void)
{
    const uint16_t s = lr20xx_last_stat();
    return lr20xx_stat_plausible(s) && s != 0x0000 && LR20XX_STAT_CMD(s) >= LR20XX_CMD_OK;
}

esp_err_t lr20xx_get_lora_packet_status(lr20xx_lora_pkt_status_t *out)
{
    uint8_t d[9] = { 0 };
    ls_lora_hw_lock();
    esp_err_t err = lr20xx_read(LR20XX_OP_GET_LORA_PKT_ST, NULL, 0, d, sizeof(d));
    if (err == ESP_OK && !read_stat_ok()) err = ESP_ERR_INVALID_RESPONSE;
    ls_lora_hw_unlock();
    if (err != ESP_OK) return err;
    if (out) {
        /* SNR in quarter dB; the packet RSSI is byte 3 with its half-dB bit in
           bit 1 of byte 5. */
        out->snr_db = (float)(int8_t)d[2] / 4.0f;
        out->length = d[1]; out->cr = d[0] & 15;
        out->detector = (d[5] >> 2) & 15;
        const uint32_t raw = ((uint32_t)d[6] << 16) | ((uint32_t)d[7] << 8) | d[8];
        out->fei_hz = (int32_t)(raw & 0x7fffff) - (int32_t)(raw & 0x800000);
        out->rssi_dbm = -(float)(((unsigned)d[3] << 1) | ((d[5] >> 1) & 1u)) / 2.0f;
    }
    return ESP_OK;
}

esp_err_t lr20xx_get_rx_pkt_length(uint16_t *len)
{
    uint8_t d[2] = { 0, 0 };
    ls_lora_hw_lock();
    esp_err_t err = lr20xx_read(LR20XX_OP_GET_RX_PKT_LEN, NULL, 0, d, sizeof(d));
    if (err == ESP_OK && !read_stat_ok()) err = ESP_ERR_INVALID_RESPONSE;
    ls_lora_hw_unlock();
    if (err == ESP_OK && len) *len = (uint16_t)((d[0] << 8) | d[1]);
    return err;
}

/* Side detector bytes: SF[7:4], PPM[2], IQ[0]; omitted slots disable.
   Validate the receive constraints before sending anything. */
esp_err_t lr20xx_lora_side_config(uint8_t main_sf, uint32_t bw_hz,
                                 const lr20xx_side_det_t *side, size_t count)
{
    if (main_sf < 5 || main_sf > 12 || count > 3 || (count && !side) ||
        (bw_hz >= 500000 && count > (main_sf >= 10 ? 1u : 2u))) return ESP_ERR_INVALID_ARG;
    uint8_t d[3];
    for (size_t i = 0; i < count; i++) {
        if (side[i].sf <= main_sf || side[i].sf > 12 || side[i].sf - main_sf > 4 ||
            side[i].ppm > 1 || side[i].iq > 1) return ESP_ERR_INVALID_ARG;
        for (size_t j = 0; j < i; j++) if (side[i].sf == side[j].sf) return ESP_ERR_INVALID_ARG;
        d[i] = (uint8_t)((side[i].sf << 4) | (side[i].ppm << 2) | side[i].iq);
    }
    return lr20xx_write(LR20XX_OP_SET_LORA_SIDE_CFG, d, count);
}
esp_err_t lr20xx_lora_side_sync(const uint8_t *sync, size_t count)
{
    if (count > 3 || (count && !sync)) return ESP_ERR_INVALID_ARG;
    return lr20xx_write(LR20XX_OP_SET_LORA_SIDE_SYNC, sync, count);
}
esp_err_t lr20xx_zwave_params(uint8_t rate, uint8_t bw, uint8_t filter,
                             uint8_t length, uint16_t preamble, uint8_t detect, bool fifo_fcs)
{
    if (rate > 3 || filter > 2) return ESP_ERR_INVALID_ARG;
    const uint8_t d[] = {rate, bw, filter, length, preamble >> 8, (uint8_t)preamble, detect, fifo_fcs};
    return lr20xx_write(LR20XX_OP_SET_ZWAVE_PARAMS, d, sizeof(d));
}
esp_err_t lr20xx_zwave_homeid(uint32_t homeid)
{
    const uint8_t d[] = {homeid >> 24, homeid >> 16, homeid >> 8, homeid};
    return lr20xx_write(LR20XX_OP_SET_ZWAVE_HOMEID, d, sizeof(d));
}
esp_err_t lr20xx_zwave_scan_config(const lr20xx_zwave_channel_t *channels, size_t count,
                                  uint8_t filter, bool fifo_fcs)
{
    /* FRAME_MAX holds the three-channel US scan. Four channels need a larger
       command frame and are deliberately refused here. */
    if (!channels || count < 2 || count > 3 || filter > 2) return ESP_ERR_INVALID_ARG;
    uint8_t d[19] = { (uint8_t)(count << 4), 0, filter, fifo_fcs };
    for (size_t i = 0; i < count; i++) {
        if (channels[i].hz < LR20XX_FREQ_MIN_HZ || channels[i].hz > LR20XX_FSK_RX_MAX_HZ ||
            channels[i].rate > 3 || !channels[i].dwell) return ESP_ERR_INVALID_ARG;
        d[1] |= channels[i].rate << (2 * i);
        const uint32_t hz = channels[i].hz;
        d[4+5*i] = hz >> 24; d[5+5*i] = hz >> 16; d[6+5*i] = hz >> 8;
        d[7+5*i] = hz; d[8+5*i] = channels[i].dwell;
    }
    return lr20xx_write(LR20XX_OP_SET_ZWAVE_SCAN_CFG, d, 4+5*count);
}
esp_err_t lr20xx_zwave_scan(void) { return lr20xx_write(LR20XX_OP_SET_ZWAVE_SCAN, NULL, 0); }
esp_err_t lr20xx_wisun_mode(uint8_t mode, uint8_t bw)
{
    if (mode > 7) return ESP_ERR_INVALID_ARG;
    const uint8_t d[] = {mode, bw};
    return lr20xx_write(LR20XX_OP_SET_WISUN_MODE, d, sizeof(d));
}
esp_err_t lr20xx_wisun_packet(bool crc16, bool whitening, bool computed_crc,
                             uint8_t fec, uint16_t length, uint8_t preamble, uint8_t detect)
{
    if (fec > 3 || length > 2047) return ESP_ERR_INVALID_ARG;
    const uint8_t d[] = {(crc16 << 5) | (whitening << 4) | (computed_crc << 3) | fec,
                         length >> 8, (uint8_t)length, preamble, detect};
    return lr20xx_write(LR20XX_OP_SET_WISUN_PKT, d, sizeof(d));
}
esp_err_t lr20xx_engine_stats(bool wisun, lr20xx_engine_stats_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    uint8_t d[6];
    ls_lora_hw_lock();
    esp_err_t err = lr20xx_read(wisun ? LR20XX_OP_GET_WISUN_STATS : LR20XX_OP_GET_ZWAVE_STATS, NULL, 0, d, sizeof(d));
    if (err == ESP_OK && !read_stat_ok()) err = ESP_ERR_INVALID_RESPONSE;
    ls_lora_hw_unlock();
    if (err == ESP_OK) {
        out->packets = (d[0] << 8) | d[1]; out->crc_errors = (d[2] << 8) | d[3];
        out->length_errors = (d[4] << 8) | d[5];
    }
    return err;
}
esp_err_t lr20xx_engine_status(bool wisun, lr20xx_engine_packet_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    uint8_t d[8];
    ls_lora_hw_lock();
    esp_err_t err = lr20xx_read(wisun ? LR20XX_OP_GET_WISUN_STATUS : LR20XX_OP_GET_ZWAVE_STATUS, NULL, 0, d, wisun ? 8 : 7);
    if (err == ESP_OK && !read_stat_ok()) err = ESP_ERR_INVALID_RESPONSE;
    ls_lora_hw_unlock();
    if (err == ESP_OK) {
        memset(out, 0, sizeof(*out));
        const unsigned o = wisun ? 2 : 0;
        out->length = (d[o] << 8) | d[o+1];
        out->rssi_dbm = -d[o+2] - ((d[wisun ? 6 : 5] >> 2) & 1) * 0.5f;
        if (wisun) { out->phr = (d[0] << 8) | d[1]; out->sync = d[6] >> 7; }
        else { out->rate = d[4]; out->channel = d[5] >> 3; }
    }
    return err;
}

/* Table 11-4, every rung, narrowest first: each width 40 MHz / (D * M * 2^E)
   once, with the code of the smallest E where two codes give the same width,
   which is the one RadioLib's LR2021 table uses for every width it lists. */
const lr20xx_rx_bw_t lr20xx_fsk_rx_bw_table[] = {
    {    3472, 231 }, {    4167, 167 }, {    4340, 223 }, {    4464, 103 },
    {    4808,  39 }, {    5208, 159 }, {    5580,  95 }, {    5787, 215 },
    {    6010,  31 }, {    6944, 230 }, {    7440,  87 }, {    8013,  23 },
    {    8333, 166 }, {    8681, 222 }, {    8929, 102 }, {    9615,  38 },
    {   10417, 158 }, {   11161,  94 }, {   11574, 214 }, {   12019,  30 },
    {   13889, 229 }, {   14881,  86 }, {   16026,  22 }, {   16667, 165 },
    {   17361, 221 }, {   17857, 101 }, {   19231,  37 }, {   20833, 157 },
    {   22321,  93 }, {   23148, 213 }, {   24038,  29 }, {   27778, 228 },
    {   29762,  85 }, {   32051,  21 }, {   33333, 164 }, {   34722, 220 },
    {   35714, 100 }, {   38462,  36 }, {   41667, 156 }, {   44643,  92 },
    {   46296, 212 }, {   48077,  28 }, {   55556, 227 }, {   59524,  84 },
    {   64103,  20 }, {   66667, 163 }, {   69444, 219 }, {   71429,  99 },
    {   76923,  35 }, {   83333, 155 }, {   89286,  91 }, {   92593, 211 },
    {   96154,  27 }, {  111111, 226 }, {  119048,  83 }, {  128205,  19 },
    {  133333, 162 }, {  138889, 218 }, {  142857,  98 }, {  153846,  34 },
    {  166667, 154 }, {  178571,  90 }, {  185185, 210 }, {  192308,  26 },
    {  222222, 225 }, {  238095,  82 }, {  256410,  18 }, {  266667, 161 },
    {  277778, 217 }, {  285714,  97 }, {  307692,  33 }, {  333333, 153 },
    {  357143,  89 }, {  370370, 209 }, {  384615,  25 }, {  444444, 224 },
    {  476190,  81 }, {  512821,  17 }, {  533333, 160 }, {  555556, 216 },
    {  571429,  96 }, {  615385,  32 }, {  666667, 152 }, {  714286,  88 },
    {  740741, 208 }, {  769231,  24 }, {  888889, 144 }, {  952381,  80 },
    { 1025641,  16 }, { 1111111, 200 }, { 1333333, 136 }, { 1428571,  72 },
    { 1538462,   8 }, { 2222222, 192 }, { 2666667, 128 }, { 2857143,  64 },
    { 3076923,   0 },
};
const int lr20xx_fsk_rx_bw_table_n =
    (int)(sizeof(lr20xx_fsk_rx_bw_table) / sizeof(lr20xx_fsk_rx_bw_table[0]));

lr20xx_rx_bw_t lr20xx_fsk_rx_bw(uint32_t hz)
{
    for (int i = 0; i < lr20xx_fsk_rx_bw_table_n; i++)
        if (lr20xx_fsk_rx_bw_table[i].hz >= hz) return lr20xx_fsk_rx_bw_table[i];
    return lr20xx_fsk_rx_bw_table[lr20xx_fsk_rx_bw_table_n - 1];
}

void lr20xx_scan_plan(uint32_t min_hz, uint32_t max_hz, int n,
                      ls_lora_scan_plan_t *out, lr20xx_rx_bw_t *rung)
{
    if (!out) return;
    lr20xx_rx_bw_t bw = lr20xx_fsk_rx_bw(500000u);
    out->looks = 1;
    if (n > 1 && max_hz > min_hz) {
        const uint64_t span = (uint64_t)(max_hz - min_hz);
        const uint64_t gaps = (uint64_t)(n - 1);
        bw = lr20xx_fsk_rx_bw_table[lr20xx_fsk_rx_bw_table_n - 1];
        for (int i = 0; i < lr20xx_fsk_rx_bw_table_n; i++) {
            const lr20xx_rx_bw_t r = lr20xx_fsk_rx_bw_table[i];
            if (r.hz >= LR20XX_SCAN_BW_MIN_HZ && (uint64_t)r.hz * gaps >= span) { bw = r; break; }
        }
        const uint64_t covered = (uint64_t)bw.hz * gaps;
        if (covered < span) {
            uint64_t looks = (span + covered - 1) / covered;
            if (looks > LS_LORA_SCAN_LOOKS_MAX) looks = LS_LORA_SCAN_LOOKS_MAX;
            out->looks = (int)looks;
        }
    }
    out->bw_hz = bw.hz;
    out->settle_us = ls_lora_scan_settle_us(bw.hz);
    if (rung) *rung = bw;
}

esp_err_t lr20xx_gfsk_set_modulation(uint32_t bitrate_bps, uint8_t pulse_shape,
                                     uint8_t rx_bw, uint32_t deviation_hz)
{
    /* Bit 31 of the rate selects 1/256 bps units; whole bps here. */
    if (bitrate_bps == 0 || (bitrate_bps & 0x80000000u) || deviation_hz > 0xFFFFFFu)
        return ESP_ERR_INVALID_ARG;
    const uint8_t a[9] = { (uint8_t)(bitrate_bps >> 24), (uint8_t)(bitrate_bps >> 16),
                           (uint8_t)(bitrate_bps >> 8), (uint8_t)bitrate_bps,
                           pulse_shape, rx_bw, (uint8_t)(deviation_hz >> 16),
                           (uint8_t)(deviation_hz >> 8), (uint8_t)deviation_hz };
    return lr20xx_write(LR20XX_OP_SET_GFSK_MOD, a, sizeof(a));
}

esp_err_t lr20xx_gfsk_set_packet_fixed(uint16_t preamble_bits, uint8_t detect_bits,
                                       uint16_t payload_len)
{
    if (detect_bits > 32 || detect_bits % 8) return ESP_ERR_INVALID_ARG;
    /* Preamble(2), preamble detector length in bits (0 is off), then long
       preamble, length-in-bits, address filter and packet format all 0 (fixed
       length), payload length(2), and CRC type 0 with whitening 0. */
    const uint8_t a[7] = { (uint8_t)(preamble_bits >> 8), (uint8_t)preamble_bits, detect_bits, 0x00,
                           (uint8_t)(payload_len >> 8), (uint8_t)payload_len, 0x00 };
    return lr20xx_write(LR20XX_OP_SET_GFSK_PKT, a, sizeof(a));
}

esp_err_t lr20xx_gfsk_set_syncword(uint32_t sync_word, uint8_t bits)
{
    if (bits < 8 || bits > 32 || bits % 8) return ESP_ERR_INVALID_ARG;
    /* Eight sync bytes, the word right-aligned in them, then the bit order
       (bit 7, MSB first) and the length in bits. The top `bits` of sync_word
       are what an SX126x matches with the same session, so they are the ones
       placed. */
    uint8_t a[9] = { 0 };
    const uint32_t w = bits == 32 ? sync_word : sync_word >> (32 - bits);
    for (int i = 0; i < bits / 8; i++) a[7 - i] = (uint8_t)(w >> (8 * i));
    a[8] = (uint8_t)(0x80 | bits);
    return lr20xx_write(LR20XX_OP_SET_GFSK_SYNC, a, sizeof(a));
}

esp_err_t lr20xx_get_gfsk_packet_status(lr20xx_gfsk_pkt_status_t *out)
{
    uint8_t d[6] = { 0 };
    ls_lora_hw_lock();
    esp_err_t err = lr20xx_read(LR20XX_OP_GET_GFSK_PKT_ST, NULL, 0, d, sizeof(d));
    if (err == ESP_OK && !read_stat_ok()) err = ESP_ERR_INVALID_RESPONSE;
    ls_lora_hw_unlock();
    if (err != ESP_OK) return err;
    if (out) {
        /* pkt_len(2), RssiAvg(8:1), RssiSync(8:1), then a byte with
           RssiAvg(0) in bit 2 and RssiSync(0) in bit 0. */
        out->pkt_len = (uint16_t)((d[0] << 8) | d[1]);
        out->rssi_avg_dbm  = -(float)(((unsigned)d[2] << 1) | ((d[4] >> 2) & 1u)) / 2.0f;
        out->rssi_sync_dbm = -(float)(((unsigned)d[3] << 1) | (d[4] & 1u)) / 2.0f;
    }
    return ESP_OK;
}

esp_err_t lr20xx_get_gfsk_rx_stats(lr20xx_gfsk_rx_stats_t *out)
{
    uint8_t d[14] = { 0 };
    ls_lora_hw_lock();
    esp_err_t err = lr20xx_read(LR20XX_OP_GET_GFSK_RX_STATS, NULL, 0, d, sizeof(d));
    if (err == ESP_OK && !read_stat_ok()) err = ESP_ERR_INVALID_RESPONSE;
    ls_lora_hw_unlock();
    if (err != ESP_OK) return err;
    if (out) {
        uint16_t *f[7] = { &out->received, &out->crc_errors, &out->length_errors,
                           &out->preamble_detections, &out->sync_ok, &out->sync_fail,
                           &out->timeouts };
        for (int i = 0; i < 7; i++) *f[i] = (uint16_t)((d[2 * i] << 8) | d[2 * i + 1]);
    }
    return ESP_OK;
}

esp_err_t lr20xx_write_reg32(uint32_t addr, uint32_t data)
{
    if (addr > 0xFFFFFFu) return ESP_ERR_INVALID_ARG;
    const uint8_t a[7] = { (uint8_t)(addr >> 16), (uint8_t)(addr >> 8), (uint8_t)addr,
                           (uint8_t)(data >> 24), (uint8_t)(data >> 16), (uint8_t)(data >> 8),
                           (uint8_t)data };
    return lr20xx_write(LR20XX_OP_WRITE_REG32, a, sizeof(a));
}

/* The registers and values of RadioLib's LR2021 DC-DC workaround. */
#define REG_DCDC_SWITCHER   0xF20024u
#define REG_DCDC_ADC_CTRL   0xF40200u
#define REG_DCDC_FREQ_LF    0x80004Cu
#define DCDC_FREQ_LF_2M8    2936012u      /* 2.8 MHz x 1.048576 */
#define DCDC_FREQ_LF_4M3    4508876u      /* 4.3 MHz x 1.048576 */

static esp_err_t dcdc_switcher(uint32_t a, uint32_t b)
{
    esp_err_t err = lr20xx_write_reg_mask32(REG_DCDC_SWITCHER, 0x0Fu << 20, a << 20);
    if (err == ESP_OK) err = lr20xx_check_last_ok();
    if (err == ESP_OK) err = lr20xx_write_reg_mask32(REG_DCDC_SWITCHER, 0x0Fu << 16, b << 16);
    if (err == ESP_OK) err = lr20xx_check_last_ok();
    return err;
}

/* Both end by setting the frequency again, as RadioLib's do. */
static esp_err_t dcdc_finish(uint32_t freq_lf)
{
    esp_err_t err = lr20xx_write_reg32(REG_DCDC_FREQ_LF, freq_lf);
    if (err == ESP_OK) err = lr20xx_check_last_ok();
    if (err == ESP_OK && s_rf_hz) err = lr20xx_set_rf_frequency(s_rf_hz);
    if (err == ESP_OK) err = lr20xx_check_last_ok();
    return err;
}

static esp_err_t dcdc_set_locked(void)
{
    uint32_t adc = 0;
    esp_err_t err = lr20xx_read_reg32(REG_DCDC_ADC_CTRL, &adc);
    const uint32_t ana_dec = (adc >> 8) & 7u;
    /* The decimation the modulation chose. On the LF input 1 and 2 take the
       slower switcher settings; the HF input, where only receive-only
       sessions and sweeps go, always takes the plain ones, as RadioLib's
       workaround does above its 1500 MHz cut-off. */
    const bool hf = s_rf_hz >= LR20XX_HF_PATH_MIN_HZ;
    if (err == ESP_OK)
        err = (!hf && (ana_dec == 1 || ana_dec == 2)) ? dcdc_switcher(11, 13) : dcdc_switcher(15, 15);
    if (err == ESP_OK) err = dcdc_finish(ana_dec == 1 ? DCDC_FREQ_LF_4M3 : DCDC_FREQ_LF_2M8);
    return err;
}

static esp_err_t dcdc_reset_locked(void)
{
    esp_err_t err = dcdc_switcher(15, 15);
    if (err == ESP_OK) err = dcdc_finish(DCDC_FREQ_LF_2M8);
    return err;
}

esp_err_t lr20xx_dcdc_workaround_set(void)
{
    LOCKED_RET(esp_err_t, dcdc_set_locked());
}

esp_err_t lr20xx_dcdc_workaround_reset(void)
{
    LOCKED_RET(esp_err_t, dcdc_reset_locked());
}

/* RadioLib's LR2021 LF table, one entry a dB from -9 dBm: PA duty cycle and
   slices for SetPaConfig, and the power byte for SetTxParams. */
static const lr20xx_pa_lf_t PA_LF[] = {
    { 1, 1,  8 }, { 2, 2,  1 }, { 2, 2,  3 }, { 2, 2,  5 }, { 1, 2, 13 }, { 2, 1, 13 },
    { 2, 2, 11 }, { 2, 2, 13 }, { 3, 1, 12 }, { 1, 1, 18 }, { 1, 1, 20 }, { 1, 1, 23 },
    { 1, 1, 27 }, { 1, 1, 33 }, { 1, 2, 26 }, { 1, 2, 31 }, { 1, 3, 27 }, { 1, 1, 37 },
    { 1, 2, 40 }, { 2, 1, 38 }, { 2, 2, 39 }, { 2, 4, 40 }, { 2, 7, 41 }, { 3, 2, 39 },
    { 3, 3, 39 }, { 3, 6, 38 }, { 4, 3, 37 }, { 4, 5, 37 }, { 4, 7, 38 }, { 5, 3, 37 },
    { 5, 6, 37 }, { 6, 7, 35 },
};

lr20xx_pa_lf_t lr20xx_pa_lf_for_dbm(int dbm)
{
    if (dbm < LR20XX_TX_POWER_MIN_DBM) dbm = LR20XX_TX_POWER_MIN_DBM;
    if (dbm > LR20XX_TX_POWER_MAX_DBM) dbm = LR20XX_TX_POWER_MAX_DBM;
    return PA_LF[dbm - LR20XX_TX_POWER_MIN_DBM];
}

bool lr20xx_tx_allowed(void)
{
    return s_rf_hz >= LR20XX_FREQ_MIN_HZ && s_rf_hz < LR20XX_TX_FREQ_LIMIT_HZ;
}

static esp_err_t set_pa_lf_locked(int dbm)
{
    s_pa_lf = false;
    if (!lr20xx_tx_allowed()) return ESP_ERR_NOT_SUPPORTED;
    const lr20xx_pa_lf_t pa = lr20xx_pa_lf_for_dbm(dbm);
    /* SelPa 0 is the LF PA. SetPaConfig: PaSel 0 (LF) in bit 7 with the LF
       mode (0, full single-ended) below it; LF duty cycle and slices; and the
       HF duty cycle at 0x10, RadioLib's "HF PA not used". */
    const uint8_t sel = 0x00;
    const uint8_t cfg[3] = { 0x00, (uint8_t)((pa.duty << 4) | pa.slices), 0x10 };
    const uint8_t txp[2] = { (uint8_t)pa.power, LR20XX_TX_RAMP_48US };
    esp_err_t err = lr20xx_write(LR20XX_OP_SEL_PA, &sel, 1);
    if (err == ESP_OK) err = lr20xx_check_last_ok();
    if (err == ESP_OK) err = lr20xx_write(LR20XX_OP_SET_PA_CONFIG, cfg, sizeof(cfg));
    if (err == ESP_OK) err = lr20xx_check_last_ok();
    if (err == ESP_OK) err = lr20xx_write(LR20XX_OP_SET_TX_PARAMS, txp, sizeof(txp));
    if (err == ESP_OK) err = lr20xx_check_last_ok();
    if (err == ESP_OK) s_pa_lf = true;
    return err;
}

esp_err_t lr20xx_set_pa_lf(int dbm)
{
    LOCKED_RET(esp_err_t, set_pa_lf_locked(dbm));
}

esp_err_t lr20xx_clear_tx_fifo(void)
{
    return lr20xx_write(LR20XX_OP_CLEAR_TX_FIFO, NULL, 0);
}

static esp_err_t write_tx_fifo_locked(const uint8_t *data, size_t len)
{
    uint8_t *pkt = ls_lora_hw_pkt();
    if (!pkt) return ESP_ERR_INVALID_STATE;
    if (!data || len == 0) return ESP_ERR_INVALID_ARG;
    if (len > 255) return ESP_ERR_INVALID_SIZE;
    if (!ls_lora_hw_wait_not_busy(20)) {
        ESP_LOGW(TAG, "busy stuck high before the FIFO write");
        s_fail_run++;
        return ESP_ERR_TIMEOUT;
    }
    pkt[0] = (uint8_t)(LR20XX_OP_WRITE_TX_FIFO >> 8);
    pkt[1] = (uint8_t)LR20XX_OP_WRITE_TX_FIFO;
    memcpy(&pkt[2], data, len);
    return frame(pkt, 2 + len);
}

/* The packet buffer is the one DMA buffer, held from the fill to the frame. */
esp_err_t lr20xx_write_tx_fifo(const uint8_t *data, size_t len)
{
    LOCKED_RET(esp_err_t, write_tx_fifo_locked(data, len));
}

static esp_err_t set_tx_locked(void)
{
    if (!lr20xx_tx_allowed() || !s_pa_lf) {
        ESP_LOGE(TAG, "transmit refused at %lu Hz%s", (unsigned long)s_rf_hz,
                 s_pa_lf ? "" : " (LF PA not programmed)");
        return ESP_ERR_NOT_SUPPORTED;
    }
    const uint8_t a[3] = { 0, 0, 0 };
    return lr20xx_write(LR20XX_OP_SET_TX, a, sizeof(a));
}

esp_err_t lr20xx_set_tx(void)
{
    LOCKED_RET(esp_err_t, set_tx_locked());
}

/* ------------------------------------------------- Mode S session -------- */

/* Defined with the packet paths' state below. After a sequence the part refused:
   whether to run it again, once after a pause and once behind a reset of the
   part. `n` counts the goes so far. */
static bool retry_gate(unsigned n, esp_err_t err, const char *what);
/* A part that has stopped taking commands is reset before a poll uses it. */
static void heal(void);

#define MODES_FRAME   LR20XX_MODES_FRAME_BYTES
#define MODES_QMAX    9                     /* whole Mode S frames in the 256-byte FIFO */
#define MODES_STALE_POLLS 3

static struct {
    bool     active;
    /* The OOK session rather than Mode S: the same machinery, its numbers
       from lr20xx_ook_rx_begin rather than the Mode S constants. */
    bool     generic;
    uint32_t freq_hz;
    int      gain;
    uint32_t bitrate;
    uint16_t pattern;
    uint8_t  pattern_bits, repeats;
    uint16_t frame;                         /* bytes a frame                     */
    uint8_t  q[MODES_QMAX * MODES_FRAME];   /* frames read, not yet handed out   */
    uint8_t  q_n, q_pos;
    float    q_rssi;                        /* level of the last frame in q      */
    uint16_t leftover;                      /* FIFO bytes beyond whole frames    */
    uint8_t  stale;
    /* A GetStatus that is not the session's own (the console's) cleared the
       reset source this poll would have seen. It saw it instead, and says so. */
    bool     reset_seen;
    /* What the console may change while the session runs. begin() sets the
       defaults; configure_session() programs whatever is here, so a chip reset
       is recovered with the same tuning. */
    int      boost;
    uint32_t bw_hz;
    uint8_t  bw_index;
    bool     thresh_set;
    int      thresh_level;
    lr20xx_modes_stats_t stats;
} ms;

bool lr20xx_modes_active(void) { return ms.active; }

void lr20xx_modes_stats(lr20xx_modes_stats_t *out)
{
    if (out) *out = ms.stats;
}

/* The antenna switch, on RF1 unless the user has chosen MMCX1. */
static void select_antenna(void)
{
#if defined(LS_BOARD_XL_RF_SW_VCTL) && defined(LS_BOARD_LR20XX_RF1_VCTL_LEVEL)
    if (ls_board_hw_antenna_is_external()) {
        ESP_LOGI(TAG, "antenna: MMCX1 selected in settings, RF1 not forced");
        return;
    }
    const esp_err_t err = ls_xl9535_out(LS_BOARD_XL_RF_SW_VCTL, LS_BOARD_LR20XX_RF1_VCTL_LEVEL);
    if (err != ESP_OK) ESP_LOGW(TAG, "antenna switch write failed: %s", esp_err_to_name(err));
#endif
}

static esp_err_t step_ok(esp_err_t err, const char *what)
{
    if (err == ESP_OK) err = lr20xx_check_last_ok();
    if (err != ESP_OK) ESP_LOGE(TAG, "modes: %s failed: %s", what, esp_err_to_name(err));
    return err;
}

static esp_err_t check_mode(unsigned want)
{
    lr20xx_status_t st;
    esp_err_t err = lr20xx_get_status(&st);
    if (err != ESP_OK) return err;
    if (!lr20xx_stat_plausible(st.stat) || LR20XX_STAT_CMD(st.stat) != LR20XX_CMD_OK ||
        LR20XX_STAT_MODE(st.stat) != want) {
        ESP_LOGE(TAG, "modes: expected chip mode %u, stat 0x%04X", want, (unsigned)st.stat);
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

static esp_err_t arm_rx(void)
{
    ms.q_n = ms.q_pos = 0;
    ms.leftover = 0;
    ms.stale = 0;
    esp_err_t err = step_ok(lr20xx_clear_rx_fifo(), "ClearRxFifo");
    if (err == ESP_OK) err = step_ok(lr20xx_clear_irq(LR20XX_IRQ_ALL), "ClearIrq");
    if (err == ESP_OK) err = lr20xx_set_rx(0xFFFFFFu);
    if (err == ESP_OK) err = check_mode(LR20XX_MODE_RX);
    return err;
}

/* The detection threshold override, when there is one. It follows
   SetOokModulationParams, which is where the chip derives its own from the
   bandwidth (22.1.2), so it is applied after every one. No frame otherwise. */
static esp_err_t apply_threshold(void)
{
    if (!ms.thresh_set) return ESP_OK;
    const uint32_t raw = (uint32_t)(ms.thresh_level + LR20XX_OOK_DET_THRESHOLD_BIAS);
    return step_ok(lr20xx_write_reg_mask32(LR20XX_REG_OOK_DET_THRESHOLD, LR20XX_OOK_DET_THRESHOLD_MASK,
                                           raw << 20),
                   "OOK detection threshold");
}

/* Everything from standby to receive. */
static esp_err_t configure_session_once(uint32_t freq_hz, int gain)
{
    esp_err_t err = step_ok(lr20xx_set_standby(false), "SetStandby");
    if (err != ESP_OK) return err;
    select_antenna();
    err = lr20xx_configure_dios();
    if (err != ESP_OK) return err;
    err = step_ok(lr20xx_set_fallback_standby_rc(), "SetRxTxFallbackMode");
    if (err == ESP_OK) err = step_ok(lr20xx_clear_irq(LR20XX_IRQ_ALL), "ClearIrq");
    /* The start-up calibration was at 915 MHz (6.4); the PLL and the
       anti-aliasing filter want redoing after a move of more than 50 MHz
       (6.4.1). The power amplifier block is not asked for. */
    if (err == ESP_OK)
        err = lr20xx_calibrate(LR20XX_CAL_LF_RC | LR20XX_CAL_HF_RC | LR20XX_CAL_PLL |
                               LR20XX_CAL_AAF | LR20XX_CAL_MU);
    if (err == ESP_OK) err = lr20xx_wait_ready(200);
    if (err == ESP_OK) err = step_ok(ESP_OK, "Calibrate");
    if (err == ESP_OK) err = step_ok(lr20xx_set_packet_type_ook(), "SetPacketType");
    if (err == ESP_OK) err = lr20xx_tune_rx(freq_hz, ms.boost);
    if (err == ESP_OK)
        err = step_ok(lr20xx_ook_set_modulation(ms.bitrate, 0, ms.bw_index, 0),
                      "SetOokModulationParams");
    if (err == ESP_OK) err = apply_threshold();
    if (err == ESP_OK)
        err = step_ok(lr20xx_ook_set_packet_fixed(16, ms.frame, 0), "SetOokPacketParams");
    if (err == ESP_OK) err = step_ok(lr20xx_ook_set_sync_none(), "SetOokSyncword");
    if (err == ESP_OK)
        err = step_ok(lr20xx_ook_set_detector(ms.pattern, ms.pattern_bits, ms.repeats),
                      "SetOokDetector");
    if (err == ESP_OK) err = step_ok(lr20xx_set_agc_gain((uint8_t)gain), "SetAgcGainManual");
    if (err == ESP_OK) err = arm_rx();
    return err;
}

static esp_err_t configure_session(uint32_t freq_hz, int gain)
{
    esp_err_t err;
    unsigned n = 0;
    do err = configure_session_once(freq_hz, gain); while (retry_gate(n++, err, "Mode S setup"));
    if (err == ESP_OK) s_reset_seen = false;      /* all of it was just programmed */
    return err;
}

esp_err_t lr20xx_modes_begin(uint32_t freq_hz, int gain_step)
{
    if (!s_bound) return ESP_ERR_INVALID_STATE;
    if (ms.active) return ESP_ERR_INVALID_STATE;
    if (freq_hz < LR20XX_MODES_FREQ_MIN_HZ || freq_hz > LR20XX_MODES_FREQ_MAX_HZ ||
        gain_step < 0 || gain_step > LR20XX_MODES_GAIN_MAX)
        return ESP_ERR_INVALID_ARG;
    if (!ls_lora_hw_pkt()) return ESP_ERR_INVALID_STATE;

    memset(&ms, 0, sizeof(ms));
    ms.freq_hz = freq_hz;
    ms.gain = gain_step;
    ms.boost = LR20XX_MODES_RX_BOOST;
    ms.bitrate = LR20XX_MODES_BITRATE_BPS;
    ms.pattern = LR20XX_MODES_PATTERN;
    ms.pattern_bits = LR20XX_MODES_PATTERN_BITS;
    ms.frame = MODES_FRAME;
    {
        const lr20xx_rx_bw_t bw = lr20xx_wide_rx_bw(LR20XX_MODES_RX_BW_HZ);
        ms.bw_hz = bw.hz;
        ms.bw_index = bw.index;
    }
    esp_err_t err = configure_session(freq_hz, gain_step);
    if (err != ESP_OK) {
        (void)lr20xx_set_standby(false);      /* leave it quiet */
        return err;
    }
    ms.active = true;
    ESP_LOGI(TAG, "Mode S session: %lu Hz, gain step %d", (unsigned long)freq_hz, gain_step);
    return ESP_OK;
}

esp_err_t lr20xx_ook_rx_begin(const lr20xx_ook_rx_cfg_t *cfg)
{
    if (!s_bound) return ESP_ERR_INVALID_STATE;
    if (ms.active) return ESP_ERR_INVALID_STATE;
    if (!cfg || cfg->freq_hz < LR20XX_FREQ_MIN_HZ || cfg->freq_hz > LR20XX_OOK_RX_MAX_HZ ||
        cfg->bitrate_bps < LR20XX_OOK_BITRATE_MIN || cfg->bitrate_bps > LR20XX_FSK_BITRATE_MAX ||
        !cfg->rx_bw_hz || cfg->rx_bw_hz > lr20xx_fsk_rx_bw_table[lr20xx_fsk_rx_bw_table_n - 1].hz ||
        cfg->pattern_bits < 1 || cfg->pattern_bits > 16 || cfg->repeats > 31 ||
        cfg->frame_bytes < 1 || cfg->frame_bytes > LR20XX_OOK_FRAME_MAX ||
        cfg->gain_step < 0 || cfg->gain_step > LR20XX_MODES_GAIN_MAX ||
        cfg->boost < -1 || cfg->boost > 7)
        return ESP_ERR_INVALID_ARG;
    if (!ls_lora_hw_pkt()) return ESP_ERR_INVALID_STATE;

    memset(&ms, 0, sizeof(ms));
    ms.generic = true;
    ms.freq_hz = cfg->freq_hz;
    ms.gain = cfg->gain_step;
    ms.boost = cfg->boost >= 0 ? cfg->boost
                               : (lr20xx_path_for_hz(cfg->freq_hz) == LR20XX_PATH_HF ? 4 : 0);
    ms.bitrate = cfg->bitrate_bps;
    ms.pattern = cfg->pattern;
    ms.pattern_bits = cfg->pattern_bits;
    ms.repeats = cfg->repeats;
    ms.frame = cfg->frame_bytes;
    {
        const lr20xx_rx_bw_t bw = lr20xx_fsk_rx_bw(cfg->rx_bw_hz);
        ms.bw_hz = bw.hz;
        ms.bw_index = bw.index;
    }
    esp_err_t err = configure_session(ms.freq_hz, ms.gain);
    if (err != ESP_OK) {
        (void)lr20xx_set_standby(false);      /* leave it quiet */
        ms.generic = false;
        return err;
    }
    ms.active = true;
    ESP_LOGI(TAG, "OOK session: %lu Hz, %lu bps, filter %lu Hz, %u-byte frames",
             (unsigned long)ms.freq_hz, (unsigned long)ms.bitrate, (unsigned long)ms.bw_hz,
             (unsigned)ms.frame);
    return ESP_OK;
}

bool lr20xx_ook_rx_active(void) { return ms.active && ms.generic; }

esp_err_t lr20xx_modes_set_gain(int gain_step)
{
    if (gain_step < 0 || gain_step > LR20XX_MODES_GAIN_MAX) return ESP_ERR_INVALID_ARG;
    if (!ms.active) return ESP_ERR_INVALID_STATE;
    if (gain_step == ms.gain) return ESP_OK;
    /* The step is written in standby, where RadioLib's ADS-B setup writes it,
       and Rx is armed again. */
    esp_err_t err = step_ok(lr20xx_set_standby(false), "SetStandby");
    if (err == ESP_OK) err = step_ok(lr20xx_set_agc_gain((uint8_t)gain_step), "SetAgcGainManual");
    if (err == ESP_OK) err = arm_rx();
    if (err == ESP_OK) ms.gain = gain_step;
    return err;
}

/* The same shape for every knob: standby, the one command, Rx again. Standby
   first because the part refuses these in Rx (5.1.1, 8.1.1) and because the
   pump's poll must never find it half-programmed; the SPI lock the dispatcher
   holds keeps the pump out for the whole of it. */
static esp_err_t retune_enter(void)
{
    if (!ms.active) return ESP_ERR_INVALID_STATE;
    return step_ok(lr20xx_set_standby(false), "SetStandby");
}

esp_err_t lr20xx_modes_set_boost(int boost)
{
    if (boost < 0 || boost > 7) return ESP_ERR_INVALID_ARG;
    if (!ms.active) return ESP_ERR_INVALID_STATE;
    if (boost == ms.boost) return ESP_OK;
    esp_err_t err = retune_enter();
    if (err == ESP_OK)
        err = step_ok(lr20xx_set_rx_path(lr20xx_path_for_hz(ms.freq_hz), (uint8_t)boost), "SetRxPath");
    if (err == ESP_OK) { ms.boost = boost; err = arm_rx(); }
    return err;
}

esp_err_t lr20xx_modes_set_rx_bw(uint32_t hz, uint32_t *chosen_hz)
{
    if (!ms.active) return ESP_ERR_INVALID_STATE;
    const lr20xx_rx_bw_t bw = lr20xx_ook_rx_bw_nearest(hz);
    if (chosen_hz) *chosen_hz = bw.hz;
    if (bw.index == ms.bw_index) return ESP_OK;
    esp_err_t err = retune_enter();
    if (err == ESP_OK)
        err = step_ok(lr20xx_ook_set_modulation(ms.bitrate, 0, bw.index, 0),
                      "SetOokModulationParams");
    if (err == ESP_OK) { ms.bw_hz = bw.hz; ms.bw_index = bw.index; err = apply_threshold(); }
    if (err == ESP_OK) err = arm_rx();
    return err;
}

esp_err_t lr20xx_modes_set_threshold(int level_db)
{
    if (level_db < LR20XX_OOK_DET_LEVEL_MIN || level_db > LR20XX_OOK_DET_LEVEL_MAX)
        return ESP_ERR_INVALID_ARG;
    if (!ms.active) return ESP_ERR_INVALID_STATE;
    if (ms.thresh_set && ms.thresh_level == level_db) return ESP_OK;
    esp_err_t err = retune_enter();
    if (err == ESP_OK) {
        ms.thresh_set = true;
        ms.thresh_level = level_db;
        err = apply_threshold();
    }
    if (err == ESP_OK) err = arm_rx();
    return err;
}

esp_err_t lr20xx_modes_clear_threshold(void)
{
    if (!ms.active) return ESP_ERR_INVALID_STATE;
    if (!ms.thresh_set) return ESP_OK;
    ms.thresh_set = false;
    esp_err_t err = retune_enter();
    if (err == ESP_OK)
        err = step_ok(lr20xx_ook_set_modulation(ms.bitrate, 0, ms.bw_index, 0),
                      "SetOokModulationParams");
    if (err == ESP_OK) err = arm_rx();
    return err;
}

esp_err_t lr20xx_modes_tuning(lr20xx_modes_tuning_t *out)
{
    if (!ms.active) return ESP_ERR_INVALID_STATE;
    if (out) {
        out->gain_step = ms.gain;
        out->boost = ms.boost;
        out->rx_bw_hz = ms.bw_hz;
        out->thresh_override = ms.thresh_set;
        out->thresh_level = ms.thresh_level;
    }
    return ESP_OK;
}

esp_err_t lr20xx_modes_read_threshold(int *raw)
{
    if (!ms.active) return ESP_ERR_INVALID_STATE;
    uint32_t v = 0;
    const esp_err_t err = lr20xx_read_reg32(LR20XX_REG_OOK_DET_THRESHOLD, &v);
    if (err == ESP_OK && raw) *raw = (int)((v & LR20XX_OOK_DET_THRESHOLD_MASK) >> 20);
    return err;
}

esp_err_t lr20xx_modes_end(void)
{
    if (!ms.active) return ESP_OK;
    esp_err_t first = step_ok(lr20xx_set_standby(false), "SetStandby");
    esp_err_t e = lr20xx_clear_irq(LR20XX_IRQ_ALL);
    if (first == ESP_OK) first = e;
    e = lr20xx_clear_rx_fifo();
    if (first == ESP_OK) first = e;
    ms.active = false;
    ms.generic = false;
    ms.q_n = ms.q_pos = 0;
    return first;
}

static int deliver(uint8_t *buf, float *rssi_dbm)
{
    memcpy(buf, &ms.q[(size_t)ms.q_pos * ms.frame], ms.frame);
    ms.q_pos++;
    if (rssi_dbm) *rssi_dbm = (ms.q_pos == ms.q_n) ? ms.q_rssi : NAN;
    if (ms.q_pos == ms.q_n) ms.q_n = ms.q_pos = 0;
    ms.stats.frames++;
    return ms.frame;
}

/* Read what the FIFO holds. `rxdone` says a packet finished; without it this
   is only a look at bytes left over from before. */
static esp_err_t drain_fifo(bool rxdone)
{
    uint16_t level = 0;
    esp_err_t err = lr20xx_get_rx_fifo_level(&level);
    if (err != ESP_OK) return err;

    const unsigned whole = level / ms.frame;
    if (whole == 0) {
        if (!rxdone && level && level == ms.leftover) {
            /* The same few bytes with nothing arriving: half a packet from
               before an overflow. They shift every frame after them. */
            if (++ms.stale >= MODES_STALE_POLLS) {
                ms.stats.overflows++;
                ms.leftover = 0;
                ms.stale = 0;
                return lr20xx_clear_rx_fifo();
            }
        } else {
            ms.stale = 0;
        }
        ms.leftover = level;
        return ESP_OK;
    }

    const unsigned qmax = (unsigned)sizeof(ms.q) / ms.frame;
    unsigned n = whole > qmax ? qmax : whole;
    err = lr20xx_read_rx_fifo(ms.q, (size_t)n * ms.frame);
    if (err != ESP_OK) return err;
    ms.q_n = (uint8_t)n;
    ms.q_pos = 0;
    ms.leftover = (uint16_t)(level - n * ms.frame);
    ms.stale = 0;

    /* A full FIFO has lost its tail, so what is left cannot be trusted to be
       aligned to a frame. */
    if (level >= 256) {
        ms.stats.overflows++;
        ms.leftover = 0;
        err = lr20xx_clear_rx_fifo();
        if (err != ESP_OK) return err;
    }

    lr20xx_ook_pkt_status_t ps;
    ms.q_rssi = NAN;
    if (lr20xx_get_ook_packet_status(&ps) == ESP_OK) ms.q_rssi = ps.rssi_high_dbm;
    return ESP_OK;
}

static void note_chip_errors(void)
{
    uint16_t errors = 0;
    if (lr20xx_get_errors(&errors) == ESP_OK) {
        ms.stats.chip_errors++;
        ESP_LOGW(TAG, "modes: chip errors 0x%04X", (unsigned)errors);
        (void)lr20xx_clear_errors();
    }
}

/* Rx armed again after the part was seen out of it. It is asked once more
   first: the part comes back to Rx by itself after some packets, and SetRx and
   ClearRxFifo are refused in Rx, which an arm taken from the earlier reading
   did every time at a high capture rate. */
static esp_err_t rearm_rx(void)
{
    lr20xx_status_t st;
    if (lr20xx_get_status(&st) == ESP_OK && lr20xx_stat_plausible(st.stat) &&
        LR20XX_STAT_MODE(st.stat) == LR20XX_MODE_RX && LR20XX_STAT_RESET_SRC(st.stat) == 0)
        return ESP_OK;
    return arm_rx();
}

int lr20xx_modes_poll(uint8_t *buf, size_t size, float *rssi_dbm)
{
    if (!ms.active || !buf || size < ms.frame) return -1;
    if (ms.q_pos < ms.q_n) return deliver(buf, rssi_dbm);
    heal();

    lr20xx_status_t st;
    if (lr20xx_get_status(&st) != ESP_OK || !lr20xx_stat_plausible(st.stat)) {
        ms.stats.bus_errors++;
        return -2;
    }
    ms.stats.polls++;
    const unsigned mode = LR20XX_STAT_MODE(st.stat);

    /* A reset source in Stat means the part restarted under us (GetStatus
       cleared it when the session began). Everything it knew is gone. */
    if (take_reset(st.stat) || ms.reset_seen) {
        ms.reset_seen = false;
        ms.stats.resets++;
        ESP_LOGW(TAG, "modes: chip reset (stat 0x%04X), reprogramming", (unsigned)st.stat);
        if (configure_session(ms.freq_hz, ms.gain) != ESP_OK) {
            ms.stats.bus_errors++;
            return -2;
        }
        return 0;
    }

    const uint32_t irq = st.irq;
    const bool rxdone = (irq & LR20XX_IRQ_RX_DONE) != 0;
    if (irq & (LR20XX_IRQ_ERROR | LR20XX_IRQ_CMD_ERROR)) note_chip_errors();
    /* Cleared before the FIFO is read, so a packet that finishes while it is
       read raises RxDone again rather than being lost with this one. */
    if (irq & MODES_IRQ_MASK) {
        if (lr20xx_clear_irq(irq & MODES_IRQ_MASK) != ESP_OK) {
            ms.stats.bus_errors++;
            return -2;
        }
    }

    if (rxdone || ms.leftover) {
        if (drain_fifo(rxdone) != ESP_OK) {
            /* RxDone is already cleared; marking the FIFO as not known to be
               empty makes the next poll look again instead of leaving the
               frame that raised it behind. */
            if (!ms.leftover) ms.leftover = 1;
            ms.stats.bus_errors++;
            return -2;
        }
    }

    /* A timeout, an error or a command the part refused can leave it out of
       Rx. Rx is armed again, from an empty FIFO, unless a frame is waiting. */
    if (mode != LR20XX_MODE_RX && ms.q_pos >= ms.q_n) {
        ms.stats.rearms++;
        if (rearm_rx() != ESP_OK) {
            ms.stats.bus_errors++;
            return -2;
        }
        return 0;
    }

    if (ms.q_pos < ms.q_n) return deliver(buf, rssi_dbm);
    return 0;
}

/* ---------------------------------------------------- ls_lora backend ---- */

static esp_err_t nsup(void) { return ESP_ERR_NOT_SUPPORTED; }

static uint32_t lr_caps(void)
{
    /* Implemented: LoRa packets and FSK sessions (both transmitting on the LF
       path only), instantaneous RSSI, and the receive-only Mode S session.
       The two silicon bits are facts about the part (3 MHz OOK/FSK filter; the
       HF input is LR2021/LR2022 only), not a promise of more than the
       receive-only sweep and FSK listener there that LS_LORA_CAP_RX_WIDE
       names. */
    uint32_t c = LS_LORA_CAP_LORA | LS_LORA_CAP_FSK | LS_LORA_CAP_RSSI_INST |
                 LS_LORA_CAP_WIDE_RX_BW | LS_LORA_CAP_MODES_RX | LS_LORA_CAP_RX_WIDE |
                 LS_LORA_CAP_FSK_STREAM | LS_LORA_CAP_FSK_DETECT;
    if (s_info.lr2021) c |= LS_LORA_CAP_BAND_1G5_2G5;
    return c;
}

static const char *lr_name(void)
{
    return s_info.lr2021 ? "LR2021" : "LR2012/LR2022";
}

static esp_err_t lr_status(uint8_t *out)
{
    lr20xx_status_t st;
    esp_err_t err = lr20xx_get_status(&st);
    if (err == ESP_OK && out) *out = (uint8_t)st.stat;
    return err;
}

static esp_err_t lr_read_reg(uint16_t a, uint8_t *b, size_t l)
{ (void)a; (void)b; (void)l; return nsup(); }

/* ------------------------------------- LoRa packets, FSK, the sweep ----- */

/* What the packet paths read on the IRQ word: the Mode S set, TxDone, and the
   LoRa header error. The same mask goes on the IRQ DIO. */
#define PKT_IRQ_MASK (MODES_IRQ_MASK | LR20XX_IRQ_TX_DONE | LR20XX_IRQ_LORA_HDR_ERROR)

/* The one transmit in a thousand that never raises TxDone must not wedge the
   sender for good. The longest legal frame here is well under 5 s. */
#define TX_TIMEOUT_MS        8000
/* How long one ls_lora_scan_pass may run before it hands back. */
#define SCAN_PASS_BUDGET_US  20000
/* RadioLib's front-end calibration policy: once the part has moved 20 MHz
   from where CalibFE last ran, it runs again. */
#define CALIB_FE_STEP_HZ     20000000u

/* The packet paths' state. PSRAM: no DMA reaches it, and internal RAM is the
   one thing this board is short of. */
static EXT_RAM_BSS_ATTR struct {
    ls_lora_cfg_t cfg;            /* as configured, bandwidth as set        */
    uint8_t  bw_code;
    bool     cfg_valid;
    /* The part holds cfg. A Mode S session, an FSK session, a sweep or a chip
       reset clears it, and the next receive or send programs the part again
       in full: packet type, modulation, everything. */
    bool     programmed;
    bool     rx_mode;
    /* A receive that could not be armed, tried again by every poll, as the
       SX126x backend does. */
    bool     rearm;
    bool     tx_busy;
    int64_t  tx_deadline;
    uint32_t cal_hz;              /* where CalibFE last ran                 */
    /* Whether LoRa was receiving when an FSK session, a sweep or a Mode S
       session took the part; only one of them can hold it at a time. */
    bool     restore_rx;

    bool     fsk_active;
    /* The session as it was asked for, and whether the part has lost it (a
       restart, or a reset the driver did): the next poll programs it again. */
    ls_fsk_cfg_t fsk_cfg;
    bool     fsk_stale;
    /* The receive that could not be armed is tried again after this, not by
       every poll: a part refusing commands was sent a dozen of them every 20 ms. */
    int64_t  rearm_after_us;
    uint32_t fsk_freq_hz;
    uint8_t  fsk_bytes;
    bool     fsk_rssi_at_sync;
    uint16_t fsk_pre_bits;
    uint8_t  fsk_sync_bits;
    uint8_t  fsk_detect_bits;
    int8_t   fsk_power;
    /* A stream session: the bytes taken from the packet now arriving, and
       whether the next ones read do not follow the last (a new packet, or
       bytes lost to a full buffer). */
    bool     fsk_stream;
    bool     fsk_stream_fresh;
    uint16_t fsk_stream_taken;

    bool     scanning;
    uint32_t scan_min_hz, scan_max_hz;
    ls_lora_scan_plan_t scan_plan;
    int      scan_plan_n, scan_look, scan_bin;
    ls_lora_scan_prof_t prof;
} lr;

static esp_err_t lr_fsk_end(void);
static esp_err_t lr_scan_end(void);
static uint32_t  lr_airtime_ms(int len);

/* ---- a part that stops taking commands --------------------------------- */

/* The same command sequence is refused by a part that is not ready (just reset,
   or still calibrating), and by one that has wedged and refuses everything
   until it is reset. The first is cured by waiting, the second only by a reset
   through NRESET, so a refused sequence is run again after a pause, and a
   second time behind a reset. Only refusals and timeouts qualify: a bad
   argument is the caller's. */
static bool recoverable(esp_err_t err)
{
    return err == ESP_ERR_INVALID_RESPONSE || err == ESP_ERR_TIMEOUT;
}

/* A reset is not repeated sooner than this, and after one that did not bring
   the part back, later: a dead part must not stall every poll on the lock. */
#define RECOVER_GAP_MIN_US   1000000
#define RECOVER_GAP_MAX_US  32000000
static int64_t s_recover_at_us, s_recover_gap_us = RECOVER_GAP_MIN_US;
static bool    s_recovered;

/* NRESET, the wait, and the identification again. The part then holds nothing
   it was told, and whoever owns it is told so. The lock is held. */
static esp_err_t recover_locked(const char *why)
{
    const int64_t now = esp_timer_get_time();
    if (s_recovered && now - s_recover_at_us < s_recover_gap_us) return ESP_ERR_INVALID_STATE;
    s_recovered = true;
    s_recover_at_us = now;
    ESP_LOGW(TAG, "%s: resetting the part (last stat 0x%04X)", why, (unsigned)s_stat);

    esp_err_t err = ls_lora_hw_reset();
    lr20xx_info_t info;
    for (int i = 0; err == ESP_OK && i < 5; i++) {
        err = probe_locked(&info);
        if (err != ESP_OK) vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "the part did not come back from a reset: %s", esp_err_to_name(err));
        if (s_recover_gap_us < RECOVER_GAP_MAX_US) s_recover_gap_us *= 2;
        return err;
    }
    s_recover_gap_us = RECOVER_GAP_MIN_US;
    s_info = info;
    s_fail_run = 0;
    s_reset_seen = true;
    s_agc_forced = false;
    lr.cal_hz = 0;
    lr.programmed = false;
    lr.rx_mode = false;
    lr.tx_busy = false;
    lr.rearm = lr.cfg_valid && !lr.fsk_active && !lr.scanning && !s_exp_active && !ms.active;
    lr.rearm_after_us = 0;
    lr.fsk_stale = lr.fsk_active;
    lr.scan_plan_n = 0;
    ms.reset_seen = ms.active;
    s_exp_reset = s_exp_active;
    (void)lr20xx_clear_irq(LR20XX_IRQ_ALL);
    return ESP_OK;
}

static bool retry_gate(unsigned n, esp_err_t err, const char *what)
{
    if (err == ESP_OK || !recoverable(err) || n >= 2) return false;
    ESP_LOGW(TAG, "%s refused (%s)%s", what, esp_err_to_name(err),
             n == 0 && s_fail_run < WEDGE_RUN ? ", again" : ", reset and again");
    if (n == 0 && s_fail_run < WEDGE_RUN) {
        vTaskDelay(pdMS_TO_TICKS(15));
        return true;
    }
    return recover_locked(what) == ESP_OK;
}

static void heal(void)
{
    if (s_fail_run >= WEDGE_RUN) (void)recover_locked("part not taking commands");
}

static esp_err_t step_lr(esp_err_t err, const char *what)
{
    if (err == ESP_OK) err = lr20xx_check_last_ok();
    if (err != ESP_OK) ESP_LOGE(TAG, "%s failed: %s", what, esp_err_to_name(err));
    return err;
}

static esp_err_t expect_mode(unsigned want)
{
    lr20xx_status_t st;
    esp_err_t err = lr20xx_get_status(&st);
    if (err != ESP_OK) return err;
    if (!lr20xx_stat_plausible(st.stat) || LR20XX_STAT_CMD(st.stat) != LR20XX_CMD_OK ||
        LR20XX_STAT_MODE(st.stat) != want) {
        ESP_LOGE(TAG, "expected chip mode %u, stat 0x%04X", want, (unsigned)st.stat);
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

/* What every packet path does first, from standby, in the Mode S session's
   order, which is the one seen working on air: the antenna, the RF switch and
   interrupt DIOs, the fallback, a calibration, the packet type with the DC-DC
   settings it resets, and the tune on the LF input. The PA is not touched. */
static esp_err_t setup_radio(uint8_t pkt_type, uint32_t freq_hz)
{
    esp_err_t err = step_lr(lr20xx_set_standby(false), "SetStandby");
    if (err != ESP_OK) return err;
    select_antenna();
    err = configure_dios_mask(PKT_IRQ_MASK);
    if (err == ESP_OK) err = step_lr(lr20xx_set_fallback_standby_rc(), "SetRxTxFallbackMode");
    if (err == ESP_OK) err = step_lr(lr20xx_clear_irq(LR20XX_IRQ_ALL), "ClearIrq");
    /* PA_OFF is left out of the calibration, as for Mode S. */
    if (err == ESP_OK)
        err = lr20xx_calibrate(LR20XX_CAL_LF_RC | LR20XX_CAL_HF_RC | LR20XX_CAL_PLL |
                               LR20XX_CAL_AAF | LR20XX_CAL_MU);
    if (err == ESP_OK) err = lr20xx_wait_ready(200);
    if (err == ESP_OK) err = step_lr(ESP_OK, "Calibrate");
    if (err == ESP_OK) err = step_lr(lr20xx_set_packet_type(pkt_type), "SetPacketType");
    if (err == ESP_OK) err = lr20xx_dcdc_workaround_reset();
    if (err == ESP_OK) err = lr20xx_tune_rx(freq_hz, -1);
    if (err == ESP_OK) lr.cal_hz = freq_hz;
    return err;
}

/* lr.cfg into the part. Standby afterwards, or on failure. */
static esp_err_t program_lora_once(void)
{
    const ls_lora_cfg_t *c = &lr.cfg;
    lr.programmed = false;
    lr.rx_mode = false;
    lr.tx_busy = false;
    esp_err_t err = setup_radio(LR20XX_PKT_TYPE_LORA, c->freq_hz);
    if (err == ESP_OK)
        err = step_lr(lr20xx_lora_set_modulation(c->sf, lr.bw_code, (uint8_t)(c->cr - 4),
                                                 lr20xx_lora_ldro(c->sf, c->bw_hz)),
                      "SetLoraModulationParams");
    /* After the modulation, where RadioLib applies it. RadioLib also applies
       it after SetRxPath, which the tune has already sent; the modulation is
       what decides the settings, so once after it covers both. */
    if (err == ESP_OK) err = lr20xx_dcdc_workaround_set();
    if (err == ESP_OK)
        err = step_lr(lr20xx_lora_set_packet(c->preamble, 255, false, c->crc_on, c->invert_iq),
                      "SetLoraPacketParams");
    if (err == ESP_OK) err = step_lr(lr20xx_lora_set_syncword(c->sync_word), "SetLoraSyncword");
    if (err == ESP_OK) err = lr20xx_set_pa_lf(c->power_dbm);
    if (err == ESP_OK) lr.programmed = true;
    else (void)lr20xx_set_standby(false);
    return err;
}

static esp_err_t program_lora(void)
{
    esp_err_t err;
    unsigned n = 0;
    do err = program_lora_once(); while (retry_gate(n++, err, "LoRa setup"));
    if (err == ESP_OK) s_reset_seen = false;
    return err;
}

static esp_err_t configure_lora(const ls_lora_cfg_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    if (cfg->sf < 5 || cfg->sf > 12) return ESP_ERR_INVALID_ARG;
    if (cfg->cr < 5 || cfg->cr > 8)  return ESP_ERR_INVALID_ARG;
    if (cfg->power_dbm < LR20XX_TX_POWER_MIN_DBM || cfg->power_dbm > LR20XX_TX_POWER_MAX_DBM)
        return ESP_ERR_INVALID_ARG;
    if (cfg->cal_min_mhz >= cfg->cal_max_mhz) return ESP_ERR_INVALID_ARG;
    if (cfg->freq_hz < LR20XX_FREQ_MIN_HZ) return ESP_ERR_INVALID_ARG;
    /* A configuration is the transmitter's too, so it is the LF path or
       nothing. 1090 MHz Mode S is a receive session of its own. */
    if (cfg->freq_hz >= LR20XX_TX_FREQ_LIMIT_HZ) return ESP_ERR_NOT_SUPPORTED;
    if (!ls_lora_hw_pkt()) return ESP_ERR_INVALID_STATE;

    const lr20xx_lora_bw_t bw = lr20xx_lora_bw_nearest(cfg->bw_hz);
    lr.cfg = *cfg;
    lr.cfg.bw_hz = bw.hz;                    /* report what was set */
    lr.bw_code = bw.code;
    lr.cfg_valid = false;
    lr.rearm = false;
    /* The image calibration band has no LR20xx command: CalibFE runs at the
       frequency in use instead. The band is still checked, as on the SX126x. */
    const esp_err_t err = program_lora();
    if (err != ESP_OK) return err;
    lr.cfg_valid = true;
    ESP_LOGI(TAG, "configured %.4f MHz SF%u BW%lu CR4/%u %+d dBm sync 0x%02X",
             lr.cfg.freq_hz / 1e6, (unsigned)lr.cfg.sf, (unsigned long)lr.cfg.bw_hz,
             (unsigned)lr.cfg.cr, (int)lr.cfg.power_dbm, (unsigned)lr.cfg.sync_word);
    return ESP_OK;
}

static esp_err_t lr_configure(const ls_lora_cfg_t *cfg)
{
    if (lr.fsk_active || lr.scanning || s_exp_active || lr20xx_modes_active()) return ESP_ERR_INVALID_STATE;
    return configure_lora(cfg);
}

static const ls_lora_cfg_t *lr_cfg(void) { return lr.cfg_valid ? &lr.cfg : NULL; }

/* Standby, the receive length back to the most (a transmit set its own), an
   empty FIFO, and continuous Rx. SetRx is refused in Rx (6.3.5), hence the
   standby. */
static esp_err_t arm_lora_rx(void)
{
    esp_err_t err = step_lr(lr20xx_set_standby(false), "SetStandby");
    if (err == ESP_OK)
        err = step_lr(lr20xx_lora_set_packet(lr.cfg.preamble, 255, false, lr.cfg.crc_on,
                                             lr.cfg.invert_iq), "SetLoraPacketParams");
    if (err == ESP_OK) err = step_lr(lr20xx_clear_rx_fifo(), "ClearRxFifo");
    if (err == ESP_OK) err = step_lr(lr20xx_clear_irq(LR20XX_IRQ_ALL), "ClearIrq");
    if (err == ESP_OK) err = lr20xx_set_rx(0xFFFFFFu);
    if (err == ESP_OK) err = expect_mode(LR20XX_MODE_RX);
    lr.rx_mode = err == ESP_OK;
    if (lr.rx_mode) lr.tx_busy = false;
    return err;
}

/* Give the part back to LoRa after an FSK session, a sweep or a Mode S
   session had it, as the SX126x backend's fsk_end does: programmed again in
   full, and receiving if it was. On failure the configuration stays, and the
   receive is left for the next poll to retry. */
static esp_err_t restore_lora(bool rx)
{
    if (!lr.cfg_valid) return ESP_OK;
    esp_err_t err = program_lora();
    if (err == ESP_OK && rx) err = arm_lora_rx();
    lr.rearm = rx && err != ESP_OK;
    return err;
}

static esp_err_t lr_receive(void)
{
    if (!lr.cfg_valid || lr.fsk_active || lr.scanning || s_exp_active || lr20xx_modes_active())
        return ESP_ERR_INVALID_STATE;
    heal();
    if (s_reset_seen) {
        /* Something saw the part restart: it holds none of the configuration. */
        s_reset_seen = false;
        lr.programmed = false;
        lr.rx_mode = false;
    }
    esp_err_t err = ESP_OK;
    if (lr.programmed && lr.rx_mode) {
        /* Already listening: nothing to do, as on the SX126x, and no standby,
           which would drop a packet half received. The chip is asked. */
        lr20xx_status_t st;
        err = lr20xx_get_status(&st);
        if (err == ESP_OK && lr20xx_stat_plausible(st.stat) &&
            LR20XX_STAT_MODE(st.stat) == LR20XX_MODE_RX && LR20XX_STAT_RESET_SRC(st.stat) == 0) {
            lr.rearm = false;
            return ESP_OK;
        }
        if (err == ESP_OK && LR20XX_STAT_RESET_SRC(st.stat) != 0) lr.programmed = false;
        err = ESP_OK;
    }
    if (!lr.programmed) err = program_lora();
    if (err == ESP_OK) err = arm_lora_rx();
    lr.rearm = err != ESP_OK;
    return err;
}

static bool lr_is_receiving(void) { return lr.rx_mode; }

/* After the payload is in the Tx FIFO: SetTx, which refuses anything but the
   LF PA below 1 GHz, then the bookkeeping the SX126x backend keeps. */
static esp_err_t start_tx(size_t len, int8_t power_dbm, const char *what)
{
    const esp_err_t err = step_lr(lr20xx_set_tx(), "SetTx");
    if (err != ESP_OK) return err;
    lr.tx_busy = true;
    lr.rx_mode = false;
    lr.rearm = false;
    lr.tx_deadline = esp_timer_get_time() + (int64_t)TX_TIMEOUT_MS * 1000;
    ESP_LOGI(TAG, "%s: %u bytes at %d dBm, %lu Hz", what, (unsigned)len, (int)power_dbm,
             (unsigned long)s_rf_hz);
    return ESP_OK;
}

static esp_err_t load_tx_fifo(const uint8_t *data, size_t len)
{
    esp_err_t err = step_lr(lr20xx_clear_tx_fifo(), "ClearTxFifo");
    if (err == ESP_OK) err = step_lr(lr20xx_write_tx_fifo(data, len), "WriteRadioTxFifo");
    return err;
}

static esp_err_t lr_send(const uint8_t *data, size_t len)
{
    if (!lr.cfg_valid || lr.fsk_active) return ESP_ERR_INVALID_STATE;
    if (!data || len == 0) return ESP_ERR_INVALID_ARG;
    if (len > 255) return ESP_ERR_INVALID_SIZE;
    if (lr.tx_busy || lr.scanning || s_exp_active || lr20xx_modes_active()) return ESP_ERR_INVALID_STATE;
    if (!ls_lora_hw_pkt()) return ESP_ERR_INVALID_STATE;
    if (lr.cfg.freq_hz >= LR20XX_TX_FREQ_LIMIT_HZ) return ESP_ERR_NOT_SUPPORTED;

    esp_err_t err = ESP_OK;
    if (!lr.programmed && (err = program_lora()) != ESP_OK) { lr.rearm = true; return err; }
    if ((err = step_lr(lr20xx_set_standby(false), "SetStandby")) != ESP_OK) return err;
    lr.rx_mode = false;
    lr.rearm = true;            /* out of receive until the transmit starts */
    if ((err = step_lr(lr20xx_clear_irq(LR20XX_IRQ_ALL), "ClearIrq")) != ESP_OK) return err;
    err = step_lr(lr20xx_lora_set_packet(lr.cfg.preamble, (uint8_t)len, false, lr.cfg.crc_on,
                                         lr.cfg.invert_iq), "SetLoraPacketParams");
    if (err == ESP_OK) err = load_tx_fifo(data, len);
    if (err == ESP_OK) err = start_tx(len, lr.cfg.power_dbm, "TX begin");
    if (err == ESP_OK)
        ESP_LOGI(TAG, "TX estimated %lu ms", (unsigned long)lr_airtime_ms((int)len));
    return err;
}

/* TxDone or Timeout on the IRQ word read over SPI, or our own deadline.
   Either way the part goes back to standby with its interrupts cleared; the
   fallback mode has normally put it there already. */
static bool lr_send_done(void)
{
    if (!lr.tx_busy) return true;
    lr20xx_status_t st;
    const bool read = lr20xx_get_status(&st) == ESP_OK && lr20xx_stat_plausible(st.stat);
    const bool done = read && (st.irq & (LR20XX_IRQ_TX_DONE | LR20XX_IRQ_TIMEOUT)) != 0;
    const bool reset = read && LR20XX_STAT_RESET_SRC(st.stat) != 0;
    if (!done && !reset && esp_timer_get_time() <= lr.tx_deadline) return false;

    if (reset) {
        ESP_LOGW(TAG, "chip reset during a transmit (stat 0x%04X)", (unsigned)st.stat);
        lr.programmed = false;
    } else if (!done) {
        ESP_LOGW(TAG, "transmit did not complete in %d ms", TX_TIMEOUT_MS);
    }
    (void)lr20xx_set_standby(false);
    (void)lr20xx_clear_irq(LR20XX_IRQ_ALL);
    lr.tx_busy = false;
    if (done) ESP_LOGI(TAG, "TX finished: irq=0x%08lx", (unsigned long)st.irq);
    return true;
}

static int lr_poll(uint8_t *buf, size_t max, float *rssi_dbm, float *snr_db)
{
    if (!lr.cfg_valid || lr.fsk_active || lr.scanning || s_exp_active || lr20xx_modes_active() ||
        !buf || !max || !ls_lora_hw_pkt())
        return 0;
    if (lr.rearm && !lr.tx_busy) {
        const int64_t now = esp_timer_get_time();
        if (now < lr.rearm_after_us) return 0;
        if (lr_receive() != ESP_OK) {
            lr.rearm_after_us = now + 250000;
            return 0;
        }
    }
    if (!lr.rx_mode && !lr.tx_busy) return 0;

    /* Stat and the IRQ word in one six-byte frame: no DIO to read first. */
    lr20xx_status_t st;
    if (lr20xx_get_status(&st) != ESP_OK || !lr20xx_stat_plausible(st.stat)) return 0;
    if (take_reset(st.stat)) {
        /* It restarted and holds none of the configuration. The next poll
           programs it again and listens. */
        ESP_LOGW(TAG, "chip reset (stat 0x%04X), reprogramming", (unsigned)st.stat);
        lr.programmed = false;
        lr.rx_mode = false;
        lr.tx_busy = false;
        lr.rearm = true;
        return 0;
    }
    const uint32_t irq = st.irq & PKT_IRQ_MASK;
    const bool left_rx = LR20XX_STAT_MODE(st.stat) != LR20XX_MODE_RX;
    if (irq && lr20xx_clear_irq(irq) != ESP_OK) return 0;
    if (irq & LR20XX_IRQ_TX_DONE) lr.tx_busy = false;
    /* Continuous Rx stays in Rx after a packet. One that left it for any
       other reason is armed again by the next poll. */
    if (lr.rx_mode && !lr.tx_busy && left_rx) lr.rearm = true;

    if (irq & (LR20XX_IRQ_CRC_ERROR | LR20XX_IRQ_LORA_HDR_ERROR | LR20XX_IRQ_LEN_ERROR)) {
        (void)lr20xx_clear_rx_fifo();
        return -1;
    }
    if (!(irq & LR20XX_IRQ_RX_DONE)) return 0;

    uint16_t len = 0;
    if (lr20xx_get_rx_pkt_length(&len) != ESP_OK || len == 0 || len > 255) {
        (void)lr20xx_clear_rx_fifo();
        return 0;
    }
    const size_t n = len < max ? len : max;
    if (lr20xx_read_rx_fifo(buf, n) != ESP_OK) {
        (void)lr20xx_clear_rx_fifo();
        return 0;
    }
    lr20xx_lora_pkt_status_t ps;
    if (lr20xx_get_lora_packet_status(&ps) == ESP_OK) {
        if (rssi_dbm) *rssi_dbm = ps.rssi_dbm;
        if (snr_db)   *snr_db   = ps.snr_db;
    }
    /* Whatever of the packet was not read goes, so the next one starts at the
       head of the FIFO; RadioLib clears it after every read. */
    (void)lr20xx_clear_rx_fifo();
    return (int)n;
}

/* Valid whenever the part is receiving, however it got there. The chip is
   asked, in the same read: the Stat that comes back with the answer says
   whether it was receiving when it measured, so no GetStatus goes first.
   Half the frames of asking twice, which is what sets how fast a burst can
   be timed. */
static esp_err_t lr_rssi_inst(float *dbm)
{
    float v = 0;
    const esp_err_t err = lr20xx_get_rssi_inst(&v);
    if (err != ESP_OK) return err;
    if (LR20XX_STAT_MODE(lr20xx_last_stat()) != LR20XX_MODE_RX) return ESP_ERR_INVALID_STATE;
    if (dbm) *dbm = v;
    return ESP_OK;
}

static uint32_t lr_airtime_ms(int len)
{
    if (!lr.cfg_valid || len < 0) return 0;
    /* Semtech AN1200.13 in microseconds, the formula ls_lora_sx126x.c uses,
       with the bandwidth as set and the LDRO decision this part makes. This
       feeds a duty-cycle budget. */
    const uint32_t bw = lr.cfg.bw_hz;
    const uint32_t sf = lr.cfg.sf;
    if (!bw) return 0;

    const uint64_t ts_us = ((uint64_t)(1u << sf) * 1000000ull) / bw;
    const int de  = lr20xx_lora_ldro((uint8_t)sf, bw) ? 1 : 0;
    const int crc = lr.cfg.crc_on ? 1 : 0;

    const int num = 8 * len - 4 * (int)sf + 28 + 16 * crc;   /* explicit header */
    const int den = 4 * ((int)sf - 2 * de);
    int ceil_term = den > 0 ? (num + den - 1) / den : 0;
    if (ceil_term < 0) ceil_term = 0;
    const int payload_symb = 8 + ceil_term * (int)lr.cfg.cr;

    const uint64_t preamble_us = ts_us * lr.cfg.preamble + (ts_us * 425) / 100; /* +4.25 */
    const uint64_t payload_us  = ts_us * (uint64_t)payload_symb;
    return (uint32_t)((preamble_us + payload_us + 999) / 1000);
}

/* ---- FSK sessions -------------------------------------------------------- */

/* Fixed length, no CRC and no whitening, for the reasons ls_lora_sx126x.c
   gives; transmit and receive both come through here so they agree on every
   field. The preamble detector is the caller's: receive passes the
   session's, transmit 0. */
static esp_err_t fsk_packet(uint16_t payload_bytes, uint8_t detect_bits)
{
    return step_lr(lr20xx_gfsk_set_packet_fixed(lr.fsk_pre_bits, detect_bits, payload_bytes),
                   "SetGfskPacketParams");
}

/* A stream session asks for the longest packet the field holds; the part
   ends it at 8191 bytes. */
#define FSK_STREAM_PACKET 0xFFFFu
#define FSK_FIFO_BYTES    256u

static uint16_t fsk_rx_bytes(void)
{
    return lr.fsk_stream ? FSK_STREAM_PACKET : lr.fsk_bytes;
}

static esp_err_t fsk_arm_rx(void)
{
    esp_err_t err = step_lr(lr20xx_clear_rx_fifo(), "ClearRxFifo");
    if (err == ESP_OK) err = step_lr(lr20xx_clear_irq(LR20XX_IRQ_ALL), "ClearIrq");
    if (err == ESP_OK) err = lr20xx_set_rx(0xFFFFFFu);
    if (err == ESP_OK) err = expect_mode(LR20XX_MODE_RX);
    lr.rx_mode = err == ESP_OK;
    /* Whatever a stream had read belongs to the packet just dropped. */
    lr.fsk_stream_taken = 0;
    lr.fsk_stream_fresh = true;
    return err;
}

static bool lr_fsk_active(void) { return lr.fsk_active; }

static esp_err_t lr_fsk_end(void)
{
    if (!lr.fsk_active) return ESP_OK;
    esp_err_t err = step_lr(lr20xx_set_standby(false), "SetStandby");
    lr.rx_mode = false;
    lr.tx_busy = false;          /* standby ended any transmit in flight */
    lr.fsk_active = false;
    /* LoRa listens on the AGC: a step the session held does not stay behind it. */
    if (s_agc_forced) (void)step_lr(lr20xx_set_agc_gain(0), "SetAgcGainManual");
    const esp_err_t restored = restore_lora(lr.restore_rx);
    if (restored != ESP_OK) err = restored;
    return err;
}

/* lr.fsk_cfg into the part, from standby to listening; run again when the part
   has lost it. */
static esp_err_t fsk_program_once(void)
{
    const ls_fsk_cfg_t *cfg = &lr.fsk_cfg;
    const lr20xx_rx_bw_t bw = lr20xx_fsk_rx_bw(cfg->bandwidth_hz);
    esp_err_t err = setup_radio(LR20XX_PKT_TYPE_GFSK, lr.fsk_freq_hz);
    if (err == ESP_OK && cfg->rx_boost_step)
        err = step_lr(lr20xx_set_rx_path(lr20xx_path_for_hz(lr.fsk_freq_hz),
                                         (uint8_t)(cfg->rx_boost_step - 1)), "SetRxPath");
    if (err == ESP_OK)
        err = step_lr(lr20xx_gfsk_set_modulation(cfg->bitrate, cfg->pulse_shape, bw.index, cfg->deviation_hz),
                      "SetGfskModulationParams");
    if (err == ESP_OK) err = lr20xx_dcdc_workaround_set();
    if (err == ESP_OK) err = fsk_packet(fsk_rx_bytes(), lr.fsk_detect_bits);
    if (err == ESP_OK)
        err = step_lr(lr20xx_gfsk_set_syncword(cfg->sync_word, lr.fsk_sync_bits), "SetGfskSyncword");
    /* Above 960 MHz the session only listens: the PA is not programmed, so
       lr20xx_set_tx refuses whatever is asked of it there. */
    if (err == ESP_OK && cfg->freq_hz < LR20XX_TX_FREQ_LIMIT_HZ)
        err = lr20xx_set_pa_lf(cfg->power_dbm);
    /* The chip's AGC, unless a step is asked for. Nothing is sent for the AGC
       unless an earlier session left a step in the part: 0 is the AGC. */
    if (err == ESP_OK && (cfg->rx_gain_step || s_agc_forced))
        err = step_lr(lr20xx_set_agc_gain(cfg->rx_gain_step), "SetAgcGainManual");
    if (err == ESP_OK) err = fsk_arm_rx();
    return err;
}

static esp_err_t fsk_program(void)
{
    esp_err_t err;
    unsigned n = 0;
    do err = fsk_program_once(); while (retry_gate(n++, err, "FSK setup"));
    if (err == ESP_OK) s_reset_seen = false;
    return err;
}

/* Every field of ls_fsk_cfg_t as the SX126x backend reads it, with one
   difference: the filter is this part's Table 11-4 rung at or above
   bandwidth_hz (what ls_lora_fsk_bw_snap returns with an LR20xx bound), not
   only an exact rung, so a caller that worked from the SX126x ladder still
   gets a filter no narrower than it asked for. A stream session is the
   exception to the payload and filter checks: it has no payload length,
   and it samples the signal at a bitrate above the signal's own. */
static esp_err_t lr_fsk_begin(const ls_fsk_cfg_t *cfg)
{
    /* Receive reaches 1100 MHz on the LF input, where the Mode S session
       already works, so UAT at 978 MHz can be heard, and 1500-2500 MHz on the
       HF input of a part that has one; a transmit above 960 MHz is still
       refused by lr20xx_set_tx. The engine runs to 2 Mbps. */
    if (!cfg || !ls_lora_rx_range_ok(lr_caps(), cfg->freq_hz, cfg->freq_hz) ||
        cfg->bitrate < LR20XX_FSK_BITRATE_MIN || cfg->bitrate > LR20XX_FSK_BITRATE_MAX ||
        cfg->deviation_hz < 600 || cfg->deviation_hz > LR20XX_FSK_DEVIATION_MAX ||
        (!cfg->stream && !cfg->payload_bytes) || !cfg->bandwidth_hz ||
        cfg->bandwidth_hz > lr20xx_fsk_rx_bw_table[lr20xx_fsk_rx_bw_table_n - 1].hz ||
        (!cfg->stream && cfg->bitrate + 2 * cfg->deviation_hz > cfg->bandwidth_hz) ||
        (cfg->stream && cfg->rssi_at_sync) ||
        (cfg->preamble_bits && cfg->preamble_bits < 8) ||
        /* 32 at most: sync_word is a uint32_t, as on the SX126x. */
        (cfg->sync_bits && (cfg->sync_bits > 32 || cfg->sync_bits % 8)) ||
        cfg->preamble_detect_bits > 32 || cfg->preamble_detect_bits % 8 ||
        cfg->rx_boost_step > 8 || cfg->rx_gain_step > LR20XX_MODES_GAIN_MAX ||
        cfg->power_dbm < LR20XX_TX_POWER_MIN_DBM || cfg->power_dbm > LR20XX_TX_POWER_MAX_DBM)
        return ESP_ERR_INVALID_ARG;
    if (lr.fsk_active || lr.scanning || lr.tx_busy || s_exp_active || lr20xx_modes_active())
        return ESP_ERR_INVALID_STATE;
    if (!ls_lora_hw_pkt()) return ESP_ERR_INVALID_STATE;

    const lr20xx_rx_bw_t bw = lr20xx_fsk_rx_bw(cfg->bandwidth_hz);
    lr.restore_rx = lr.rx_mode || lr.rearm;
    lr.fsk_active = true;
    lr.programmed = false;
    lr.rx_mode = false;
    lr.rearm = false;
    lr.fsk_cfg = *cfg;
    lr.fsk_stale = false;
    lr.fsk_freq_hz = cfg->freq_hz;
    lr.fsk_power = cfg->power_dbm;
    lr.fsk_bytes = cfg->payload_bytes;
    lr.fsk_pre_bits = cfg->preamble_bits ? cfg->preamble_bits : 32;
    lr.fsk_sync_bits = cfg->sync_bits ? cfg->sync_bits : 32;
    lr.fsk_detect_bits = cfg->preamble_detect_bits;
    lr.fsk_rssi_at_sync = cfg->rssi_at_sync;
    lr.fsk_stream = cfg->stream;

    const esp_err_t err = fsk_program();
    if (err != ESP_OK) {
        const esp_err_t restored = lr_fsk_end();
        return restored == ESP_OK ? err : restored;
    }
    ESP_LOGI(TAG, "FSK %s: %lu Hz, %lu bps, deviation %lu Hz, filter %lu Hz",
             cfg->stream ? "stream" : "session", (unsigned long)cfg->freq_hz,
             (unsigned long)cfg->bitrate, (unsigned long)cfg->deviation_hz, (unsigned long)bw.hz);
    return ESP_OK;
}

static int lr_fsk_poll(uint8_t *buf, size_t size, float *rssi_dbm)
{
    if (!lr.fsk_active || lr.fsk_stream || !buf || size < lr.fsk_bytes) return -1;
    heal();
    lr20xx_status_t st;
    if (lr20xx_get_status(&st) != ESP_OK || !lr20xx_stat_plausible(st.stat)) return -1;
    if (take_reset(st.stat) || lr.fsk_stale) {
        /* The part restarted, or the driver reset it: it listens to nothing
           until it is told again. */
        ESP_LOGW(TAG, "FSK session: part restarted (stat 0x%04X), reprogramming", (unsigned)st.stat);
        lr.fsk_stale = false;
        lr.cal_hz = 0;
        if (fsk_program() != ESP_OK) { lr.fsk_stale = true; return -1; }
        return 0;
    }
    const uint32_t irq = st.irq & (LR20XX_IRQ_RX_DONE | LR20XX_IRQ_CRC_ERROR |
                                   LR20XX_IRQ_TIMEOUT | LR20XX_IRQ_LEN_ERROR);
    if (!irq) return 0;
    if (lr20xx_clear_irq(irq) != ESP_OK) return -1;
    if (irq & (LR20XX_IRQ_CRC_ERROR | LR20XX_IRQ_TIMEOUT | LR20XX_IRQ_LEN_ERROR)) {
        (void)lr20xx_clear_rx_fifo();
        return -1;
    }
    uint16_t len = 0;
    if (lr20xx_get_rx_pkt_length(&len) != ESP_OK || len != lr.fsk_bytes ||
        lr20xx_read_rx_fifo(buf, lr.fsk_bytes) != ESP_OK) {
        (void)lr20xx_clear_rx_fifo();
        return -1;
    }
    if (rssi_dbm) *rssi_dbm = lr.fsk_rssi_at_sync ? NAN : -200.0f;
    lr20xx_gfsk_pkt_status_t ps;
    if (lr20xx_get_gfsk_packet_status(&ps) == ESP_OK && rssi_dbm)
        *rssi_dbm = lr.fsk_rssi_at_sync ? ps.rssi_sync_dbm : ps.rssi_avg_dbm;
    /* Continuous Rx looks for the next sync word without being restarted. */
    return lr.fsk_bytes;
}

/* A stream session's bytes as they arrive. On RxDone the packet's last
   bytes are read, as many as its length says are still in the buffer, and
   the receive is armed again, which empties the buffer: nothing of the next
   packet is ever read as the end of this one. */
static int lr_fsk_stream_read(uint8_t *buf, size_t size, bool *restarted)
{
    if (restarted) *restarted = false;
    if (!lr.fsk_active || !lr.fsk_stream || !buf || size < FSK_FIFO_BYTES) return -1;
    heal();
    lr20xx_status_t st;
    if (lr20xx_get_status(&st) != ESP_OK || !lr20xx_stat_plausible(st.stat)) return -1;
    if (take_reset(st.stat) || lr.fsk_stale) {
        ESP_LOGW(TAG, "FSK stream: part restarted (stat 0x%04X), reprogramming", (unsigned)st.stat);
        lr.fsk_stale = false;
        lr.cal_hz = 0;
        if (fsk_program() != ESP_OK) { lr.fsk_stale = true; return -1; }
        return 0;
    }
    const bool done = (st.irq & LR20XX_IRQ_RX_DONE) != 0;
    uint16_t level = 0;
    if (lr20xx_get_rx_fifo_level(&level) != ESP_OK || level > FSK_FIFO_BYTES) return -1;
    size_t want = level;
    if (done) {
        uint16_t len = 0;
        if (lr20xx_get_rx_pkt_length(&len) == ESP_OK)
            want = len > lr.fsk_stream_taken ? (size_t)(len - lr.fsk_stream_taken) : 0;
        if (want > level) want = level;
    }
    if (want > size) want = size;
    size_t n = 0;
    while (n < want) {
        const size_t part = want - n > 255 ? 255 : want - n;
        if (lr20xx_read_rx_fifo(buf + n, part) != ESP_OK) {
            /* What left the buffer is unknown: start clean. */
            (void)lr20xx_set_standby(false);
            (void)fsk_arm_rx();
            return -1;
        }
        n += part;
    }
    if (n && restarted) *restarted = lr.fsk_stream_fresh;
    if (n) lr.fsk_stream_fresh = false;
    lr.fsk_stream_taken = (uint16_t)(lr.fsk_stream_taken + n);
    /* A full buffer has dropped what arrived after it filled. */
    if (level >= FSK_FIFO_BYTES) lr.fsk_stream_fresh = true;
    if (done || LR20XX_STAT_MODE(st.stat) != LR20XX_MODE_RX) {
        (void)lr20xx_set_standby(false);
        if (fsk_arm_rx() != ESP_OK) return n ? (int)n : -1;
    }
    return (int)n;
}

static esp_err_t lr_fsk_send(const uint8_t *data, size_t len)
{
    if (!lr.fsk_active || !ls_lora_hw_pkt() || lr.fsk_stream) return ESP_ERR_INVALID_STATE;
    if (!data || len == 0)          return ESP_ERR_INVALID_ARG;
    if (len > 255)                  return ESP_ERR_INVALID_SIZE;
    if (lr.tx_busy || lr.scanning)  return ESP_ERR_INVALID_STATE;
    if (lr.fsk_freq_hz >= LR20XX_TX_FREQ_LIMIT_HZ) return ESP_ERR_NOT_SUPPORTED;

    esp_err_t err = step_lr(lr20xx_set_standby(false), "SetStandby");
    if (err != ESP_OK) return err;
    /* Cleared before the FIFO write: a failure below must not leave the flag
       claiming the part is still listening. */
    lr.rx_mode = false;
    if ((err = step_lr(lr20xx_clear_irq(LR20XX_IRQ_ALL), "ClearIrq")) != ESP_OK) return err;
    /* The length this payload is, not the session's receive length;
       lr_fsk_receive puts that back. */
    if ((err = fsk_packet((uint16_t)len, 0)) != ESP_OK) return err;
    if ((err = load_tx_fifo(data, len)) != ESP_OK) return err;
    err = start_tx(len, lr.fsk_power, "FSK TX");
    if (err == ESP_OK)
        ESP_LOGI(TAG, "FSK TX behind a %u-bit preamble and a %u-bit sync word",
                 (unsigned)lr.fsk_pre_bits, (unsigned)lr.fsk_sync_bits);
    return err;
}

/* A session the part has lost (it restarted, or the driver reset it) is put
   back before the call that found it out does its own work. */
static esp_err_t fsk_refresh(void)
{
    heal();
    if (!lr.fsk_stale) return ESP_OK;
    lr.fsk_stale = false;
    const esp_err_t err = fsk_program();
    if (err != ESP_OK) lr.fsk_stale = true;
    return err;
}

static esp_err_t lr_fsk_receive(void)
{
    if (!lr.fsk_active) return ESP_ERR_INVALID_STATE;
    if (lr.tx_busy)     return ESP_ERR_INVALID_STATE;
    {
        const esp_err_t r = fsk_refresh();
        if (r != ESP_OK) return r;
    }
    /* Standby first: SetRx is refused in Rx, and this may be asked of a
       session that is already listening. */
    esp_err_t err = step_lr(lr20xx_set_standby(false), "SetStandby");
    if (err == ESP_OK) err = fsk_packet(fsk_rx_bytes(), lr.fsk_detect_bits);
    if (err == ESP_OK) err = fsk_arm_rx();
    return err;
}

/* Standby with the crystal running, CalibFE when the move is a big one (the
   sweep's rule), the new frequency, and Rx again: the modulation, packet and
   DC-DC settings stay as the session set them. One input only, because the
   path and its boost belong to the session's setup. */
static esp_err_t lr_fsk_retune(uint32_t hz)
{
    if (!lr.fsk_active || lr.tx_busy) return ESP_ERR_INVALID_STATE;
    if (!ls_lora_rx_range_ok(lr_caps(), hz, hz) ||
        lr20xx_path_for_hz(hz) != lr20xx_path_for_hz(lr.fsk_freq_hz))
        return ESP_ERR_INVALID_ARG;
    {
        const esp_err_t r = fsk_refresh();
        if (r != ESP_OK) return r;
    }
    esp_err_t err = step_lr(lr20xx_set_standby(true), "SetStandby");
    lr.rx_mode = false;
    const uint32_t moved = hz > lr.cal_hz ? hz - lr.cal_hz : lr.cal_hz - hz;
    if (err == ESP_OK && moved >= CALIB_FE_STEP_HZ) {
        err = step_lr(lr20xx_calib_fe_hz(hz, lr20xx_path_for_hz(hz)), "CalibFE");
        if (err == ESP_OK) lr.cal_hz = hz;
    }
    if (err == ESP_OK) err = step_lr(lr20xx_set_rf_frequency(hz), "SetRfFrequency");
    if (err == ESP_OK) lr.fsk_freq_hz = hz;
    if (err == ESP_OK) err = fsk_arm_rx();
    return err;
}

/* ---- the RSSI sweep ------------------------------------------------------- */

static bool lr_scanning(void) { return lr.scanning; }

/* GFSK receive on the band, planned for `n` bins on this part's own filter
   table (lr20xx_scan_plan), so a wide band takes a filter up to 3 MHz and
   far fewer looks than the SX126x's 500 kHz would need. The tune picks the
   input from the band, which never crosses from one to the other. */
static esp_err_t scan_configure(int n)
{
    lr20xx_rx_bw_t bw;
    lr20xx_scan_plan(lr.scan_min_hz, lr.scan_max_hz, n, &lr.scan_plan, &bw);
    lr.scan_plan_n = n;
    lr.scan_look = 0;
    lr.scan_bin = 0;

    /* Nothing is demodulated: the rate and deviation only have to be ones the
       part takes with this filter. */
    uint32_t rate = bw.hz / 4, dev = bw.hz / 8;
    if (rate < 600) rate = 600;
    if (dev < 600)  dev = 600;
    esp_err_t err;
    unsigned tries = 0;
    do {
        err = setup_radio(LR20XX_PKT_TYPE_GFSK, lr.scan_min_hz);
        if (err == ESP_OK)
            err = step_lr(lr20xx_gfsk_set_modulation(rate, 0, bw.index, dev), "SetGfskModulationParams");
        if (err == ESP_OK) err = lr20xx_dcdc_workaround_set();
    } while (retry_gate(tries++, err, "sweep setup"));
    if (err == ESP_OK) s_reset_seen = false;
    return err;
}

static esp_err_t lr_scan_begin(uint32_t min_hz, uint32_t max_hz)
{
    /* The same band again is nothing to do: the waterfall asks once a frame.
       A different band while sweeping is retuned below, in place. */
    if (lr.scanning && min_hz == lr.scan_min_hz && max_hz == lr.scan_max_hz) return ESP_OK;
    if (!lr.cfg_valid || lr.fsk_active || s_exp_active || lr20xx_modes_active()) return ESP_ERR_INVALID_STATE;
    /* Never on top of a transmission. */
    if (lr.tx_busy) return ESP_ERR_INVALID_STATE;
    if (max_hz <= min_hz) return ESP_ERR_INVALID_ARG;
    /* 150-1100 MHz on the LF input, 1500-2500 MHz on the HF one. */
    if (!ls_lora_rx_range_ok(lr_caps(), min_hz, max_hz)) return ESP_ERR_INVALID_ARG;

    const bool retune = lr.scanning;
    if (!retune) lr.restore_rx = lr.rx_mode || lr.rearm;
    lr.programmed = false;
    lr.rx_mode = false;
    lr.rearm = false;
    lr.scan_min_hz = min_hz;
    lr.scan_max_hz = max_hz;

    const esp_err_t err = scan_configure(LS_LORA_SCAN_BINS);
    if (err != ESP_OK) {
        /* A first begin that fails gives LoRa the part back; a retune that
           fails ends the sweep, which does the same. */
        if (retune) (void)lr_scan_end();
        else        (void)restore_lora(lr.restore_rx);
        return err;
    }
    lr.scanning = true;
    return ESP_OK;
}

static void lr_scan_profile(ls_lora_scan_prof_t *out)
{
    if (out) *out = lr.prof;
}

/* The part's RSSI, believed only from a Stat that says it is receiving. */
static bool scan_rssi(float *dbm)
{
    return lr20xx_get_rssi_inst(dbm) == ESP_OK &&
           LR20XX_STAT_MODE(lr20xx_last_stat()) == LR20XX_MODE_RX;
}

static int lr_scan_pass(float *dbm, int n, bool *row_done)
{
    if (row_done) *row_done = false;
    if (!lr.scanning || !dbm || n <= 0) return 0;
    heal();
    if (s_reset_seen) {
        /* The part restarted: the sweep's modulation and tune went with it. */
        s_reset_seen = false;
        lr.scan_plan_n = 0;
    }
    /* A bin count the plan was not made for is planned again, once. */
    if (n != lr.scan_plan_n && scan_configure(n) != ESP_OK) return 0;

    const int looks = lr.scan_plan.looks > 0 ? lr.scan_plan.looks : 1;
    const int k = (lr.scan_look >= 0 && lr.scan_look < looks) ? lr.scan_look : 0;

    uint32_t t_standby = 0, t_tune = 0, t_rx = 0, t_settle = 0, t_rssi = 0;
    int64_t mark;
    int got = 0;
    bool failed = false;
    const int64_t pass_start = esp_timer_get_time();
    for (int i = lr.scan_bin; i < n; i++) {
        const uint32_t hz = ls_lora_scan_look_hz(lr.scan_min_hz, lr.scan_max_hz, n, i, looks, k);

        /* Standby with the crystal left running, as on the SX126x. */
        mark = esp_timer_get_time();
        if (lr20xx_set_standby(true) != ESP_OK) { failed = true; break; }
        t_standby += (uint32_t)(esp_timer_get_time() - mark);

        mark = esp_timer_get_time();
        const uint32_t moved = hz > lr.cal_hz ? hz - lr.cal_hz : lr.cal_hz - hz;
        if (moved >= CALIB_FE_STEP_HZ) {
            if (lr20xx_calib_fe_hz(hz, lr20xx_path_for_hz(hz)) != ESP_OK ||
                lr20xx_check_last_ok() != ESP_OK) { failed = true; break; }
            lr.cal_hz = hz;
        }
        if (lr20xx_set_rf_frequency(hz) != ESP_OK) { failed = true; break; }
        t_tune += (uint32_t)(esp_timer_get_time() - mark);

        mark = esp_timer_get_time();
        if (lr20xx_set_rx(0xFFFFFFu) != ESP_OK) { failed = true; break; }
        t_rx += (uint32_t)(esp_timer_get_time() - mark);

        mark = esp_timer_get_time();
        esp_rom_delay_us(lr.scan_plan.settle_us);
        t_settle += (uint32_t)(esp_timer_get_time() - mark);

        float v = 0;
        mark = esp_timer_get_time();
        if (!scan_rssi(&v)) { failed = true; break; }
        t_rssi += (uint32_t)(esp_timer_get_time() - mark);

        /* The first look of a row writes; every later one keeps the peak. */
        if (k == 0 || v > dbm[i]) dbm[i] = v;
        got++;
        lr.scan_bin = i + 1;
        if (esp_timer_get_time() - pass_start >= SCAN_PASS_BUDGET_US) break;
    }

    if (got) {
        lr.prof.standby_us = t_standby / (uint32_t)got;
        lr.prof.tune_us    = t_tune    / (uint32_t)got;
        lr.prof.rx_us      = t_rx      / (uint32_t)got;
        lr.prof.settle_us  = t_settle  / (uint32_t)got;
        lr.prof.rssi_us    = t_rssi    / (uint32_t)got;
    }

    if (failed) { lr.scan_bin = 0; lr.scan_look = 0; return 0; }
    if (lr.scan_bin == n) {
        lr.scan_bin = 0;
        lr.scan_look = (k + 1) % looks;
        if (row_done) *row_done = (k + 1 == looks);
    }
    return n;
}

static int lr_scan_sweep(float *dbm, int n)
{
    if (!lr.scanning || !dbm || n <= 0) return 0;
    /* Every look of one row, the lock taken a pass at a time, as on the
       SX126x: between passes another task may use the part. */
    ls_lora_hw_lock();
    lr.scan_look = 0;
    lr.scan_bin = 0;
    ls_lora_hw_unlock();
    bool done = false;
    int got = 0;
    while (!done) {
        ls_lora_hw_lock();
        got = lr_scan_pass(dbm, n, &done);
        ls_lora_hw_unlock();
        if (got < n) break;
    }
    return got;
}

static esp_err_t lr_scan_end(void)
{
    if (!lr.scanning) return ESP_OK;
    lr.scanning = false;
    lr.scan_plan_n = 0;          /* the next sweep plans afresh */
    if (!lr.cfg_valid) return ESP_ERR_INVALID_STATE;
    return restore_lora(lr.restore_rx);
}

/* ---- the Mode S session, against the packet paths ------------------------ */

/* The ADS-B app takes the part from the mesh as an FSK session does, so a
   LoRa configuration may be live underneath. The session reprograms all of
   it; ending the session gives LoRa the part back as lr_fsk_end does. */
typedef esp_err_t (*session_start_fn)(const void *arg);

/* Take the part from LoRa for a Mode S or OOK session; a start refused before
   anything was sent leaves LoRa holding it, and one that failed part way
   gives it back. */
static esp_err_t session_begin(session_start_fn start, const void *arg)
{
    if (lr.fsk_active || lr.scanning || lr.tx_busy || s_exp_active || lr20xx_modes_active())
        return ESP_ERR_INVALID_STATE;
    const bool was_programmed = lr.programmed, was_rx = lr.rx_mode, was_rearm = lr.rearm;
    lr.restore_rx = was_rx || was_rearm;
    lr.programmed = false;
    lr.rx_mode = false;
    lr.rearm = false;
    const esp_err_t err = start(arg);
    if (err == ESP_ERR_INVALID_ARG || err == ESP_ERR_INVALID_STATE) {
        /* Refused before a frame went out: LoRa still holds the part. */
        lr.programmed = was_programmed;
        lr.rx_mode = was_rx;
        lr.rearm = was_rearm;
        return err;
    }
    if (err != ESP_OK) (void)restore_lora(lr.restore_rx);
    return err;
}

static esp_err_t session_end(void)
{
    const esp_err_t err = lr20xx_modes_end();
    const esp_err_t restored = restore_lora(lr.restore_rx);
    return err != ESP_OK ? err : restored;
}

static const lr20xx_zwave_channel_t US_ZWAVE[] = {
    {908420000u, 1, 19}, {908400000u, 2, 34}, {916000000u, 3, 8},
};
static esp_err_t exp_arm(void)
{
    esp_err_t err = step_ok(lr20xx_set_standby(false), "SetStandby");
    if (err == ESP_OK) err = step_ok(lr20xx_clear_rx_fifo(), "ClearRxFifo");
    if (err == ESP_OK) err = step_ok(lr20xx_clear_irq(LR20XX_IRQ_ALL), "ClearIrq");
    if (err == ESP_OK) err = step_ok(s_exp_engine == LR20XX_ENGINE_ZWAVE ?
                                    lr20xx_zwave_scan() : lr20xx_set_rx(0xFFFFFFu), "Experimental Rx");
    return err;
}
static esp_err_t exp_program_once(void)
{
    const bool lora = s_exp_engine == LR20XX_ENGINE_LORAMON;
    const uint32_t hz = lora ? (s_exp_preset ? 906875000u : 910525000u) :
                        s_exp_engine == LR20XX_ENGINE_ZWAVE ? 908420000u :
                        s_exp_preset == 2 ? 902600000u + s_exp_channel * 600000u :
                        902200000u + s_exp_channel * 200000u;
    esp_err_t err = setup_radio(lora ? LR20XX_PKT_TYPE_LORA :
                               s_exp_engine == LR20XX_ENGINE_ZWAVE ? LR20XX_PKT_TYPE_ZWAVE : LR20XX_PKT_TYPE_WISUN, hz);
    if (lora) {
        const uint8_t sf = s_exp_preset == 0 ? 7 : s_exp_preset == 1 ? 8 : 9;
        const uint32_t bw = s_exp_preset ? 250000u : 62500u;
        const uint8_t sync = s_exp_preset ? 0x2B : 0x12;
        const lr20xx_lora_bw_t b = lr20xx_lora_bw_nearest(bw);
        const lr20xx_side_det_t sides[] = {
            {sf+1, lr20xx_lora_ldro(sf+1, bw), 0},
            {sf+2, lr20xx_lora_ldro(sf+2, bw), 0},
            {sf+3, lr20xx_lora_ldro(sf+3, bw), 0},
        };
        const uint8_t syncs[] = {sync, sync, sync};
        if (err == ESP_OK) err = step_ok(lr20xx_lora_set_modulation(sf, b.code, 1, lr20xx_lora_ldro(sf, bw)), "LoRa modulation");
        if (err == ESP_OK) err = step_ok(lr20xx_lora_set_packet(16, 255, false, true, false), "LoRa packet");
        if (err == ESP_OK) err = step_ok(lr20xx_lora_set_syncword(sync), "LoRa sync");
        if (err == ESP_OK) err = step_ok(lr20xx_lora_side_config(sf, bw, sides, 3), "LoRa sides");
        if (err == ESP_OK) err = step_ok(lr20xx_lora_side_sync(syncs, 3), "LoRa side sync");
    } else if (s_exp_engine == LR20XX_ENGINE_ZWAVE) {
        if (err == ESP_OK) err = step_ok(lr20xx_zwave_params(1, lr20xx_fsk_rx_bw(200000).index, 0, 255, 0, 32, true), "Z-Wave params");
        if (err == ESP_OK) err = step_ok(lr20xx_zwave_scan_config(US_ZWAVE, 3, 0, true), "Z-Wave scan config");
    } else {
        static const uint8_t modes[] = {1, 2, 4};
        static const uint32_t widths[] = {200000, 250000, 400000};
        if (err == ESP_OK) err = step_ok(lr20xx_wisun_mode(modes[s_exp_preset], lr20xx_fsk_rx_bw(widths[s_exp_preset]).index), "Wi-SUN mode");
        if (err == ESP_OK) err = step_ok(lr20xx_wisun_packet(false, true, true, s_exp_fec, 0, 8, 24), "Wi-SUN packet");
    }
    if (err == ESP_OK) err = lr20xx_dcdc_workaround_set();
    if (err == ESP_OK) err = exp_arm();
    if (err != ESP_OK) (void)lr20xx_set_standby(false);
    return err;
}
static esp_err_t exp_program(void)
{
    esp_err_t err;
    unsigned n = 0;
    do err = exp_program_once(); while (retry_gate(n++, err, "experiment setup"));
    if (err == ESP_OK) s_reset_seen = false;
    return err;
}
esp_err_t lr20xx_exp_begin(lr20xx_engine_t engine, unsigned preset, unsigned channel, unsigned fec)
{
    if (engine > LR20XX_ENGINE_WISUN || preset > 2 || channel > (engine == LR20XX_ENGINE_WISUN && preset == 2 ? 41u : 128u) || fec > 3) return ESP_ERR_INVALID_ARG;
    ls_lora_hw_lock();
    /* Not an LR2021 (an SX126x socket, or an LR2012/LR2022 without the engines): not a state, a fact. */
    if (!s_bound || !s_info.lr2021) { ls_lora_hw_unlock(); return ESP_ERR_NOT_SUPPORTED; }
    if (s_exp_active || lr.fsk_active || lr.scanning || lr.tx_busy || lr20xx_modes_active()) {
        ls_lora_hw_unlock(); return ESP_ERR_INVALID_STATE;
    }
    lr.restore_rx = lr.rx_mode || lr.rearm;
    lr.programmed = lr.rx_mode = lr.rearm = false;
    s_exp_reset = false;
    s_exp_engine = engine; s_exp_preset = preset; s_exp_channel = channel; s_exp_fec = fec;
    const esp_err_t err = exp_program();
    s_exp_active = err == ESP_OK;
    if (err != ESP_OK) (void)restore_lora(lr.restore_rx);
    ls_lora_hw_unlock();
    return err;
}
static int exp_poll_locked(uint8_t *buf, size_t size, lr20xx_engine_packet_t *out)
{
    if (!s_exp_active || !buf || !size || !out) return -1;
    heal();
    lr20xx_status_t st;
    if (lr20xx_get_status(&st) != ESP_OK || !lr20xx_stat_plausible(st.stat)) return -1;
    if (s_exp_reset || take_reset(st.stat)) {
        s_exp_reset = true;
        if (exp_program() != ESP_OK) return -1;
        s_exp_reset = false;
        return 0;
    }
    if (!(st.irq & LR20XX_IRQ_RX_DONE)) {
        if (st.irq) (void)lr20xx_clear_irq(st.irq);
        if (LR20XX_STAT_MODE(st.stat) != LR20XX_MODE_RX) return exp_arm() == ESP_OK ? 0 : -1;
        return 0;
    }
    if (st.irq & (LR20XX_IRQ_CRC_ERROR | LR20XX_IRQ_LEN_ERROR)) {
        (void)exp_arm(); return -1;
    }
    esp_err_t err;
    memset(out, 0, sizeof(*out));
    if (s_exp_engine == LR20XX_ENGINE_LORAMON) {
        lr20xx_lora_pkt_status_t p;
        err = lr20xx_get_lora_packet_status(&p);
        if (err == ESP_OK) {
            out->length = p.length; out->rssi_dbm = p.rssi_dbm; out->snr_db = p.snr_db;
            out->cr = p.cr; out->detector = p.detector; out->fei_hz = p.fei_hz;
        }
    } else err = lr20xx_engine_status(s_exp_engine == LR20XX_ENGINE_WISUN, out);
    int n = -1;
    if (err == ESP_OK) {
        /* The DMA FIFO is 256 bytes. Longer Wi-SUN PSDUs are counted with
           their PHR, then dropped; they must not be parsed as complete MACs. */
        if (out->length > size || out->length > 255) n = -2;
        else if (out->length && lr20xx_read_rx_fifo(buf, out->length) == ESP_OK) n = out->length;
    }
    if (exp_arm() != ESP_OK) return -1;
    return n;
}
int lr20xx_exp_poll(uint8_t *buf, size_t size, lr20xx_engine_packet_t *out)
{ LOCKED_RET(int, exp_poll_locked(buf, size, out)); }
esp_err_t lr20xx_exp_end(void)
{
    ls_lora_hw_lock();
    esp_err_t err = ESP_OK;
    if (s_exp_active) {
        err = lr20xx_set_standby(false);
        (void)lr20xx_clear_irq(LR20XX_IRQ_ALL); (void)lr20xx_clear_rx_fifo();
        s_exp_active = false;
        const esp_err_t restored = restore_lora(lr.restore_rx);
        if (err == ESP_OK) err = restored;
    }
    ls_lora_hw_unlock(); return err;
}

typedef struct { uint32_t freq_hz; int gain_step; } modes_args_t;

static esp_err_t modes_start(const void *arg)
{
    const modes_args_t *a = arg;
    return lr20xx_modes_begin(a->freq_hz, a->gain_step);
}

static esp_err_t lr_modes_begin(uint32_t freq_hz, int gain_step)
{
    const modes_args_t a = { freq_hz, gain_step };
    return session_begin(modes_start, &a);
}

static bool lr_modes_active(void) { return lr20xx_modes_active() && !lr20xx_ook_rx_active(); }

static int lr_modes_poll(uint8_t *buf, size_t size, float *rssi_dbm)
{ return lr_modes_active() ? lr20xx_modes_poll(buf, size, rssi_dbm) : -1; }
static esp_err_t lr_modes_set_gain(int gain_step) { return lr20xx_modes_set_gain(gain_step); }

static esp_err_t lr_modes_end(void)
{
    return lr_modes_active() ? session_end() : ESP_OK;
}

static esp_err_t ook_start(const void *arg)
{
    const ls_ook_cfg_t *c = arg;
    const lr20xx_ook_rx_cfg_t r = {
        .freq_hz = c->freq_hz,
        .bitrate_bps = c->bitrate,
        .rx_bw_hz = c->bandwidth_hz,
        .pattern = c->pattern,
        .pattern_bits = c->pattern_bits,
        .repeats = c->repeats,
        .frame_bytes = c->frame_bytes,
        .gain_step = c->gain_step,
        .boost = c->boost,
    };
    return lr20xx_ook_rx_begin(&r);
}

static esp_err_t lr_ook_begin(const ls_ook_cfg_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    return session_begin(ook_start, cfg);
}

static bool lr_ook_active(void) { return lr20xx_ook_rx_active(); }

static int lr_ook_poll(uint8_t *buf, size_t size, float *rssi_dbm)
{ return lr_ook_active() ? lr20xx_modes_poll(buf, size, rssi_dbm) : -1; }

static esp_err_t lr_ook_end(void)
{
    return lr_ook_active() ? session_end() : ESP_OK;
}
static esp_err_t lr_modes_tuning(ls_lora_modes_tuning_t *out)
{
    lr20xx_modes_tuning_t t;
    const esp_err_t err = lr20xx_modes_tuning(&t);
    if (err == ESP_OK && out) {
        out->gain_step = t.gain_step;
        out->boost = t.boost;
        out->rx_bw_hz = t.rx_bw_hz;
        out->thresh_override = t.thresh_override;
        out->thresh_level = t.thresh_level;
    }
    return err;
}
static esp_err_t lr_modes_set_boost(int boost) { return lr20xx_modes_set_boost(boost); }
static esp_err_t lr_modes_set_bw(uint32_t hz, uint32_t *chosen) { return lr20xx_modes_set_rx_bw(hz, chosen); }
static esp_err_t lr_modes_set_thresh(int level_db, bool automatic)
{
    return automatic ? lr20xx_modes_clear_threshold() : lr20xx_modes_set_threshold(level_db);
}
static esp_err_t lr_modes_read_thresh(int *raw) { return lr20xx_modes_read_threshold(raw); }

static void lr_diagnostics(void)
{
    /* Gather with the bus held, print with it free: a console that is slow to
       drain must not stall the Mode S session's polls behind a printf. */
    ls_lora_hw_lock();
    const bool experimental = s_exp_active;
    const bool session = lr20xx_modes_active() || experimental;
    lr20xx_status_t st = { 0, 0 };
    const esp_err_t err = lr20xx_get_status(&st);
    /* GetStatus clears the reset source, and the session's poll is what reads
       it. If it was set, the poll must still see it. */
    if (session && err == ESP_OK && LR20XX_STAT_RESET_SRC(st.stat) != 0) {
        if (experimental) s_exp_reset = true;
        else ms.reset_seen = true;
    }
    uint8_t major = s_info.fw_major, minor = s_info.fw_minor;
    esp_err_t verr = ESP_OK, eerr = ESP_OK;
    uint16_t errors = 0;
    lr20xx_modes_stats_t mstat;
    lr20xx_modes_stats(&mstat);
    lr20xx_modes_tuning_t tune = { 0, 0, 0, false, 0 };
    if (session) (void)lr20xx_modes_tuning(&tune);
    if (err == ESP_OK && !session) {
        verr = lr20xx_get_version(&major, &minor);
        eerr = lr20xx_get_errors(&errors);
    }
    const int busy = ls_lora_hw_busy_level();
    const ls_lora_cfg_t cfg = lr.cfg;
    /* Read only, never ResetRxStats, and not asked of a part that has just
       restarted: it has no packet type to count in. */
    lr20xx_gfsk_rx_stats_t fstat = { 0 };
    esp_err_t ferr = ESP_ERR_INVALID_STATE;
    if (err == ESP_OK && !session && lr.fsk_active && !lr.tx_busy &&
        LR20XX_STAT_RESET_SRC(st.stat) == 0)
        ferr = lr20xx_get_gfsk_rx_stats(&fstat);
    const bool cfg_valid = lr.cfg_valid, programmed = lr.programmed, rx = lr.rx_mode;
    const bool fsk = lr.fsk_active, scanning = lr.scanning, tx = lr.tx_busy;
    const bool stream = lr.fsk_stream;
    ls_lora_hw_unlock();

    if (err != ESP_OK) {
        printf("lora: GetStatus failed: %s\n", esp_err_to_name(err));
        return;
    }
    if (experimental) {
        printf("lora: LR2021 experiment receive session, stat=0x%04X irq=0x%08lX busy=%d\n",
               (unsigned)st.stat, (unsigned long)st.irq, busy);
        return;
    }
    if (session) {
        /* The session owns the part. Version, error and register reads are not
           sent: the version is the one read at start-up, and nothing here
           changes the chip's state. */
        printf("lora: %s FW %u.%u (read at start) stat=0x%04X mode=%u cmd=%u reset-src=%u"
               " irq-pending=%u irq=0x%08lX busy=%d\n",
               lr_name(), (unsigned)major, (unsigned)minor, (unsigned)st.stat,
               (unsigned)LR20XX_STAT_MODE(st.stat), (unsigned)LR20XX_STAT_CMD(st.stat),
               (unsigned)LR20XX_STAT_RESET_SRC(st.stat), (unsigned)LR20XX_STAT_IRQ(st.stat),
               (unsigned long)st.irq, busy);
        printf("lora: %s session active - only GetStatus was sent; the chip's error word"
               " and version are not re-read so the session is not disturbed\n",
               lr20xx_ook_rx_active() ? "OOK" : "Mode S");
    } else {
        printf("lora: %s FW %u.%u%s stat=0x%04X mode=%u cmd=%u reset-src=%u irq-pending=%u"
               " irq=0x%08lX errors=0x%04X%s busy=%d\n",
               lr_name(), (unsigned)major, (unsigned)minor, verr == ESP_OK ? "" : "(read failed)",
               (unsigned)st.stat, (unsigned)LR20XX_STAT_MODE(st.stat),
               (unsigned)LR20XX_STAT_CMD(st.stat), (unsigned)LR20XX_STAT_RESET_SRC(st.stat),
               (unsigned)LR20XX_STAT_IRQ(st.stat), (unsigned long)st.irq, (unsigned)errors,
               eerr == ESP_OK ? "" : "(read failed)", busy);
    }
    printf("lora: LR20xx backend: LoRa packets, FSK sessions, the RSSI sweep and a"
           " receive-only Mode S session. Transmit is on the LF path below 1 GHz only."
           " The IRQ word is polled over SPI.\n");
    if (cfg_valid)
        printf("lora: LoRa %.4f MHz SF%u BW%lu CR4/%u %+d dBm sync 0x%02X: %s%s\n",
               cfg.freq_hz / 1e6, (unsigned)cfg.sf, (unsigned long)cfg.bw_hz, (unsigned)cfg.cr,
               (int)cfg.power_dbm, (unsigned)cfg.sync_word,
               tx ? "transmitting" : rx ? "receiving" : "idle",
               programmed ? "" : ", reprogrammed on next use");
    else
        printf("lora: LoRa not configured\n");
    if (fsk) printf("lora: FSK %s active\n", stream ? "stream" : "session");
    if (fsk && ferr == ESP_OK)
        printf("lora: FSK rx stats: received=%u crc=%u length=%u preamble=%u sync-ok=%u"
               " sync-fail=%u timeout=%u\n", (unsigned)fstat.received, (unsigned)fstat.crc_errors,
               (unsigned)fstat.length_errors, (unsigned)fstat.preamble_detections,
               (unsigned)fstat.sync_ok, (unsigned)fstat.sync_fail, (unsigned)fstat.timeouts);
    if (scanning) printf("lora: sweep active\n");
    if (session) {
        printf("lora: %s session on: polls=%lu frames=%lu rearms=%lu resets=%lu"
               " overflows=%lu chip-errors=%lu bus-errors=%lu\n",
               lr20xx_ook_rx_active() ? "OOK" : "Mode S", (unsigned long)mstat.polls, (unsigned long)mstat.frames, (unsigned long)mstat.rearms,
               (unsigned long)mstat.resets, (unsigned long)mstat.overflows,
               (unsigned long)mstat.chip_errors, (unsigned long)mstat.bus_errors);
        printf("lora: Mode S tuning: gain step %d%s, boost %d, rx bw %lu Hz, detection threshold %s",
               tune.gain_step, tune.gain_step == 0 ? " (AGC)" : "", tune.boost,
               (unsigned long)tune.rx_bw_hz, tune.thresh_override ? "override" : "chip's own");
        if (tune.thresh_override) printf(" %d dB", tune.thresh_level);
        printf("\n");
    }
}

static void lr_stop(void)
{
    /* The part may have lost power: forget the sessions and the
       configuration, send it nothing. */
    s_exp_active = false;
    memset(&ms, 0, sizeof(ms));
    memset(&lr, 0, sizeof(lr));
    s_bound = false;
    memset(&s_info, 0, sizeof(s_info));
    s_stat = 0;
    s_irq = 0;
    s_rf_hz = 0;
    s_pa_lf = false;
    s_agc_forced = false;
    s_reset_seen = false;
    s_fail_run = 0;
    s_recovered = false;
    s_recover_gap_us = RECOVER_GAP_MIN_US;
}

const ls_lora_ops_t ls_lora_lr20xx_ops = {
    .kind = LS_LORA_CHIP_LR20XX,
    .name = "LR20xx",
    .caps = lr_caps,
    .status = lr_status,
    .read_reg = lr_read_reg,
    .configure = lr_configure,
    .cfg = lr_cfg,
    .receive = lr_receive,
    .is_receiving = lr_is_receiving,
    .send = lr_send,
    .send_done = lr_send_done,
    .poll = lr_poll,
    .rssi_inst = lr_rssi_inst,
    .airtime_ms = lr_airtime_ms,
    .fsk_begin = lr_fsk_begin,
    .fsk_poll = lr_fsk_poll,
    .fsk_send = lr_fsk_send,
    .fsk_receive = lr_fsk_receive,
    .fsk_end = lr_fsk_end,
    .fsk_active = lr_fsk_active,
    .fsk_retune = lr_fsk_retune,
    .fsk_stream_read = lr_fsk_stream_read,
    .scan_begin = lr_scan_begin,
    .scan_sweep = lr_scan_sweep,
    .scan_pass = lr_scan_pass,
    .scan_end = lr_scan_end,
    .scanning = lr_scanning,
    .scan_profile = lr_scan_profile,
    .modes_begin = lr_modes_begin,
    .modes_poll = lr_modes_poll,
    .modes_set_gain = lr_modes_set_gain,
    .modes_end = lr_modes_end,
    .modes_active = lr_modes_active,
    .modes_tuning = lr_modes_tuning,
    .modes_set_boost = lr_modes_set_boost,
    .modes_set_bw = lr_modes_set_bw,
    .modes_set_thresh = lr_modes_set_thresh,
    .modes_read_thresh = lr_modes_read_thresh,
    .ook_begin = lr_ook_begin,
    .ook_poll = lr_ook_poll,
    .ook_end = lr_ook_end,
    .ook_active = lr_ook_active,
    .diagnostics = lr_diagnostics,
    .stop = lr_stop,
    .name_fn = lr_name,
};

#else  /* no LoRa socket, or this board does not probe for an LR20xx */

typedef int ls_lora_lr20xx_not_built_t;

#endif
