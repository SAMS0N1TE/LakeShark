/* Programs on the LR2021's own CPU. Alex Beal's lr2021-re found the 32 MHz
 * ARCv2 core inside the part and how to run code from its 4 KB Patch RAM;
 * lr2021_pram_image.h is his program (MIT). It adds ActivatePram (0x012D)
 * sub-commands: 9 captures raw I/Q, runs a 256-point FFT and converts to dB
 * on the chip, 1 steps the receiver across a band summing instant RSSI.
 * The host reads 256 bytes a frame back instead of the samples, and does no
 * FFT. Included by the LR20xx board driver for its lock, state and recovery.
 *
 * Nothing here loads Semtech's own patch, so all of PRAM is free. NRESET
 * clears it: every session uploads the program, and ends with the same
 * reset and LoRa reprogram the I/Q stream ends with. */
#include "lr2021_pram_image.h"
#include "lr2021_scan_image.h"
#include "lr2021_ddc_image.h"

#define COP_PRAM_HOST   0x801000u   /* write window */
#define COP_PRAM_CPU    0x101000u   /* where the program runs and reads back */
#define COP_SWEEP_OUT   0x101C80u   /* entry 1: u16 RSSI sums */
#define COP_SWEEP_MAX   400u        /* points per entry-1 call (its buffer holds 448) */
#define COP_FFT_DB      0x807B00u   /* entry 9: 256 x u8 half-dB */

typedef struct {
    uint32_t sessions, frames, sweeps, points, uploads, upload_fail;
    uint32_t last_chip_us, max_chip_us, last_read_us;
    esp_err_t last_error;
} cop_stats_t;
static cop_stats_t s_cop;

/* One command on the packet buffer, the answer after a BUSY wait as long as
   the chip program needs (a 400-point sweep takes most of a second). */
static esp_err_t cop_call_locked(const uint8_t *args, size_t n, uint8_t *out, size_t m, int busy_ms)
{
    uint8_t *pkt = ls_lora_hw_pkt();
    if (!pkt) return ESP_ERR_INVALID_STATE;
    if (2 + n > FIFO_READ_MAX || 2 + m > FIFO_READ_MAX) return ESP_ERR_INVALID_SIZE;
    if (!ls_lora_hw_wait_not_busy(20)) { s_fail_run++; return ESP_ERR_TIMEOUT; }
    pkt[0] = 0x01; pkt[1] = 0x2D;
    memcpy(pkt + 2, args, n);
    esp_err_t err = frame(pkt, 2 + n);
    if (err != ESP_OK) return err;
    if (!ls_lora_hw_wait_not_busy(busy_ms)) { s_fail_run++; return ESP_ERR_TIMEOUT; }
    memset(pkt, 0, 2 + m);
    err = frame(pkt, 2 + (m ? m : 2));
    if (err != ESP_OK) return err;
    if (!lr20xx_stat_plausible(s_stat) || s_stat == 0x0000 || LR20XX_STAT_CMD(s_stat) < LR20XX_CMD_OK)
        return ESP_ERR_INVALID_RESPONSE;
    if (m && out) memcpy(out, pkt + 2, m);
    s_fail_run = 0;
    return ESP_OK;
}

/* ReadRegMem32 of up to 64 words in one answer frame (32 in the capture SPI mode's\n   64-byte frames: callers there ask for 15); bytes in memory order. */
static esp_err_t cop_mem_read_locked(uint32_t addr, uint8_t *out, unsigned words)
{
    uint8_t *pkt = ls_lora_hw_pkt();
    if (!pkt) return ESP_ERR_INVALID_STATE;
    if (!words || words > 64) return ESP_ERR_INVALID_ARG;
    if (!ls_lora_hw_wait_not_busy(20)) { s_fail_run++; return ESP_ERR_TIMEOUT; }
    pkt[0] = (uint8_t)(LR20XX_OP_READ_REG >> 8); pkt[1] = (uint8_t)LR20XX_OP_READ_REG;
    pkt[2] = (uint8_t)(addr >> 16); pkt[3] = (uint8_t)(addr >> 8); pkt[4] = (uint8_t)addr;
    pkt[5] = (uint8_t)words;
    esp_err_t err = frame(pkt, 6);
    if (err != ESP_OK) return err;
    if (!ls_lora_hw_wait_not_busy(20)) { s_fail_run++; return ESP_ERR_TIMEOUT; }
    memset(pkt, 0, 2 + 4 * words);
    err = frame(pkt, 2 + 4 * words);
    if (err != ESP_OK) return err;
    if (!lr20xx_stat_plausible(s_stat) || s_stat == 0x0000 || LR20XX_STAT_CMD(s_stat) < LR20XX_CMD_OK)
        return ESP_ERR_INVALID_RESPONSE;
    /* Words come big-endian off the wire. */
    for (unsigned i = 0; i < words; i++)
        for (unsigned b = 0; b < 4; b++) out[4 * i + b] = pkt[2 + 4 * i + 3 - b];
    return ESP_OK;
}

/* The two programs: pram.bin (sweep, FFT) answers entry 2 "LRC1", scan.bin
   (the channel scanner) "SCN2". One is in PRAM at a time. */
typedef struct { const uint32_t *words; unsigned n; const uint8_t *hello; size_t hello_n; const char *magic; } cop_image_t;
static const uint8_t COP_HELLO_PRAM[2] = {2, 0xAB}, COP_HELLO_SCAN[1] = {2};
static const cop_image_t COP_PRAM = {lr2021_pram_image, LR2021_PRAM_WORDS, COP_HELLO_PRAM, 2, "LRC1"};
static const cop_image_t COP_SCAN = {lr2021_scan_image, LR2021_SCAN_WORDS, COP_HELLO_SCAN, 1, "SCN2"};
static const cop_image_t COP_DDC = {lr2021_ddc_image, LR2021_DDC_WORDS, COP_HELLO_SCAN, 1, "DDC2"};
static const cop_image_t *s_cop_image;

static bool cop_hello_locked(const cop_image_t *img)
{
    uint8_t r[6] = {0};
    return cop_call_locked(img->hello, img->hello_n, r, img == &COP_PRAM ? 6 : 4, 20) == ESP_OK &&
           !memcmp(r, img->magic, 4);
}

/* Standby, the image into PRAM, ActivatePram, the image read back where it
   runs, and its hello. A different program already active goes first, by
   the reset (an active program's write window moves). Under the lock. */
static esp_err_t cop_upload_image_locked(const cop_image_t *img)
{
    if (s_cop_loaded && s_cop_image == img) return ESP_OK;
    if (s_cop_loaded) { s_recovered = false; (void)recover_locked("chip program swap"); }
    s_cop.uploads++;
    esp_err_t err = step_ok(lr20xx_set_standby(false), "chip program standby");
    for (unsigned i = 0; err == ESP_OK && i < img->n; i++)
        err = lr20xx_write_reg32(COP_PRAM_HOST + 4u * i, img->words[i]);
    if (err == ESP_OK) {
        const uint8_t activate = 0;
        err = lr20xx_write(0x012D, &activate, 1);
        if (err == ESP_OK) err = lr20xx_check_last_ok();
    }
    for (unsigned i = 0; err == ESP_OK && i < img->n; i += 32) {
        LORA_TABLE static uint8_t b[128];
        const unsigned n = img->n - i > 32 ? 32 : img->n - i;
        err = cop_mem_read_locked(COP_PRAM_CPU + 4u * i, b, n);
        for (unsigned j = 0; err == ESP_OK && j < n; j++) {
            const uint32_t w = b[4 * j] | (uint32_t)b[4 * j + 1] << 8 | (uint32_t)b[4 * j + 2] << 16 |
                               (uint32_t)b[4 * j + 3] << 24;
            if (w != img->words[i + j]) err = ESP_ERR_INVALID_CRC;
        }
    }
    if (err == ESP_OK && !cop_hello_locked(img)) err = ESP_ERR_INVALID_RESPONSE;
    s_cop_loaded = err == ESP_OK;
    s_cop_image = s_cop_loaded ? img : NULL;
    if (err != ESP_OK) { s_cop.upload_fail++; ESP_LOGW(TAG, "chip program upload: %s", esp_err_to_name(err)); }
    return err;
}
static esp_err_t cop_upload_locked(void) { return cop_upload_image_locked(&COP_PRAM); }

