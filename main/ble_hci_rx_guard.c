#include "ble_hci_rx_guard.h"
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>

static atomic_bool s_ready;
static atomic_uint_least32_t s_early;

int __real_hci_rx_handler(uint8_t *buf, size_t len);
uint8_t is_transport_tx_ready(void);
esp_err_t __real_transport_drv_reconfigure(void);

/* Hosted 2.12.9 arms its init timer even when the transport is already up. */
esp_err_t __wrap_transport_drv_reconfigure(void)
{
    return is_transport_tx_ready() ? ESP_OK : __real_transport_drv_reconfigure();
}

/* SDIO delivers HCI before NimBLE has allocated its NPL pools. */
int __wrap_hci_rx_handler(uint8_t *buf, size_t len)
{
    if (!atomic_load_explicit(&s_ready, memory_order_acquire)) {
        atomic_fetch_add_explicit(&s_early, 1, memory_order_relaxed);
        return ESP_ERR_INVALID_STATE;
    }
    return __real_hci_rx_handler(buf, len);
}

esp_err_t ble_hci_rx_prepare(esp_err_t (*init)(void))
{
    if (atomic_load_explicit(&s_ready, memory_order_acquire)) return ESP_OK;
    if (!init) return ESP_ERR_INVALID_ARG;
    esp_err_t rc = init();
    if (rc == ESP_OK) atomic_store_explicit(&s_ready, true, memory_order_release);
    return rc;
}

uint32_t ble_hci_rx_early_packets(void)
{
    return atomic_load_explicit(&s_early, memory_order_relaxed);
}

esp_err_t ble_hci_controller_prepare(esp_err_t (*init)(void),
    esp_err_t (*disable)(void), esp_err_t (*enable)(void))
{
    if (!init || !disable || !enable) return ESP_ERR_INVALID_ARG;
    esp_err_t rc = init();
    if (rc == ESP_ERR_INVALID_STATE) {
        /* Hosted can boot with BT enabled; re-register HCI on a fresh enable. */
        rc = disable();
        if (rc != ESP_OK && rc != ESP_ERR_INVALID_STATE) return rc;
    } else if (rc != ESP_OK) {
        return rc;
    }
    return enable();
}
