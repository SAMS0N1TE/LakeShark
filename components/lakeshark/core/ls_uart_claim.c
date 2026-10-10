#include "ls_uart_claim.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static StaticSemaphore_t s_storage;
static SemaphoreHandle_t s_lock;
static portMUX_TYPE s_make_lock = portMUX_INITIALIZER_UNLOCKED;

esp_err_t ls_uart_claim_lock(void)
{
    portENTER_CRITICAL(&s_make_lock);
    if (!s_lock) s_lock = xSemaphoreCreateMutexStatic(&s_storage);
    portEXIT_CRITICAL(&s_make_lock);
    if (!s_lock) return ESP_ERR_NO_MEM;
    return xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE ? ESP_OK : ESP_FAIL;
}
void ls_uart_claim_unlock(void)
{
    xSemaphoreGive(s_lock);
}