/* The same conditions the I/Q stream asks for: the 1.24 ROM the program was
   built against, and the ordinary LoRa owner with nothing else on the part. */
static esp_err_t cop_state_ok_locked(void)
{
    return s_bound && s_info.lr2021 && s_info.fw_major == 1 && s_info.fw_minor == 24 &&
           lr.cfg_valid && !lr.tx_busy && !lr.scanning && !lr.fsk_active && !ms.active &&
           !s_exp_active ? ESP_OK : ESP_ERR_INVALID_STATE;
}

/* The part back to LoRa the way stream_start leaves it. */
static esp_err_t cop_finish_locked(bool rx, const char *why)
{
    (void)lr20xx_set_standby(false);
    s_recovered = false;
    esp_err_t err = recover_locked(why);
    if (err == ESP_OK) {
        lr.rearm = false;
        err = program_lora();
        if (err == ESP_OK && rx) err = lr_receive();
    }
    return err;
}

/* Receive set up for the capture: LoRa modulation at the bandwidth that sets
   the sample rate (4 x bw: 62.5 kHz -> 250 kS/s), then raw packet type 0x0B.
   The program's entry 9 runs the capture and SetRx itself. */
static esp_err_t cop_program_rx_locked(uint32_t hz, uint32_t bw_hz, uint8_t gain)
{
    const lr20xx_rx_path_t path = lr20xx_path_for_hz(hz);
    const lr20xx_lora_bw_t bw = lr20xx_lora_bw_nearest(bw_hz);
    const uint8_t lora = LR20XX_PKT_TYPE_LORA, raw = 0x0B;
    s_pa_lf = false;
    esp_err_t err = step_ok(lr20xx_clear_errors(), "chip program errors");
    if (err == ESP_OK) err = step_ok(lr20xx_write(0x0207, &lora, 1), "chip program packet type");
    if (err == ESP_OK) err = lr20xx_calib_fe_hz(hz, path);
    if (err == ESP_OK) err = lr20xx_wait_ready(200);
    if (err == ESP_OK) err = step_ok(lr20xx_set_rf_frequency(hz), "chip program tune");
    if (err == ESP_OK) err = step_ok(lr20xx_set_rx_path(path, path == LR20XX_PATH_HF ? 4 : 0), "chip program input");
    if (err == ESP_OK) err = step_ok(lr20xx_lora_set_modulation(7, bw.code, 1, false), "chip program modulation");
    if (err == ESP_OK) err = step_ok(lr20xx_set_agc_gain(gain), "chip program gain");
    if (err == ESP_OK) err = step_ok(lr20xx_write(0x0207, &raw, 1), "chip program raw type");
    return err;
}

esp_err_t lr20xx_chip_spectrum(const lr20xx_chip_spec_cfg_t *cfg, lr20xx_chip_spec_sink_t sink,
                               void *ctx, lr20xx_chip_result_t *out)
{
    lr20xx_chip_result_t res = {0};
    if (!cfg || !sink || cfg->nseg < 1 || cfg->nseg > 4 || cfg->gain > 13 ||
        cfg->hz < LR20XX_FREQ_MIN_HZ || cfg->hz > LR20XX_FREQ_MAX_HZ) {
        if (out) { *out = res; out->error = ESP_ERR_INVALID_ARG; }
        return ESP_ERR_INVALID_ARG;
    }
    /* The I/Q stream's ownership flag: a decoder or a second waterfall asking
       for the part meanwhile is refused instead of waiting on the lock. */
    bool expected = false;
    if (!atomic_compare_exchange_strong(&s_iq_live_active, &expected, true)) {
        if (out) { *out = res; out->error = ESP_ERR_INVALID_STATE; }
        return ESP_ERR_INVALID_STATE;
    }
    ls_lora_hw_lock();
    esp_err_t err = cop_state_ok_locked();
    const bool rx = lr.rx_mode || lr.rearm;
    const bool touched = err == ESP_OK;
    if (touched) s_cop.sessions++;
    if (err == ESP_OK) err = cop_upload_locked();
    if (err == ESP_OK) err = cop_program_rx_locked(cfg->hz, cfg->bw_hz, cfg->gain);
    LORA_TABLE static uint8_t db[256];   /* console callers have a small stack */
    while (err == ESP_OK) {
        const uint8_t p[3] = {9, cfg->nseg, 0};
        uint8_t r[4] = {0};
        const int64_t t0 = esp_timer_get_time();
        err = cop_call_locked(p, 3, r, 4, 50);
        const int64_t t1 = esp_timer_get_time();
        if (err == ESP_OK && !r[1]) err = ESP_ERR_INVALID_RESPONSE;
        if (err == ESP_OK) err = cop_mem_read_locked(COP_FFT_DB, db, 32);
        if (err == ESP_OK) err = cop_mem_read_locked(COP_FFT_DB + 128u, db + 128, 32);
        if (err != ESP_OK) break;
        res.frames++;
        s_cop.frames++;
        s_cop.last_chip_us = (uint32_t)(t1 - t0);
        if (s_cop.last_chip_us > s_cop.max_chip_us) s_cop.max_chip_us = s_cop.last_chip_us;
        s_cop.last_read_us = (uint32_t)(esp_timer_get_time() - t1);
        if (!sink(ctx, db, s_cop.last_chip_us)) break;
    }
    res.error = err;
    s_cop.last_error = err;
    if (touched) res.recovery_error = cop_finish_locked(rx, "chip spectrum finished");
    ls_lora_hw_unlock();
    atomic_store(&s_iq_live_active, false);
    if (out) *out = res;
    return err != ESP_OK ? err : res.recovery_error;
}

/* Entry 1 over n points: the part retunes in continuous receive and sums
   `avg` instant RSSI readings per point after `dwell` x16 loop iterations
   (1000 is about 1.4 ms, what a 62.5 kHz filter needs to settle). */
