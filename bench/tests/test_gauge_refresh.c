#include "ls_test.h"
#include "ls_gauge.h"
#include "ls_gauge_soc.h"
#include "ls_i2c.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include <setjmp.h>

/* The sampler as the board runs it: a full read, then voltage and current
   every 5 s, the cache read by every screen, nothing on the bus from a draw. */

static TaskFunction_t worker;
static unsigned reads, waits;
static uint16_t voltage = 4145;
static int16_t current = -300;
static bool fail;
static jmp_buf finished;
static void (*on_wait)(void);
esp_err_t ls_i2c_device(ls_i2c_bus_id_t bus, uint8_t addr, uint32_t hz,
                        i2c_master_dev_handle_t *out)
{ (void)bus; (void)addr; (void)hz; *out = (void *)1; return ESP_OK; }
esp_err_t ls_i2c_probe(ls_i2c_bus_id_t bus, uint8_t addr, int ms)
{ (void)bus; (void)addr; (void)ms; return ESP_OK; }
esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t dev,
    const uint8_t *tx, size_t nt, uint8_t *rx, size_t nr, int ms)
{
    (void)dev; (void)nt; (void)nr; (void)ms;
    reads++;
    if (fail) return ESP_FAIL;
    uint16_t v = *tx == 0x08 ? voltage : *tx == 0x0C ? (uint16_t)current : 0;
    rx[0] = (uint8_t)v; rx[1] = (uint8_t)(v >> 8);
    return ESP_OK;
}
BaseType_t xTaskCreate(TaskFunction_t task, const char *name, uint32_t stack,
    void *arg, UBaseType_t priority, TaskHandle_t *handle)
{
    (void)name; (void)stack; (void)arg;
    LS_EQ_INT(priority, 1);
    worker = task; *handle = (void *)1; return pdPASS;
}
void vTaskDelay(TickType_t ticks)
{
    LS_EQ_INT(ticks, pdMS_TO_TICKS(5000));
    waits++;
    on_wait();
    ls_shim_time_advance(5000000);
}

/* ---- the cache stays cache, and a bad read keeps the last good one ---- */

static unsigned first_reads;
static uint8_t first_pct;
static void cache_checks(void)
{
    ls_gauge_t g;
    const unsigned before = reads;
    LS_CHECK(ls_gauge_get(&g));
    LS_EQ_INT(reads, before);              /* cache-only */
    if (waits == 1) {
        first_reads = reads;
        first_pct = g.percent;
        LS_EQ_INT(g.state, LS_BATT_DISCHARGING);
        LS_CHECK(!g.plugged);
        LS_EQ_INT(g.milliamps, -300);
    }
    if (waits == 2) LS_EQ_INT(reads - first_reads, 2);  /* voltage and current only */
    if (waits == 3) fail = true;
    if (waits == 4) {
        int64_t age;
        LS_CHECK(ls_gauge_peek(NULL, &age));
        LS_EQ_INT(age, 5000000);               /* the failed read kept the old sample */
        LS_EQ_INT(g.percent, first_pct);
        longjmp(finished, 1);
    }
}

LS_CASE(battery_refresh_runs_on_its_own_timer_and_only_reads_what_moves)
{
    ls_shim_time_set(100);
    on_wait = cache_checks;
    LS_EQ_INT(ls_gauge_start(), ESP_OK);
    LS_CHECK(worker != NULL);
    if (!setjmp(finished)) worker(NULL);
    LS_EQ_INT(waits, 4);
    /* 4145 mV under 300 mA rests at 4175: a full cell, shown as 100 on battery. */
    LS_EQ_INT(first_pct, 100);
}
