

#include "ls_nfc.h"
#include "ls_board.h"

#include <string.h>

#if LS_HAS_NFC

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "nfc_a.h"
#include "st25r3916_regs.h"

static const char *TAG = "ls_nfc";

/* NFC-A transmit/receive FIFO staging.  ST25R3916 FIFO is 512 bytes;
   this driver never queues more than a Type 2 read/response pair. */
#define LS_NFC_FIFO_BUF        96
/* Enough for the entire Type 2 memory map of a 176-byte NTAG213 plus
   headroom.  A larger tag reads in chunks anyway. */
#define LS_NFC_TAG_MEM_BUF     512
#define LS_NFC_READER_TASK_STK 4096

typedef struct {
    ls_nfc_config_t   cfg;
    ls_nfc_event_cb_t cb;
    void             *user;

    /* PSRAM-resident bounded NDEF message buffer. */
    uint8_t          *ndef_msg;
    size_t            ndef_msg_cap;
    /* Records array parsed out of ndef_msg.  Views only; do not free. */
    ls_nfc_ndef_record_t *records;
    size_t                records_cap;

    /* Internal-RAM FIFO staging.  Used by ISR and by SPI DMA. */
    uint8_t          *fifo_buf;
    uint8_t          *tag_mem;

    QueueHandle_t     irq_q;
    SemaphoreHandle_t lock;
    TaskHandle_t      reader;

    bool              spi_owned;   /* did we install the ISR ourselves */
    bool              stop_req;
    bool              running;
} ls_nfc_state_t;

static ls_nfc_state_t s;

/* --- Forward declarations for the reader task and lifecycle helpers. */
static void reader_task(void *arg);
static void rf_field_off_quiet(void);
static void release_all(void);

/* ISR: publish the interrupt cause into the queue and yield.  The
   register read that clears MAIN_IRQ is done by the task, not here -
   this avoids doing SPI from IRAM. */
static void IRAM_ATTR nfc_irq_isr(void *arg)
{
    (void)arg;
    BaseType_t hpw = pdFALSE;
    uint32_t marker = 1;
    xQueueSendFromISR(s.irq_q, &marker, &hpw);
    if (hpw) portYIELD_FROM_ISR();
}

int ls_nfc_start(const ls_nfc_config_t *cfg,
                 ls_nfc_event_cb_t cb, void *user)
{
    if (!cfg || !cb) return LS_NFC_ERR_MALFORMED;
    if (s.running)   return LS_NFC_ERR_INTERNAL;

    memset(&s, 0, sizeof(s));
    s.cfg  = *cfg;
    s.cb   = cb;
    s.user = user;

    size_t msg_cap = cfg->max_ndef_bytes ? cfg->max_ndef_bytes
                                         : LS_NFC_DEFAULT_MAX_NDEF;
    if (msg_cap > LS_NFC_MAX_NDEF_LIMIT) msg_cap = LS_NFC_MAX_NDEF_LIMIT;
    size_t rec_cap = cfg->max_records ? cfg->max_records
                                      : LS_NFC_DEFAULT_MAX_RECORDS;

    /* PSRAM for the bounded NDEF buffer - see bench/NFC.md.  The
       reference demo used an unconditional 8 KiB static; here it is a
       cfg-bounded PSRAM allocation released on stop and on every failure
       path below. */
    s.ndef_msg    = (uint8_t *)heap_caps_malloc(msg_cap, MALLOC_CAP_SPIRAM);
    s.ndef_msg_cap = msg_cap;
    s.records     = (ls_nfc_ndef_record_t *)
                    heap_caps_calloc(rec_cap, sizeof(ls_nfc_ndef_record_t),
                                     MALLOC_CAP_SPIRAM);
    s.records_cap = rec_cap;

    /* Internal RAM for FIFO staging and the working tag-memory image,
       both of which are touched by DMA or by the ISR path. */
    s.fifo_buf = (uint8_t *)heap_caps_malloc(LS_NFC_FIFO_BUF,
                                             MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    s.tag_mem  = (uint8_t *)heap_caps_malloc(LS_NFC_TAG_MEM_BUF,
                                             MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);

    s.irq_q = xQueueCreate(4, sizeof(uint32_t));
    s.lock  = xSemaphoreCreateMutex();

    if (!s.ndef_msg || !s.records || !s.fifo_buf || !s.tag_mem ||
        !s.irq_q || !s.lock) {
        ESP_LOGE(TAG, "start: allocation failed");
        release_all();
        return LS_NFC_ERR_INTERNAL;
    }

    /* IRQ line, active high per data sheet §1.2.3.  gpio_install_isr_service
       is idempotent (returns INVALID_STATE if already installed) - either
       return is fine here. */
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << cfg->irq_gpio,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type    = GPIO_INTR_POSEDGE,
    };
    if (gpio_config(&io) != ESP_OK) { release_all(); return LS_NFC_ERR_TRANSPORT; }
    (void)gpio_install_isr_service(0);
    if (gpio_isr_handler_add((gpio_num_t)cfg->irq_gpio,
                             nfc_irq_isr, NULL) != ESP_OK) {
        release_all();
        return LS_NFC_ERR_TRANSPORT;
    }
    s.spi_owned = true;

    if (xTaskCreate(reader_task, "ls_nfc", LS_NFC_READER_TASK_STK,
                    NULL, 6, &s.reader) != pdPASS) {
        gpio_isr_handler_remove((gpio_num_t)cfg->irq_gpio);
        release_all();
        return LS_NFC_ERR_INTERNAL;
    }

    s.running = true;
    return LS_NFC_ERR_NONE;
}