esp_err_t lr20xx_chip_sweep(const lr20xx_chip_sweep_cfg_t *cfg, float *dbm, lr20xx_chip_result_t *out)
{
    lr20xx_chip_result_t res = {0};
    const uint32_t avg = cfg ? cfg->avg : 0;
    if (!cfg || !dbm || !cfg->points || !cfg->step_hz || !avg || avg > 64 || cfg->bw_hz == 0 ||
        cfg->start_hz < LR20XX_FREQ_MIN_HZ ||
        cfg->start_hz + (uint64_t)cfg->step_hz * (cfg->points - 1) > LR20XX_FREQ_MAX_HZ) {
        if (out) { *out = res; out->error = ESP_ERR_INVALID_ARG; }
        return ESP_ERR_INVALID_ARG;
    }
    ls_lora_hw_lock();
    esp_err_t err = cop_state_ok_locked();
    const bool rx = lr.rx_mode || lr.rearm;
    const bool touched = err == ESP_OK;
    if (err == ESP_OK) err = cop_upload_locked();
    /* The filter of the scan plan, on GFSK as the host-driven sweep has it;
       one front-end calibration at the start of each stretch of the band. */
    uint32_t done = 0, cal_hz = 0;
    if (err == ESP_OK) {
        const lr20xx_rx_bw_t bw = lr20xx_fsk_rx_bw(cfg->bw_hz);
        uint32_t rate = bw.hz / 4, dev = bw.hz / 8;
        if (rate < 600) rate = 600;
        if (dev < 600) dev = 600;
        const uint8_t gfsk = LR20XX_PKT_TYPE_GFSK;
        err = step_ok(lr20xx_write(0x0207, &gfsk, 1), "chip sweep packet type");
        if (err == ESP_OK) err = step_lr(lr20xx_gfsk_set_modulation(rate, 0, bw.index, dev), "chip sweep modulation");
    }
    while (err == ESP_OK && done < cfg->points) {
        const uint32_t f = cfg->start_hz + done * cfg->step_hz;
        uint32_t n = cfg->points - done;
        if (n > COP_SWEEP_MAX) n = COP_SWEEP_MAX;
        /* Never across the LF/HF split, and a new calibration each 10 MHz. */
        uint32_t span = CALIB_FE_STEP_HZ / cfg->step_hz;
        if (span < 1) span = 1;
        if (n > span) n = span;
        while (n > 1 && lr20xx_path_for_hz(f + (n - 1) * cfg->step_hz) != lr20xx_path_for_hz(f)) n--;
        const uint32_t mid = f + (n / 2) * cfg->step_hz;
        if (!cal_hz || (mid > cal_hz ? mid - cal_hz : cal_hz - mid) >= CALIB_FE_STEP_HZ / 2 ||
            lr20xx_path_for_hz(mid) != lr20xx_path_for_hz(cal_hz)) {
            err = step_ok(lr20xx_set_standby(false), "chip sweep standby");
            if (err == ESP_OK) err = lr20xx_calib_fe_hz(mid, lr20xx_path_for_hz(mid));
            if (err == ESP_OK) err = lr20xx_wait_ready(200);
            if (err == ESP_OK) err = step_ok(lr20xx_set_rf_frequency(f), "chip sweep tune");
            if (err == ESP_OK) err = step_ok(lr20xx_set_rx_path(lr20xx_path_for_hz(mid), 0), "chip sweep input");
            if (err == ESP_OK) err = lr20xx_set_rx(0xFFFFFFu);
            if (err != ESP_OK) break;
            cal_hz = mid;
            /* The first point after entering receive reads garbage: settle on one. */
            const uint8_t p1[15] = {1, (uint8_t)(f >> 24), (uint8_t)(f >> 16), (uint8_t)(f >> 8), (uint8_t)f,
                0, 0, 0, 0, 0, 1, (uint8_t)(cfg->dwell >> 8), (uint8_t)cfg->dwell, 0, 1};
            uint8_t r1[2];
            err = cop_call_locked(p1, sizeof(p1), r1, 2, 50);
            if (err != ESP_OK) break;
        }
        const uint32_t st = cfg->step_hz;
        const uint8_t p[15] = {1, (uint8_t)(f >> 24), (uint8_t)(f >> 16), (uint8_t)(f >> 8), (uint8_t)f,
            (uint8_t)(st >> 24), (uint8_t)(st >> 16), (uint8_t)(st >> 8), (uint8_t)st,
            (uint8_t)(n >> 8), (uint8_t)n, (uint8_t)(cfg->dwell >> 8), (uint8_t)cfg->dwell,
            (uint8_t)(avg >> 8), (uint8_t)avg};
        uint8_t r[2] = {0};
        /* About 2 ms a point at dwell 1000; a generous ceiling over that. */
        err = cop_call_locked(p, sizeof(p), r, 2, (int)(n * (cfg->dwell / 300u + 4u) + 50u));
        if (err == ESP_OK && (((uint32_t)r[0] << 8) | r[1]) != n) err = ESP_ERR_INVALID_RESPONSE;
        for (uint32_t i = 0; err == ESP_OK && i < n; i += 64) {
            LORA_TABLE static uint8_t b[128];
            const uint32_t k = n - i > 64 ? 64 : n - i;
            err = cop_mem_read_locked(COP_SWEEP_OUT + 2u * i, b, (k + 1) / 2);
            for (uint32_t j = 0; err == ESP_OK && j < k; j++)
                dbm[done + i + j] = -(float)(b[2 * j] | (uint32_t)b[2 * j + 1] << 8) / (2.0f * (float)avg);
        }
        if (err == ESP_OK) done += n;
    }
    res.frames = done;
    s_cop.sweeps++;
    s_cop.points += done;
    res.error = err;
    s_cop.last_error = err;
    if (touched) res.recovery_error = cop_finish_locked(rx, "chip sweep finished");
    ls_lora_hw_unlock();
    if (out) *out = res;
    return err != ESP_OK ? err : res.recovery_error;
}

/* ---- the RSSI sweep on the chip ------------------------------------------- *
 * lr_scan_pass hands its passes here while the program is loaded: the same
 * plan, filter, front-end calibration every CALIB_FE_STEP_HZ and pass budget,
 * but the chip retunes and reads RSSI itself, one command a pass instead of
 * four or more a point. The looks of a row are one evenly spaced run of
 * n x looks points (ls_lora_scan_look_hz is linear in looks*i + k), each bin
 * keeping its strongest look. lr.scan_bin counts points here, not bins. */
static void cop_scan_arm_locked(void)
{
    s_cop_scan = s_cop_scan_rx = false;
    if (s_cop_off || !s_info.lr2021 || s_info.fw_major != 1 || s_info.fw_minor != 24) return;
    if (cop_upload_locked() == ESP_OK) s_cop_scan = true;
    else s_cop_off = true;   /* the host-driven sweep from here on this boot */
}

static void cop_scan_stop_locked(void)
{
    if (!s_cop_scan) return;
    s_cop_scan = s_cop_scan_rx = false;
    (void)lr20xx_set_standby(false);
    s_recovered = false;
    (void)recover_locked("chip sweep finished");
}

static uint32_t cop_point_hz(uint32_t j, int n, int looks)
{
    const int64_t span = (int64_t)(lr.scan_max_hz - lr.scan_min_hz);
    const int64_t num = 2LL * j - looks + 1, den = 2LL * looks * (n - 1);
    int64_t off = span * num / den;
    if (off < 0) off = 0;
    if (off > span) off = span;
    return lr.scan_min_hz + (uint32_t)off;
}

