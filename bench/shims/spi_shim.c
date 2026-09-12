/* The host side of driver/spi_master.h. See the header for why it exists. */
#include "driver/spi_master.h"

#include <string.h>

#define MAX_LAST 32

static bool     s_bus_up[3];
static unsigned s_devices;
static unsigned s_transfers;
static uint8_t  s_last_tx[MAX_LAST];
static size_t   s_last_len;
static esp_err_t s_fail_init = ESP_OK;

static ls_shim_spi_responder_t s_fn;
static void *s_ctx;

/* Handles are never dereferenced by the drivers, only passed back, so a
   distinct non-null pointer per device is all they need to be. */
static struct spi_device_t { int slot; } s_pool[8];

void ls_shim_spi_reset(void)
{
    memset(s_bus_up, 0, sizeof(s_bus_up));
    s_devices = 0;
    s_transfers = 0;
    s_last_len = 0;
    s_fail_init = ESP_OK;
    s_fn = NULL;
    s_ctx = NULL;
}

void ls_shim_spi_reset_counters(void)
{
    s_transfers = 0;
    s_last_len = 0;
    s_fn = NULL;
    s_ctx = NULL;
}

void ls_shim_spi_on_transfer(ls_shim_spi_responder_t fn, void *ctx)
{
    s_fn = fn;
    s_ctx = ctx;
}

unsigned ls_shim_spi_transfers(void) { return s_transfers; }
unsigned ls_shim_spi_devices(void)   { return s_devices; }

const uint8_t *ls_shim_spi_last_tx(size_t *len)
{
    if (len) *len = s_last_len;
    return s_last_tx;
}

bool ls_shim_spi_bus_up(spi_host_device_t host)
{
    return (host >= 0 && host < 3) ? s_bus_up[host] : false;
}

void ls_shim_spi_fail_next_init(esp_err_t err) { s_fail_init = err; }

esp_err_t spi_bus_initialize(spi_host_device_t host,
                             const spi_bus_config_t *cfg, int dma)
{
    (void)cfg; (void)dma;
    if (s_fail_init != ESP_OK) {
        esp_err_t e = s_fail_init;
        s_fail_init = ESP_OK;
        return e;
    }
    if (host < 0 || host >= 3) return ESP_ERR_INVALID_ARG;
    /* The real driver refuses a second initialise of the same host, which is
       the case ls_spi.c's caching exists to avoid reaching. */
    if (s_bus_up[host]) return ESP_ERR_INVALID_STATE;
    s_bus_up[host] = true;
    return ESP_OK;
}

esp_err_t spi_bus_add_device(spi_host_device_t host,
                             const spi_device_interface_config_t *dev,
                             spi_device_handle_t *out)
{
    (void)dev;
    if (!out) return ESP_ERR_INVALID_ARG;
    if (host < 0 || host >= 3 || !s_bus_up[host]) return ESP_ERR_INVALID_STATE;
    if (s_devices >= 8) return ESP_ERR_NO_MEM;
    s_pool[s_devices].slot = (int)s_devices;
    *out = &s_pool[s_devices];
    s_devices++;
    return ESP_OK;
}

esp_err_t spi_device_transmit(spi_device_handle_t dev, spi_transaction_t *t)
{
    if (!dev || !t) return ESP_ERR_INVALID_ARG;
    size_t len = t->length / 8;

    /* Record what went out before the responder can overwrite it: the drivers
       hand the same buffer as tx and rx, so a responder that writes a reply
       destroys the evidence of what was asked. */
    s_last_len = len < MAX_LAST ? len : MAX_LAST;
    if (t->tx_buffer) memcpy(s_last_tx, t->tx_buffer, s_last_len);

    if (s_fn)
        s_fn((const uint8_t *)t->tx_buffer, (uint8_t *)t->rx_buffer, len, s_ctx);
    s_transfers++;
    return ESP_OK;
}

esp_err_t spi_device_acquire_bus(spi_device_handle_t dev, uint32_t wait)
{
    (void)wait;
    return dev ? ESP_OK : ESP_ERR_INVALID_ARG;
}

void spi_device_release_bus(spi_device_handle_t dev) { (void)dev; }