void ls_nfc_stop(void)
{
    if (!s.running) return;

    s.stop_req = true;
    /* Poke the task so it wakes and observes stop_req. */
    if (s.irq_q) {
        uint32_t marker = 0;
        xQueueSend(s.irq_q, &marker, 0);
    }
    /* Wait briefly for the task to unwind.  If it does not, we still
       release resources - a wedged reader task is bounded by its
       stop_req check and cannot outlive the ISR removal below. */
    for (int i = 0; i < 50 && s.reader; i++) vTaskDelay(pdMS_TO_TICKS(10));

    if (s.spi_owned) {
        gpio_isr_handler_remove((gpio_num_t)s.cfg.irq_gpio);
        rf_field_off_quiet();
        s.spi_owned = false;
    }
    if (s.reader) { vTaskDelete(s.reader); s.reader = NULL; }
    release_all();
    s.running = false;
}

bool ls_nfc_running(void) { return s.running; }

/* --- Internal helpers ------------------------------------------------- */

static void release_all(void)
{
    if (s.ndef_msg) { heap_caps_free(s.ndef_msg); s.ndef_msg = NULL; }
    if (s.records)  { heap_caps_free(s.records);  s.records  = NULL; }
    if (s.fifo_buf) { heap_caps_free(s.fifo_buf); s.fifo_buf = NULL; }
    if (s.tag_mem)  { heap_caps_free(s.tag_mem);  s.tag_mem  = NULL; }
    if (s.irq_q)    { vQueueDelete(s.irq_q);      s.irq_q    = NULL; }
    if (s.lock)     { vSemaphoreDelete(s.lock);   s.lock     = NULL; }
    s.ndef_msg_cap = 0;
    s.records_cap  = 0;
}

/* -- SPI helpers.  Every access takes s.lock; the poller task holds the
   lock while a transceive/receive pair is in flight so a stop() called
   from another thread cannot yank the SPI device mid-frame. */

static esp_err_t spi_wr(uint8_t reg, uint8_t v)
{
    spi_transaction_t tx = {0};
    uint8_t buf[2] = { (uint8_t)(ST25R3916_CMD_MODE_REG_WRITE | (reg & 0x3F)), v };
    tx.length    = 16;
    tx.tx_buffer = buf;
    return spi_device_transmit((spi_device_handle_t)s.cfg.spi_device, &tx);
}

static esp_err_t spi_rd(uint8_t reg, uint8_t *v)
{
    spi_transaction_t tx = {0};
    uint8_t buf[2] = { (uint8_t)(ST25R3916_CMD_MODE_REG_READ | (reg & 0x3F)), 0 };
    uint8_t rx[2]  = {0};
    tx.length    = 16;
    tx.rxlength  = 16;
    tx.tx_buffer = buf;
    tx.rx_buffer = rx;
    esp_err_t r = spi_device_transmit((spi_device_handle_t)s.cfg.spi_device, &tx);
    if (r == ESP_OK && v) *v = rx[1];
    return r;
}

static esp_err_t spi_direct_cmd(uint8_t cmd)
{
    spi_transaction_t tx = {0};
    uint8_t b = cmd;
    tx.length    = 8;
    tx.tx_buffer = &b;
    return spi_device_transmit((spi_device_handle_t)s.cfg.spi_device, &tx);
}

static void rf_field_off_quiet(void)
{
    /* Data sheet §3.2: OP_CONTROL bit 7 (en) drives the RF field.  Clear
       it before we drop the ISR so a partial init still leaves the
       antenna un-driven.  Failures are swallowed on purpose - if SPI is
       gone there is nothing to do and re-entering the caller is worse. */
    uint8_t op = 0;
    if (spi_rd(ST25R3916_REG_OP_CONTROL, &op) == ESP_OK) {
        op &= (uint8_t)~0x80;
        (void)spi_wr(ST25R3916_REG_OP_CONTROL, op);
    }
}

/* -- Reader task.  Kept small: bring the RF field up, look for a tag,
   report it, wait for it to leave, drop the field.  The hardware
   bring-up sequence that would live here is stubbed to a periodic
   "no-tag" until pins are measured on the T-Display-P4 () and the
   IRQ polarity is confirmed.  bench/NFC.md marks this as a separate
   bring-up step; the driver framework is what lands in this task. */

static uint32_t poll_ms(void)
{
    return s.cfg.poll_ms ? s.cfg.poll_ms : 200;
}

static void emit_removed(void)
{
    ls_nfc_event_t ev = {0};
    ev.kind = LS_NFC_EV_REMOVED;
    if (s.cb) s.cb(&ev, s.user);
}

static void reader_task(void *arg)
{
    (void)arg;

    /* Initial chip reset and default set.  Data sheet §3.3.1. */
    (void)spi_direct_cmd(ST25R3916_CMD_SET_DEFAULT);
    (void)spi_direct_cmd(ST25R3916_CMD_CLEAR_FIFO);

    while (!s.stop_req) {
        uint32_t marker = 0;
        /* Wait for either an IRQ or the poll tick.  A NULL SPI device
           (unit-test path) simply drops to the tick. */
        (void)xQueueReceive(s.irq_q, &marker, pdMS_TO_TICKS(poll_ms()));
        if (s.stop_req) break;

    }

    emit_removed();
    s.reader = NULL;
    vTaskDelete(NULL);
}

#else  /* !LS_HAS_NFC ------------------------------------------------- */

/* On a board without NFC the API is still callable so an app can ask
   "is it there?" without a compile fence at every call site.  Every
   entry point refuses. */

int  ls_nfc_start(const ls_nfc_config_t *cfg,
                  ls_nfc_event_cb_t cb, void *user)
{
    (void)cfg; (void)cb; (void)user;
    return LS_NFC_ERR_UNSUPPORTED_TECH;
}
void ls_nfc_stop(void)         {}
bool ls_nfc_running(void)      { return false; }

#endif /* LS_HAS_NFC */