static int cop_scan_pass_locked(float *dbm, int n, bool *row_done)
{
    if (n < 2) return 0;
    const int looks = lr.scan_plan.looks > 0 ? lr.scan_plan.looks : 1;
    const uint32_t total = (uint32_t)n * (uint32_t)looks;
    const uint32_t step = (uint32_t)(((uint64_t)(lr.scan_max_hz - lr.scan_min_hz) * 2u) /
                                     (2ull * (uint64_t)looks * (uint64_t)(n - 1)));
    /* dwell 1000 is about 1.4 ms of chip loop: the plan's settle time in those units. */
    uint32_t dwell = lr.scan_plan.settle_us * 1000u / 1400u;
    if (dwell < 20) dwell = 20;
    if (dwell > 65535) dwell = 65535;
    uint32_t per_pass = SCAN_PASS_BUDGET_US / (lr.scan_plan.settle_us + 150u);
    if (per_pass < 1) per_pass = 1;
    if (per_pass > COP_SWEEP_MAX) per_pass = COP_SWEEP_MAX;
    uint32_t j = (uint32_t)lr.scan_bin;
    if (j >= total) j = 0;
    const uint32_t f = cop_point_hz(j, n, looks);
    const int64_t t0 = esp_timer_get_time();
    esp_err_t err = ESP_OK;
    const uint32_t moved = f > lr.cal_hz ? f - lr.cal_hz : lr.cal_hz - f;
    if (!s_cop_scan_rx || moved >= CALIB_FE_STEP_HZ || lr20xx_path_for_hz(f) != lr20xx_path_for_hz(lr.cal_hz)) {
        err = lr20xx_set_standby(true);
        if (err == ESP_OK) err = lr20xx_calib_fe_hz(f, lr20xx_path_for_hz(f));
        if (err == ESP_OK) err = lr20xx_check_last_ok();
        if (err == ESP_OK) { lr.cal_hz = f; err = lr20xx_set_rf_frequency(f); }
        if (err == ESP_OK) err = lr20xx_set_rx(0xFFFFFFu);
        if (err == ESP_OK) {
            /* The first point after entering receive reads garbage: settle on one. */
            const uint8_t p1[15] = {1, (uint8_t)(f >> 24), (uint8_t)(f >> 16), (uint8_t)(f >> 8), (uint8_t)f,
                0, 0, 0, 0, 0, 1, (uint8_t)(dwell >> 8), (uint8_t)dwell, 0, 1};
            uint8_t r1[2];
            err = cop_call_locked(p1, sizeof(p1), r1, 2, 50);
        }
        s_cop_scan_rx = err == ESP_OK;
    }
    /* This pass: within the budget, the calibrated stretch and one input. */
    uint32_t c = total - j;
    if (c > per_pass) c = per_pass;
    while (c > 1) {
        const uint32_t last = cop_point_hz(j + c - 1, n, looks);
        if (last - lr.cal_hz < CALIB_FE_STEP_HZ && lr20xx_path_for_hz(last) == lr20xx_path_for_hz(f)) break;
        c--;
    }
    if (err == ESP_OK) {
        const uint8_t p[15] = {1, (uint8_t)(f >> 24), (uint8_t)(f >> 16), (uint8_t)(f >> 8), (uint8_t)f,
            (uint8_t)(step >> 24), (uint8_t)(step >> 16), (uint8_t)(step >> 8), (uint8_t)step,
            (uint8_t)(c >> 8), (uint8_t)c, (uint8_t)(dwell >> 8), (uint8_t)dwell, 0, 1};
        uint8_t r[2] = {0};
        err = cop_call_locked(p, sizeof(p), r, 2, (int)(c * (dwell / 300u + 4u) + 50u));
        if (err == ESP_OK && (((uint32_t)r[0] << 8) | r[1]) != c) err = ESP_ERR_INVALID_RESPONSE;
    }
    for (uint32_t m = 0; err == ESP_OK && m < c; m += 64) {
        LORA_TABLE static uint8_t b[128];
        const uint32_t k = c - m > 64 ? 64 : c - m;
        err = cop_mem_read_locked(COP_SWEEP_OUT + 2u * m, b, (k + 1) / 2);
        for (uint32_t q = 0; err == ESP_OK && q < k; q++) {
            const uint32_t jj = j + m + q, i = jj / (uint32_t)looks;
            const float v = -(float)(b[2 * q] | (uint32_t)b[2 * q + 1] << 8) / 2.0f;
            if (jj % (uint32_t)looks == 0 || v > dbm[i]) dbm[i] = v;
        }
    }
    if (err != ESP_OK) {
        /* The host-driven sweep takes over: the reset, then a fresh plan. */
        ESP_LOGW(TAG, "chip sweep pass: %s, host sweep from here", esp_err_to_name(err));
        s_cop.last_error = err;
        s_cop_off = true;
        s_cop_scan = s_cop_scan_rx = false;
        s_recovered = false;
        (void)recover_locked("chip sweep failed");
        lr.scan_plan_n = 0;
        return 0;
    }
    s_cop.sweeps++;
    s_cop.points += c;
    lr.prof.settle_us = (uint32_t)((esp_timer_get_time() - t0) / (int64_t)c);
    lr.prof.standby_us = lr.prof.tune_us = lr.prof.rx_us = lr.prof.rssi_us = 0;
    j += c;
    if (j >= total) { j = 0; if (row_done) *row_done = true; }
    lr.scan_bin = (int)j;
    return n;
}

/* ---- many LoRa channels on one radio (scan.bin) ---------------------------- *
 * The chip hops its channel table running CAD on each (preamble-only CAD by
 * default, no false alarms in lr2021-re's runs), stays on the first channel
 * that shows a real preamble and parks on the packet. Here the host polls the
 * IRQ word for RxDone, reads the packet and resumes. Entry 4: op 1 start
 * (n, CAD symbols, preamble wait symbols, CAD flags), 3 resume, 2 status,
 * 0 stop; each answers ten LE words. Table at 0x807800, 8 bytes a channel. */
#define COP_SCAN_TAB 0x807800u
static const uint8_t COP_CAD_PEAK[2][8] = {
    {60, 60, 60, 64, 64, 66, 70, 74},   /* one-symbol CAD, SF5..SF12 */
    {56, 56, 56, 58, 58, 60, 64, 68},   /* two symbols */
};

static esp_err_t cop_scan_op_locked(uint8_t op, const uint8_t *extra, size_t n, uint32_t w[10])
{
    uint8_t p[8] = {4, op};
    if (n > sizeof(p) - 2) return ESP_ERR_INVALID_ARG;
    if (n) memcpy(p + 2, extra, n);
    uint8_t r[40] = {0};
    esp_err_t err = cop_call_locked(p, 2 + n, r, sizeof(r), 50);
    for (unsigned i = 0; err == ESP_OK && w && i < 10; i++)
        w[i] = r[4 * i] | (uint32_t)r[4 * i + 1] << 8 | (uint32_t)r[4 * i + 2] << 16 | (uint32_t)r[4 * i + 3] << 24;
    return err;
}

