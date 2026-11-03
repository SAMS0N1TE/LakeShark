#include "lr_chip_wf.h"
#include "ls_lora.h"
#include "ls_lora_lr20xx.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>
#include <stdatomic.h>
#include <string.h>

/* A full-scale int8 tone reads about 84 dB on the chip (127 x 128 window
   gain); that is 0 dBFS here, the scale lr_iq_spectrum's rows use. */
#define CHIP_DB_FULL_SCALE 84.0f

typedef struct {
    atomic_bool stop, done;
    uint32_t hz;
    esp_err_t error;
    /* Owner only: this row's power sums. */
    float acc[256];
    uint32_t n;
    int64_t row_t0;
} session_t;

typedef struct {
    float bins[256];
    lr_spec_line line;
} box_t;
static EXT_RAM_BSS_ATTR box_t s_box;
static atomic_flag s_mailbox = ATOMIC_FLAG_INIT;
static atomic_uint s_center, s_rows, s_frames, s_per_row;
static atomic_bool s_failed;
static atomic_int s_error;
static session_t *session;
/* Power of each half-dB code: 10^(code / 20). */
static EXT_RAM_BSS_ATTR float s_pow[256];
static bool s_pow_ready;

static void publish(session_t *s, uint32_t now_ms)
{
    if (atomic_flag_test_and_set_explicit(&s_mailbox, memory_order_acquire)) return;
    for (unsigned k = 0; k < 256; k++) {
        /* FFT order to low frequency first. */
        const float p = s->acc[(k + 128) & 255] / (float)s->n;
        const float dbfs = 10.0f * log10f(p > 1e-3f ? p : 1e-3f) - CHIP_DB_FULL_SCALE;
        const float v = (dbfs + 100.0f) / 100.0f;
        s_box.bins[k] = v < 0 ? 0 : v > 1 ? 1 : v;
    }
    s_box.line.center_hz = s->hz;
    s_box.line.captured_ms = now_ms;
    s_box.line.sequence++;
    atomic_flag_clear_explicit(&s_mailbox, memory_order_release);
    atomic_store(&s_per_row, s->n);
    atomic_fetch_add(&s_rows, 1);
}

static bool sink(void *ctx, const uint8_t *db, uint32_t chip_us)
{
    (void)chip_us;
    session_t *s = ctx;
    if (atomic_load(&s->stop)) return false;
    for (unsigned i = 0; i < 256; i++) s->acc[i] += s_pow[db[i]];
    s->n++;
    atomic_fetch_add(&s_frames, 1);
    const int64_t now = esp_timer_get_time();
    if (now - s->row_t0 >= LR_SPEC_PERIOD_MS * 1000) {
        publish(s, (uint32_t)(now / 1000));
        memset(s->acc, 0, sizeof(s->acc));
        s->n = 0;
        s->row_t0 = now;
    }
    return true;
}

static void owner(void *arg)
{
    session_t *s = arg;
    /* The chip program needs the LoRa owner configured, as the I/Q stream does. */
    esp_err_t err = ESP_OK;
    if (!ls_lora_cfg()) {
        ls_lora_cfg_t cfg;
        ls_lora_cfg_default(&cfg);
        err = ls_lora_configure(&cfg);
    }
    if (err == ESP_OK) {
        s->row_t0 = esp_timer_get_time();
        atomic_store(&s_center, s->hz);
        /* Two segments a frame: about 5 ms on the chip, ~20 frames a row. */
        const lr20xx_chip_spec_cfg_t cfg = {.hz = s->hz, .bw_hz = 62500, .nseg = 2, .gain = 0};
        lr20xx_chip_result_t r;
        err = lr20xx_chip_spectrum(&cfg, sink, s, &r);
    }
    atomic_store(&s_center, 0);
    s->error = err;
    atomic_store(&s_error, err);
    if (!atomic_load(&s->stop)) atomic_store(&s_failed, true);
    atomic_store_explicit(&s->done, true, memory_order_release);
    vTaskDelete(NULL);
}

static atomic_bool s_session_busy;
static void session_lock(void)
{
    while (atomic_exchange_explicit(&s_session_busy, true, memory_order_acquire)) vTaskDelay(1);
}
static void session_unlock(void) { atomic_store_explicit(&s_session_busy, false, memory_order_release); }

static void stop_locked(void)
{
    if (!session) return;
    atomic_store(&session->stop, true);
    while (!atomic_load_explicit(&session->done, memory_order_acquire)) vTaskDelay(1);
    heap_caps_free(session);
    session = NULL;
}

void lr_chip_wf_stop(void)
{
    session_lock();
    stop_locked();
    atomic_store(&s_failed, false);
    session_unlock();
}

esp_err_t lr_chip_wf_start(uint32_t hz)
{
    session_lock();
    esp_err_t err = ESP_OK;
    if (session && session->hz != hz) stop_locked();
    if (session) {
        err = atomic_load_explicit(&session->done, memory_order_acquire) ? session->error : ESP_OK;
    } else if (ls_lora_chip() != LS_LORA_CHIP_LR20XX) {
        err = ESP_ERR_NOT_SUPPORTED;
    } else if (!ls_lora_packet_rx_hz_ok(ls_lora_caps(), hz)) {
        err = ESP_ERR_INVALID_ARG;
    } else if (lr20xx_stream_source2_active() || ls_lora_fsk_active() || ls_lora_scanning()) {
        err = ESP_ERR_INVALID_STATE;
    } else {
        if (!s_pow_ready) {
            for (unsigned i = 0; i < 256; i++) s_pow[i] = powf(10.0f, (float)i / 20.0f);
            s_pow_ready = true;
        }
        session_t *s = heap_caps_calloc(1, sizeof(*s), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s) err = ESP_ERR_NO_MEM;
        else {
            atomic_init(&s->stop, false);
            atomic_init(&s->done, false);
            s->hz = hz;
            atomic_store(&s_failed, false);
            /* Core 0 with the radio tasks; it sleeps in every BUSY wait. */
            if (xTaskCreatePinnedToCore(owner, "wf_chip", 4096, s, 10, NULL, 0) != pdPASS) {
                heap_caps_free(s);
                err = ESP_ERR_NO_MEM;
            } else session = s;
        }
    }
    session_unlock();
    return err;
}

bool lr_chip_wf_read(float *bins, lr_spec_line *line, uint32_t now_ms)
{
    if (!bins || !line || !atomic_load(&s_center) ||
        atomic_flag_test_and_set_explicit(&s_mailbox, memory_order_acquire)) return false;
    const bool ok = s_box.line.sequence && s_box.line.center_hz == atomic_load(&s_center) &&
                    (uint32_t)(now_ms - s_box.line.captured_ms) <= 500;
    if (ok) { memcpy(bins, s_box.bins, sizeof(s_box.bins)); *line = s_box.line; }
    atomic_flag_clear_explicit(&s_mailbox, memory_order_release);
    return ok;
}

bool lr_chip_wf_failed(void) { return atomic_load(&s_failed); }

void lr_chip_wf_status(lr_chip_wf_status_t *out)
{
    if (!out) return;
    out->active = atomic_load(&s_center) != 0;
    out->hz = atomic_load(&s_center);
    out->rows = atomic_load(&s_rows);
    out->frames = atomic_load(&s_frames);
    out->frames_per_row = atomic_load(&s_per_row);
    out->error = (esp_err_t)atomic_load(&s_error);
}
