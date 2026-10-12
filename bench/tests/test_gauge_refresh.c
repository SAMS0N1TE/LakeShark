#include "ls_test.h"
#include "ls_gauge.h"
#include "ls_i2c.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include <setjmp.h>

static TaskFunction_t worker;
static unsigned reads, waits;
static uint16_t voltage = 4145;
static bool fail;
static jmp_buf finished;
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
    uint16_t v = *tx == 0x08 ? voltage : 0; /* zero current, no charger event */
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
    LS_EQ_INT(ticks, pdMS_TO_TICKS(30000));
    ls_gauge_t g;
    unsigned before = reads;
    LS_CHECK(ls_gauge_get(&g));
    LS_EQ_INT(reads, before); /* cache-only even with time advanced */
    LS_EQ_INT(g.milliamps, 0);
    LS_CHECK(!g.charging);
    LS_EQ_INT(g.percent, waits == 0 ? 95 : 50);
    int64_t age;
    LS_CHECK(ls_gauge_peek(NULL, &age));
    LS_EQ_INT(age, waits == 2 ? 30000000 : 0);
    if (++waits == 3) longjmp(finished, 1);
    voltage = 3790;
    fail = waits == 2; /* third read fails; retain last good sample */
    ls_shim_time_advance(30000000);
}
LS_CASE(battery_refresh_runs_on_timer_without_draws_or_charger_events)
{
    ls_shim_time_set(100);
    LS_EQ_INT(ls_gauge_start(), ESP_OK);
    LS_CHECK(worker != NULL);
    if (!setjmp(finished)) worker(NULL);
    LS_EQ_INT(waits, 3);
    LS_EQ_INT(reads, 14); /* probe voltage + two complete samples + failed voltage */
    unsigned before = reads;
    ls_shim_time_advance(60000000);
    ls_gauge_t g;
    LS_CHECK(ls_gauge_get(&g));
    LS_EQ_INT(reads, before);
}