esp_err_t lr20xx_chip_lora_scan(const lr20xx_chip_scan_cfg_t *cfg, lr20xx_chip_scan_sink_t sink, void *ctx,
                                lr20xx_chip_scan_stats_t *stats, lr20xx_chip_result_t *out)
{
    lr20xx_chip_result_t res = {0};
    if (stats) memset(stats, 0, sizeof(*stats));
    bool bad = !cfg || !sink || !cfg->chans || cfg->n < 1 || cfg->n > 32 || cfg->cad_symbols < 1 || cfg->cad_symbols > 2;
    for (unsigned i = 0; !bad && i < cfg->n; i++) {
        const lr20xx_chip_chan_t *c = &cfg->chans[i];
        bad = c->sf < 5 || c->sf > 12 || !ls_lora_packet_rx_hz_ok(lr_caps(), c->hz) ||
              lr20xx_path_for_hz(c->hz) != lr20xx_path_for_hz(cfg->chans[0].hz);
    }
    if (bad) {
        if (out) { *out = res; out->error = ESP_ERR_INVALID_ARG; }
        return ESP_ERR_INVALID_ARG;
    }
    bool expected = false;
    if (!atomic_compare_exchange_strong(&s_iq_live_active, &expected, true)) {
        if (out) { *out = res; out->error = ESP_ERR_INVALID_STATE; }
        return ESP_ERR_INVALID_STATE;
    }
    ls_lora_hw_lock();
    esp_err_t err = cop_state_ok_locked();
    const bool rx = lr.rx_mode || lr.rearm;
    const bool touched = err == ESP_OK;
    if (touched) s_cop.sessions++;
    const lr20xx_chip_chan_t *c0 = touched ? &cfg->chans[0] : NULL;
    if (err == ESP_OK) {
        lr.programmed = lr.rx_mode = lr.rearm = false;
        err = setup_radio(LR20XX_PKT_TYPE_LORA, c0->hz);
    }
    if (err == ESP_OK) err = cop_upload_image_locked(&COP_SCAN);
    if (err == ESP_OK) {
        const lr20xx_lora_bw_t bw = lr20xx_lora_bw_nearest(c0->bw_hz);
        err = step_lr(lr20xx_lora_set_modulation(c0->sf, bw.code, 1, lr20xx_lora_ldro(c0->sf, bw.hz)), "scan modulation");
    }
    if (err == ESP_OK) err = step_lr(lr20xx_lora_set_packet(c0->sf <= 8 ? 32 : 16, 255, false, true, false), "scan packet");
    if (err == ESP_OK) err = step_lr(lr20xx_lora_set_syncword(cfg->sync_word), "scan syncword");
    if (err == ESP_OK) {
        /* Fallback FS keeps the synthesizer running between hops; a detected
           preamble stops the short RX timeout of a CAD hit. */
        const uint8_t fs = 3, stop_on_preamble = 1;
        err = step_ok(lr20xx_write(0x0206, &fs, 1), "scan fallback");
        if (err == ESP_OK) err = step_ok(lr20xx_write(0x0209, &stop_on_preamble, 1), "scan preamble stop");
    }
    for (unsigned i = 0; err == ESP_OK && i < cfg->n; i++) {
        const lr20xx_chip_chan_t *c = &cfg->chans[i];
        const lr20xx_lora_bw_t bw = lr20xx_lora_bw_nearest(c->bw_hz);
        const uint32_t mod0 = (uint32_t)(c->sf << 4) | bw.code, mod1 = 0x10u | (lr20xx_lora_ldro(c->sf, bw.hz) ? 1u : 0u);
        /* Clamped: the -O2 Nano build cannot prove the caller's SF 5-12 /
           1-2 CAD symbol range and stops on array-bounds. */
        const unsigned cad_i = cfg->cad_symbols >= 2 ? 1u : 0u;
        const unsigned sf_i = c->sf >= 5 && c->sf <= 12 ? (unsigned)(c->sf - 5) : 0u;
        const uint32_t peak = COP_CAD_PEAK[cad_i][sf_i], pre = c->sf <= 8 ? 32u : 16u;
        err = lr20xx_write_reg32(COP_SCAN_TAB + 8u * i, c->hz);
        if (err == ESP_OK) err = lr20xx_write_reg32(COP_SCAN_TAB + 8u * i + 4u, mod0 | mod1 << 8 | peak << 16 | pre << 24);
    }
    if (err == ESP_OK) err = step_ok(lr20xx_clear_irq(LR20XX_IRQ_ALL), "scan irq");
    uint32_t w[10] = {0};
    if (err == ESP_OK) {
        const uint8_t start[4] = {cfg->n, cfg->cad_symbols, 8, cfg->cad_flags};
        err = cop_scan_op_locked(1, start, sizeof(start), w);
    }
    LORA_TABLE static uint8_t data[256];
    while (err == ESP_OK) {
        if (!sink(ctx, NULL)) break;
        bool pending = false;
        uint32_t irq = 0;
        err = lr20xx_irq_pending(&pending, &irq);
        if (err != ESP_OK) break;
        if (!(irq & LR20XX_IRQ_RX_DONE)) {
            /* Between packets the chip runs the scan itself with BUSY low. */
            ls_lora_hw_unlock();
            vTaskDelay(pdMS_TO_TICKS(cfg->poll_ms ? cfg->poll_ms : 5));
            ls_lora_hw_lock();
            continue;
        }
        lr20xx_chip_packet_t p = {.crc_ok = !(irq & (LR20XX_IRQ_CRC_ERROR | LR20XX_IRQ_LORA_HDR_ERROR)), .data = data};
        uint16_t len = 0;
        if (lr20xx_get_rx_pkt_length(&len) == ESP_OK && len && len <= 255 &&
            lr20xx_read_rx_fifo(data, len) == ESP_OK) p.len = (uint8_t)len;
        lr20xx_lora_pkt_status_t ps;
        if (lr20xx_get_lora_packet_status(&ps) == ESP_OK) { p.rssi_dbm = ps.rssi_dbm; p.snr_db = ps.snr_db; }
        (void)lr20xx_clear_rx_fifo();
        /* The resume clears RxDone and answers which channel it came in on. */
        err = cop_scan_op_locked(3, NULL, 0, w);
        if (err != ESP_OK) break;
        p.channel = (uint8_t)(w[0] >> 8);
        res.frames++;
        if (p.len && !sink(ctx, &p)) break;
    }
    if (touched && cop_scan_op_locked(2, NULL, 0, w) == ESP_OK && stats) {
        stats->cads = w[1]; stats->falses = w[2]; stats->rx_lost = w[3]; stats->packets = w[4]; stats->passes = w[5];
    }
    if (touched) (void)cop_scan_op_locked(0, NULL, 0, NULL);
    res.error = err;
    s_cop.last_error = err;
    if (touched) res.recovery_error = cop_finish_locked(rx, "chip scan finished");
    ls_lora_hw_unlock();
    atomic_store(&s_iq_live_active, false);
    if (out) *out = res;
    return err != ESP_OK ? err : res.recovery_error;
}

/* lora lrscan <MHz[,MHz...] | start:stop:step MHz> [sf 7] [bw kHz 62.5] [seconds 30] [sync 0x12]:
   every packet heard on any of the channels, with the channel it came in on. */
extern bool ls_mesh_radio_hold(bool on) __attribute__((weak));
typedef struct { int64_t end; uint32_t packets; const lr20xx_chip_chan_t *chans; } cop_scan_probe_t;
static bool cop_scan_probe_sink(void *ctx, const lr20xx_chip_packet_t *p)
{
    cop_scan_probe_t *s = ctx;
    if (!p) return esp_timer_get_time() < s->end;
    s->packets++;
    printf("lrscan: %.3f s ch=%u %.4f MHz len=%u crc=%s rssi=%.1f snr=%.1f data=",
           (double)(esp_timer_get_time() % 100000000) / 1e6, p->channel,
           p->channel < 32 ? s->chans[p->channel].hz / 1e6 : 0.0, p->len, p->crc_ok ? "ok" : "bad",
           (double)p->rssi_dbm, (double)p->snr_db);
    for (unsigned i = 0; i < p->len && i < 48; i++) printf("%02X", p->data[i]);
    printf(p->len > 48 ? "...\n" : "\n");
    return true;
}
LR_CMD lrscan_command(int argc, char **argv)
{
    LORA_TABLE static lr20xx_chip_chan_t chans[32];
    unsigned n = 0;
    float sf = 7, bw = 62.5f, secs = 30;
    unsigned sync = 0x12;
    if (argc >= 3) {
        float a, b, st;
        if (sscanf(argv[2], "%f:%f:%f", &a, &b, &st) == 3 && st > 0 && b >= a) {
            for (float f = a; f <= b + st / 2 && n < 32; f += st) chans[n++].hz = (uint32_t)(f * 1e6f + 0.5f);
        } else {
            for (char *s = argv[2]; *s && n < 32;) {
                chans[n++].hz = (uint32_t)(strtof(s, &s) * 1e6f + 0.5f);
                if (*s == ',') s++; else break;
            }
        }
    }
    if (argc > 3) sf = strtof(argv[3], NULL);
    if (argc > 4) bw = strtof(argv[4], NULL);
    if (argc > 5) secs = strtof(argv[5], NULL);
    if (argc > 6) sync = (unsigned)strtoul(argv[6], NULL, 0);
    if (argc < 3 || argc > 7 || !n || sf < 5 || sf > 12 || bw < 7 || bw > 1000 || secs <= 0 || secs > 3600 || sync > 255) {
        puts("usage: lora lrscan <MHz[,MHz..] | start:stop:step> [sf 7] [bw kHz 62.5] [seconds 30] [sync 0x12]");
        return 1;
    }
    for (unsigned i = 0; i < n; i++) { chans[i].sf = (uint8_t)sf; chans[i].bw_hz = (uint32_t)(bw * 1000); }
    /* MeshCore gives the radio up for the scan, and gets it back after. */
    bool held = false;
    if (ls_mesh_radio_hold) {
        for (int i = 0; i < 300 && !(held = ls_mesh_radio_hold(true)); i++) vTaskDelay(pdMS_TO_TICKS(10));
        if (!held) { ls_mesh_radio_hold(false); puts("lrscan: MeshCore did not release the radio"); return 1; }
    }
    const lr20xx_chip_scan_cfg_t cfg = {.chans = chans, .n = (uint8_t)n, .sync_word = (uint8_t)sync,
                                        .cad_symbols = 1, .cad_flags = 0x10, .poll_ms = 5};
    cop_scan_probe_t probe = {.end = esp_timer_get_time() + (int64_t)(secs * 1e6f), .chans = chans};
    printf("lrscan: %u channels %.4f..%.4f MHz SF%u BW%.1f for %.0f s\n", n, chans[0].hz / 1e6,
           chans[n - 1].hz / 1e6, (unsigned)sf, (double)bw, (double)secs);
    lr20xx_chip_scan_stats_t st;
    lr20xx_chip_result_t r;
    const esp_err_t err = lr20xx_chip_lora_scan(&cfg, cop_scan_probe_sink, &probe, &st, &r);
    if (held) ls_mesh_radio_hold(false);
    printf("lrscan done: packets=%lu cads=%lu false=%lu lost=%lu passes=%lu result=%s recovery=%s\n",
           (unsigned long)probe.packets, (unsigned long)st.cads, (unsigned long)st.falses, (unsigned long)st.rx_lost,
           (unsigned long)st.passes, esp_err_to_name(r.error), esp_err_to_name(r.recovery_error));
    return err != ESP_OK;
}

/* ---- down-conversion on the chip (experiment, ddc.bin) -------------------- *
 * lora lrchipddc [MHz] [seconds] [dec] [shift] [T]: the 576k raw route streams into
 * the chip's own RAM, the chip mixes by fs/4 and decimates (CIC), and the host
 * reads its output ring in PRAM. Reports whether the chip kept up (isr_max
 * against T, overruns) and what reached the host. The FIFO belongs to the
 * program while it runs, so nothing here reads it. */
#define COP_DDC_RING      0x804000u   /* the idle capture bank: 4 KB, 16 ms at 125k pairs/s */
#define COP_DDC_BLK       256u
#define COP_DDC_BLK_DATA  252u
#define COP_DDC_BLK_PAIRS (COP_DDC_BLK_DATA / 2)
#define COP_DDC_NBLK      16u
/* The live form for the I/Q session: runs until the sink says stop. The
   sink gets decimated int8 pairs in order, first_pair counting output pairs;
   a lapped ring shows as a jump in first_pair, which the session treats as
   any other gap. The caller holds the mesh off the radio, as for the stream. */
esp_err_t lr20xx_chip_ddc_live(const lr20xx_chip_ddc_cfg_t *cfg, lr20xx_chip_ddc_sink_t sink, void *ctx,
                               lr20xx_chip_ddc_result_t *out)
{
    lr20xx_chip_ddc_result_t res = {0};
    if (!cfg || !sink || !cfg->dec || cfg->dec > 64 || cfg->shift > 16 || cfg->gain > 13 ||
        !ls_lora_packet_rx_hz_ok(lr_caps(), cfg->lo_hz)) {
        if (out) { *out = res; out->error = ESP_ERR_INVALID_ARG; }
        return ESP_ERR_INVALID_ARG;
    }
    bool expected = false;
    if (!atomic_compare_exchange_strong(&s_iq_live_active, &expected, true)) {
        if (out) { *out = res; out->error = ESP_ERR_INVALID_STATE; }
        return ESP_ERR_INVALID_STATE;
    }
    const lr20xx_iq_config_t c = {.route = cfg->code ? LR20XX_IQ_SOURCE1 : LR20XX_IQ_SOURCE0_576,
                                  .code = cfg->code, .frequency_hz = cfg->lo_hz, .gain = cfg->gain,
                                  .path = lr20xx_path_for_hz(cfg->lo_hz)};
    uint8_t route[11];
    lr20xx_iq_format_t fmt;
    const unsigned t = 448;
    ls_lora_hw_lock();
    esp_err_t err = cop_state_ok_locked();
    const bool rx = lr.rx_mode || lr.rearm;
    const bool touched = err == ESP_OK;
    if (err == ESP_OK && !lr20xx_iq_route_build(c.route, c.code, 0, route, &res.in_rate_hz, &fmt)) err = ESP_ERR_INVALID_ARG;
    if (err == ESP_OK) err = cop_upload_image_locked(&COP_DDC);
    if (err == ESP_OK) err = ls_lora_hw_capture_fast(true);
    if (err == ESP_OK) err = iq_program_route_locked(&c);
    if (err == ESP_OK) err = step_ok(lr20xx_write(0x0290, route, 11), "ddc route");
    /* The radio answers ring reads after its own interrupt work: sleep on BUSY, not spin. */
    const bool slept = err == ESP_OK && !cfg->spin && ls_lora_hw_busy_sleep(true) == ESP_OK;
    const uint8_t clear_flags[] = {0x3F, 0x3F};
    if (err == ESP_OK) err = step_ok(lr20xx_write(0x0114, clear_flags, 2), "ddc FIFO flags");
    if (err == ESP_OK) err = step_ok(lr20xx_set_rx(0xFFFFFFu), "ddc SetRx");
    /* SetRx can reset the fixed gain on some boards (lr2021-sdr): again after it. */
    if (err == ESP_OK) err = step_ok(lr20xx_set_agc_gain(cfg->gain), "ddc gain");
    if (err == ESP_OK) {
        const uint8_t start[7] = {6, 1, (uint8_t)(t >> 8), (uint8_t)t, cfg->dec, cfg->shift,
                                  (uint8_t)((cfg->plus ? 1 : 0) | (cfg->order3 ? 2 : 0))};
        uint8_t r[48];
        err = cop_call_locked(start, sizeof(start), r, sizeof(r), 20);
    }
    /* ddc.bin's ring: 16 blocks of 256 bytes, 252 of output pairs and the
       block's sequence number (1, 2, ...) written last. A block whose number
       is the one expected is complete; one already newer means the ring
       lapped. Reads are paced on the block period, so few find nothing. */
    LORA_TABLE static uint8_t buf[COP_DDC_BLK];
    uint32_t blk = 1;
    uint64_t first = 0;
    const int64_t period_us = res.in_rate_hz ? (int64_t)COP_DDC_BLK_PAIRS * cfg->dec * 1000000 / res.in_rate_hz : 1000;
    int64_t due = esp_timer_get_time();
    bool more = true;
    unsigned tmo_run = 0;
    while (err == ESP_OK && more) {
        int64_t now = esp_timer_get_time();
        if (now < due) { vTaskDelay(1); continue; }
        const int64_t r0 = now;
        err = cop_mem_read_locked(COP_DDC_RING + ((blk - 1) & (COP_DDC_NBLK - 1)) * COP_DDC_BLK, buf,
                                  COP_DDC_BLK / 4);
        now = esp_timer_get_time();
        res.read_us += (uint32_t)(now - r0);
        res.reads++;
        /* A host stall (a flash write holds both cores) can outlast a BUSY
           wait: the answer is lost, the next command is clean. Try again;
           four in a row is a radio that stopped. */
        if (err == ESP_ERR_TIMEOUT && ++tmo_run < 4) { res.timeouts++; err = ESP_OK; continue; }
        if (err != ESP_OK) break;
        tmo_run = 0;
        const uint32_t seq = buf[COP_DDC_BLK_DATA] | (uint32_t)buf[COP_DDC_BLK_DATA + 1] << 8 |
                             (uint32_t)buf[COP_DDC_BLK_DATA + 2] << 16 | (uint32_t)buf[COP_DDC_BLK_DATA + 3] << 24;
        if (seq == blk) {
            more = sink(ctx, buf, COP_DDC_BLK_DATA, first);
            first += COP_DDC_BLK_PAIRS; res.out_pairs += COP_DDC_BLK_PAIRS;
            blk++;
            due += period_us;
            if (due < now - 8 * period_us) due = now;   /* far behind: read straight on */
        } else if (seq > blk && seq - blk < 0x10000000u) {
            /* Lapped: that slot already holds a newer block. Skip to just past it. */
            res.laps++;
            res.lapped_bytes += (seq + 1 - blk) * COP_DDC_BLK_DATA;
            first += (uint64_t)(seq + 1 - blk) * COP_DDC_BLK_PAIRS;
            blk = seq + 1;
            due = now;
        } else {
            res.early++;                       /* not written yet */
            due = now + period_us / 4;
        }
    }
    if (touched) {
        const uint8_t st[2] = {6, 2}, stop[2] = {6, 0};
        uint8_t r[48] = {0};
        if (cop_call_locked(st, 2, r, sizeof(r), 20) == ESP_OK) {
            res.chip_overruns = r[8] | (uint32_t)r[9] << 8 | (uint32_t)r[10] << 16 | (uint32_t)r[11] << 24;
            res.chip_busy_max = r[12] | (uint32_t)r[13] << 8 | (uint32_t)r[14] << 16 | (uint32_t)r[15] << 24;
        }
        (void)cop_call_locked(stop, 2, r, sizeof(r), 20);
        if (slept) (void)ls_lora_hw_busy_sleep(false);
        (void)ls_lora_hw_capture_fast(false);
    }
    res.error = err;
    if (touched) res.recovery_error = cop_finish_locked(rx, "chip ddc finished");
    ls_lora_hw_unlock();
    atomic_store(&s_iq_live_active, false);
    if (out) *out = res;
    return err != ESP_OK ? err : res.recovery_error;
}

/* lora lrchipddc [MHz 915] [seconds 5] [dec 8] [shift 6] [code 0x19] [order 2|3]:
   lr20xx_chip_ddc_live on its own, with MeshCore held off; reports what reached
   the host, the host's time in reads and a spectrum of 2048 output pairs. */
typedef struct { int64_t t0, end; uint32_t snap_n, got; uint8_t snap[4096]; } cop_ddc_probe_t;
/* Both cores' idle run time (FreeRTOS run-time stats, microseconds). */
static void cop_idle_us(uint32_t idle[2])
{
    for (BaseType_t core = 0; core < 2; core++) {
        TaskStatus_t info;
        vTaskGetInfo(xTaskGetIdleTaskHandleForCore(core), &info, pdFALSE, eRunning);
        idle[core] = info.ulRunTimeCounter;
    }
}
static EXT_RAM_BSS_ATTR cop_ddc_probe_t s_ddc_probe;
static bool cop_ddc_probe_sink(void *ctx, const uint8_t *iq8, size_t bytes, uint64_t first)
{
    (void)first;
    cop_ddc_probe_t *p = ctx;
    const int64_t now = esp_timer_get_time();
    p->got += bytes;
    /* 2048 pairs from one second in, for the spectrum below. */
    if (now - p->t0 > 1000000 && p->snap_n < sizeof(p->snap)) {
        const uint32_t k = sizeof(p->snap) - p->snap_n < bytes ? sizeof(p->snap) - p->snap_n : bytes;
        memcpy(p->snap + p->snap_n, iq8, k);
        p->snap_n += k;
    }
    return now < p->end;
}
LR_CMD lrddc_command(int argc, char **argv)
{
    const float mhz = argc > 2 ? strtof(argv[2], NULL) : 915.0f, secs = argc > 3 ? strtof(argv[3], NULL) : 5.0f;
    const unsigned dec = argc > 4 ? (unsigned)strtoul(argv[4], NULL, 0) : 8;
    const unsigned shift = argc > 5 ? (unsigned)strtoul(argv[5], NULL, 0) : 6;
    const unsigned code = argc > 6 ? (unsigned)strtoul(argv[6], NULL, 0) : 0x19;
    const unsigned order = argc > 7 ? (unsigned)strtoul(argv[7], NULL, 0) : 2;
    const bool spin = argc > 8 && strtoul(argv[8], NULL, 0) == 0;   /* sleep 0: poll BUSY */
    if (argc > 9 || code > 255 || mhz < 150 || mhz > 960 || secs <= 0 || secs > 60 || !dec || dec > 64 ||
        shift > 16 || (order != 2 && order != 3)) {
        puts("usage: lora lrchipddc [MHz 915] [seconds 5] [dec 8] [shift 6] [code 0x19 (0 = 576k)] [order 2|3] [sleep 1|0]");
        return 1;
    }
    bool held = false;
    if (ls_mesh_radio_hold) {
        for (int i = 0; i < 300 && !(held = ls_mesh_radio_hold(true)); i++) vTaskDelay(pdMS_TO_TICKS(10));
        if (!held) { ls_mesh_radio_hold(false); puts("lrddc: MeshCore did not release the radio"); return 1; }
    }
    /* One second of the board as it is, then the run, both cores' busy share each. */
    uint32_t i0[2], i1[2], i2[2];
    cop_idle_us(i0);
    const int64_t b0 = esp_timer_get_time();
    vTaskDelay(pdMS_TO_TICKS(1000));
    cop_idle_us(i1);
    const int64_t b1 = esp_timer_get_time();
    cop_ddc_probe_t *p = &s_ddc_probe;
    p->t0 = esp_timer_get_time();
    p->end = p->t0 + (int64_t)(secs * 1e6f);
    p->snap_n = p->got = 0;
    const lr20xx_chip_ddc_cfg_t c = {.lo_hz = (uint32_t)(mhz * 1e6f + 0.5f), .code = (uint8_t)code,
                                     .dec = (uint8_t)dec, .shift = (uint8_t)shift, .order3 = order == 3, .spin = spin};
    lr20xx_chip_ddc_result_t r;
    const esp_err_t err = lr20xx_chip_ddc_live(&c, cop_ddc_probe_sink, p, &r);
    const uint32_t us = (uint32_t)(esp_timer_get_time() - p->t0);
    cop_idle_us(i2);
    if (held) ls_mesh_radio_hold(false);
    const unsigned rate = r.in_rate_hz;
    const double base_us = (double)(b1 - b0);
    printf("lrddc: cpu busy before core0 %.0f%% core1 %.0f%% | during core0 %.0f%% core1 %.0f%%\n",
           100.0 - 100.0 * (uint32_t)(i1[0] - i0[0]) / base_us, 100.0 - 100.0 * (uint32_t)(i1[1] - i0[1]) / base_us,
           100.0 - 100.0 * (uint32_t)(i2[0] - i1[0]) / us, 100.0 - 100.0 * (uint32_t)(i2[1] - i1[1]) / us);
    printf("lrddc: %.4f MHz in %u S/s, CIC%u / %u -> %u S/s, %.2f s\n", (double)mhz, rate, order, dec, rate / dec, us / 1e6);
    printf("lrddc: chip overruns=%lu busy_max=%lu (of 448); host got %lu pairs (%.0f/s), laps %lu, early reads %lu, timeouts %lu\n",
           (unsigned long)r.chip_overruns, (unsigned long)r.chip_busy_max, (unsigned long)r.out_pairs,
           us ? r.out_pairs * 1e6 / us : 0.0, (unsigned long)r.laps, (unsigned long)r.early, (unsigned long)r.timeouts);
    printf("lrddc: host time in reads %lu us of %lu (%.0f%%), %lu reads, %.0f us a read; result=%s recovery=%s\n",
           (unsigned long)r.read_us, (unsigned long)us, us ? 100.0 * r.read_us / us : 0.0, (unsigned long)r.reads,
           r.reads ? (double)r.read_us / r.reads : 0.0, esp_err_to_name(r.error), esp_err_to_name(r.recovery_error));
    if (p->snap_n == sizeof(p->snap) && rate) {
        /* Hann-windowed DFT, 256 bins across the output rate, averaged over 8 windows of 256. */
        static EXT_RAM_BSS_ATTR float pw[256], cs[256], sn[256], win[256];
        const double fo = (double)rate / dec;
        for (unsigned k = 0; k < 256; k++) {
            cs[k] = cosf(2.0f * (float)M_PI * k / 256.0f); sn[k] = sinf(2.0f * (float)M_PI * k / 256.0f);
            win[k] = 0.5f - 0.5f * cs[k]; pw[k] = 0;
        }
        for (unsigned wdw = 0; wdw < 8; wdw++) {
            const int8_t *x = (const int8_t *)p->snap + wdw * 512;
            for (unsigned b = 0; b < 256; b++) {
                float re = 0, im = 0;
                for (unsigned n = 0, ph = 0; n < 256; n++, ph = (ph + b) & 255) {
                    const float i = x[2 * n] * win[n], q = x[2 * n + 1] * win[n];
                    re += i * cs[ph] + q * sn[ph]; im += q * cs[ph] - i * sn[ph];
                }
                pw[b] += re * re + im * im;
            }
        }
        unsigned top = 0;
        for (unsigned b = 1; b < 256; b++) if (pw[b] > pw[top]) top = b;
        float lo = 1e30f, hi = 0;
        for (unsigned b = 0; b < 256; b++) { if (pw[b] < lo) lo = pw[b]; if (pw[b] > hi) hi = pw[b]; }
        for (int it = 0; it < 40; it++) {   /* the median bin, by bisection */
            const float m = 0.5f * (lo + hi);
            unsigned below = 0;
            for (unsigned b = 0; b < 256; b++) below += pw[b] < m;
            if (below < 128) lo = m; else hi = m;
        }
        const float med = 0.5f * (lo + hi);
        const int tb = top < 128 ? (int)top : (int)top - 256;
        printf("lrddc spectrum: peak %+.0f Hz, %.1f dB over the median bin (%.0f Hz bins)\n",
               tb * fo / 256.0, 10.0 * log10((double)(pw[top] / (med > 0 ? med : 1))), fo / 256.0);
        printf("lrddc bins:");
        for (unsigned k = 0; k < 256; k++) printf(" %d", (int)lroundf(10.0f * log10f(pw[(k + 128) & 255] + 1.0f)));
        printf("\n");
    }
    return err != ESP_OK;
}

void lr20xx_chip_stats(lr20xx_chip_stats_t *out)
{
    if (!out) return;
    out->sessions = s_cop.sessions; out->frames = s_cop.frames;
    out->sweeps = s_cop.sweeps; out->points = s_cop.points;
    out->uploads = s_cop.uploads; out->upload_fail = s_cop.upload_fail;
    out->last_chip_us = s_cop.last_chip_us; out->max_chip_us = s_cop.max_chip_us;
    out->last_read_us = s_cop.last_read_us; out->last_error = s_cop.last_error;
}

/* lora lrchip [frames [hz [nseg]]] | lora lrchip sweep <start> <stop> <step> [dwell] */
typedef struct { uint32_t want, got; int64_t t0, t_end; uint32_t peak_bin; uint8_t peak; } cop_probe_t;
static bool cop_probe_sink(void *ctx, const uint8_t *db, uint32_t chip_us)
{
    (void)chip_us;
    cop_probe_t *p = ctx;
    if (!p->got) p->t0 = esp_timer_get_time();
    p->got++;
    for (unsigned i = 0; i < 256; i++)
        if (db[i] > p->peak && i) { p->peak = db[i]; p->peak_bin = i; }
    p->t_end = esp_timer_get_time();
    return p->got < p->want;
}
LR_CMD cop_command(int argc, char **argv)
{
    if (argc >= 3 && !strcmp(argv[2], "cpu")) {
        /* Both cores' busy share over a window, from the idle tasks' run time. */
        uint32_t a[2], b[2], ms = argc > 3 ? (uint32_t)strtoul(argv[3], NULL, 0) : 3000;
        if (ms < 100 || ms > 60000) ms = 3000;
        cop_idle_us(a);
        const int64_t t0 = esp_timer_get_time();
        vTaskDelay(pdMS_TO_TICKS(ms));
        cop_idle_us(b);
        const double us = (double)(esp_timer_get_time() - t0);
        printf("lrchip cpu: core0 %.0f%% core1 %.0f%% busy over %lu ms\n", 100.0 - 100.0 * (uint32_t)(b[0] - a[0]) / us,
               100.0 - 100.0 * (uint32_t)(b[1] - a[1]) / us, (unsigned long)ms);
        return 0;
    }
    if (argc == 3 && !strcmp(argv[2], "stats")) {
        printf("lrchip stats: sessions=%lu frames=%lu sweep_passes=%lu points=%lu uploads=%lu fail=%lu loaded=%d "
               "scan=%s settle_us=%lu last=%s\n", (unsigned long)s_cop.sessions, (unsigned long)s_cop.frames,
               (unsigned long)s_cop.sweeps, (unsigned long)s_cop.points, (unsigned long)s_cop.uploads,
               (unsigned long)s_cop.upload_fail, s_cop_loaded, s_cop_off ? "host" : s_cop_scan ? "chip (active)" : "chip",
               (unsigned long)lr.prof.settle_us, esp_err_to_name(s_cop.last_error));
        return 0;
    }
    if (argc == 4 && !strcmp(argv[2], "scan") && (!strcmp(argv[3], "on") || !strcmp(argv[3], "off"))) {
        /* Takes effect when the next sweep plans (a new band, or the next start). */
        s_cop_off = !strcmp(argv[3], "off");
        printf("lrchip scan: %s from the next sweep\n", s_cop_off ? "host-driven" : "on the chip");
        return 0;
    }
    if (argc > 2 && !strcmp(argv[2], "sweep")) {
        uint32_t a = 0, b = 0, s = 0, dwell = 1000;
        if (argc < 6 || argc > 7 || !probe_number(argv[3], 0xFFFFFFFFu, &a) || !probe_number(argv[4], 0xFFFFFFFFu, &b) ||
            !probe_number(argv[5], 0xFFFFFFFFu, &s) || !s || b < a || (b - a) / s >= 4000 ||
            (argc == 7 && !probe_number(argv[6], 65535, &dwell))) {
            puts("usage: lora lrchip sweep <start Hz> <stop Hz> <step Hz> [dwell]");
            return 1;
        }
        const uint32_t n = (b - a) / s + 1;
        float *dbm = heap_caps_malloc(n * sizeof(float), MALLOC_CAP_SPIRAM);
        if (!dbm) return 1;
        const lr20xx_chip_sweep_cfg_t c = {.start_hz = a, .step_hz = s, .points = (uint16_t)n,
            .dwell = (uint16_t)dwell, .avg = 4, .bw_hz = 62500};
        lr20xx_chip_result_t r;
        const int64_t t0 = esp_timer_get_time();
        const esp_err_t err = lr20xx_chip_sweep(&c, dbm, &r);
        const uint32_t ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
        unsigned top = 0;
        for (unsigned i = 1; i < r.frames; i++) if (dbm[i] > dbm[top]) top = i;
        for (unsigned i = 0; i < r.frames; i += r.frames > 64 ? r.frames / 64 : 1)
            printf("lrchip: %lu %.1f\n", (unsigned long)(a + i * s), (double)dbm[i]);
        printf("lrchip sweep: points=%lu ms=%lu strongest=%lu Hz %.1f dBm result=%s recovery=%s\n",
               (unsigned long)r.frames, (unsigned long)ms, (unsigned long)(a + top * s),
               r.frames ? (double)dbm[top] : 0.0, esp_err_to_name(r.error), esp_err_to_name(r.recovery_error));
        heap_caps_free(dbm);
        return err != ESP_OK;
    }
    uint32_t frames = 200, hz = lr.cfg_valid ? lr.cfg.freq_hz : 915000000u, nseg = 1;
    if (argc > 5 || (argc > 2 && !probe_number(argv[2], 100000, &frames)) ||
        (argc > 3 && !probe_number(argv[3], LR20XX_FREQ_MAX_HZ, &hz)) ||
        (argc > 4 && (!probe_number(argv[4], 4, &nseg) || !nseg)) || !frames) {
        puts("usage: lora lrchip [frames [Hz [nseg 1..4]]] | lora lrchip sweep <start> <stop> <step> [dwell]");
        return 1;
    }
    cop_probe_t p = {.want = frames};
    const lr20xx_chip_spec_cfg_t c = {.hz = hz, .bw_hz = 62500, .nseg = (uint8_t)nseg, .gain = 0};
    lr20xx_chip_result_t r;
    const esp_err_t err = lr20xx_chip_spectrum(&c, cop_probe_sink, &p, &r);
    const int64_t us = p.t_end - p.t0;
    const int bin = p.peak_bin < 128 ? (int)p.peak_bin : (int)p.peak_bin - 256;
    printf("lrchip: frames=%lu fps=%.1f chip_us=%lu max_chip_us=%lu read_us=%lu peak=%+.1f kHz %.1f dB uploads=%lu fail=%lu result=%s recovery=%s\n",
           (unsigned long)r.frames, us > 0 && p.got > 1 ? (p.got - 1) * 1e6 / (double)us : 0.0,
           (unsigned long)s_cop.last_chip_us, (unsigned long)s_cop.max_chip_us, (unsigned long)s_cop.last_read_us,
           bin * 250.0 / 256.0, p.peak / 2.0, (unsigned long)s_cop.uploads, (unsigned long)s_cop.upload_fail,
           esp_err_to_name(r.error), esp_err_to_name(r.recovery_error));
    return err != ESP_OK;
}
