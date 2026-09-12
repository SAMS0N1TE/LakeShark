#include "ls_test.h"
#include "ble_hci_rx_guard.h"
#include <stddef.h>

extern int hci_rx_handler(uint8_t *buf, size_t len);
extern unsigned ls_hci_test_calls;
extern uint8_t *ls_hci_test_buf;
extern size_t ls_hci_test_len;
static int init_calls;
static uint8_t packet[] = {4, 14, 1, 0};
extern uint8_t ls_transport_test_ready;
extern int ls_transport_test_result, ls_transport_test_calls;
extern int transport_drv_reconfigure(void);

LS_CASE(an_active_transport_does_not_restart_the_init_watchdog)
{
    ls_transport_test_ready = 0;
    ls_transport_test_result = ESP_FAIL;
    LS_EQ_INT(transport_drv_reconfigure(), ESP_FAIL);
    LS_EQ_INT(ls_transport_test_calls, 1);
    ls_transport_test_ready = 1;
    LS_EQ_INT(transport_drv_reconfigure(), ESP_OK);
    LS_EQ_INT(transport_drv_reconfigure(), ESP_OK);
    LS_EQ_INT(ls_transport_test_calls, 1);
    ls_transport_test_ready = 0;
    ls_transport_test_result = ESP_OK;
    LS_EQ_INT(transport_drv_reconfigure(), ESP_OK);
    LS_EQ_INT(ls_transport_test_calls, 2);
}

static esp_err_t fail_init(void)
{
    init_calls++;
    LS_EQ_INT(hci_rx_handler(packet, sizeof(packet)), ESP_ERR_INVALID_STATE);
    return ESP_ERR_NO_MEM;
}
static esp_err_t good_init(void)
{
    init_calls++;
    LS_EQ_INT(hci_rx_handler(packet, sizeof(packet)), ESP_ERR_INVALID_STATE);
    return ESP_OK;
}

static int ci_rc, cd_rc, ce_rc, ci_count, cd_count, ce_count;
static esp_err_t controller_init(void) { ci_count++; return ci_rc; }
static esp_err_t controller_disable(void) { cd_count++; return cd_rc; }
static esp_err_t controller_enable(void) { ce_count++; return ce_rc; }
static int controller_run(int init, int disable, int enable)
{
    ci_rc = init; cd_rc = disable; ce_rc = enable;
    ci_count = cd_count = ce_count = 0;
    return ble_hci_controller_prepare(controller_init, controller_disable, controller_enable);
}

LS_CASE(controller_start_accepts_cold_and_already_running_slaves)
{
    LS_EQ_INT(controller_run(ESP_OK, ESP_OK, ESP_OK), ESP_OK);
    LS_EQ_INT(ci_count, 1); LS_EQ_INT(cd_count, 0); LS_EQ_INT(ce_count, 1);
    LS_EQ_INT(controller_run(ESP_ERR_INVALID_STATE, ESP_OK, ESP_OK), ESP_OK);
    LS_EQ_INT(cd_count, 1); LS_EQ_INT(ce_count, 1);
    LS_EQ_INT(controller_run(ESP_ERR_INVALID_STATE, ESP_ERR_INVALID_STATE, ESP_OK), ESP_OK);
    LS_EQ_INT(ce_count, 1);
}

LS_CASE(controller_start_preserves_actual_errors)
{
    LS_EQ_INT(controller_run(ESP_ERR_NOT_SUPPORTED, ESP_OK, ESP_OK), ESP_ERR_NOT_SUPPORTED);
    LS_EQ_INT(cd_count, 0); LS_EQ_INT(ce_count, 0);
    LS_EQ_INT(controller_run(ESP_ERR_INVALID_STATE, ESP_FAIL, ESP_OK), ESP_FAIL);
    LS_EQ_INT(ce_count, 0);
    LS_EQ_INT(controller_run(ESP_OK, ESP_OK, ESP_ERR_INVALID_STATE), ESP_ERR_INVALID_STATE);
    LS_EQ_INT(ce_count, 1);
}

LS_CASE(early_and_failed_init_packets_never_reach_nimble)
{
    LS_EQ_INT(hci_rx_handler(packet, sizeof(packet)), ESP_ERR_INVALID_STATE);
    LS_EQ_INT(ble_hci_rx_prepare(NULL), ESP_ERR_INVALID_ARG);
    LS_EQ_INT(ble_hci_rx_prepare(fail_init), ESP_ERR_NO_MEM);
    LS_EQ_INT(hci_rx_handler(packet, sizeof(packet)), ESP_ERR_INVALID_STATE);
    LS_EQ_INT(ls_hci_test_calls, 0);
    LS_EQ_INT(ble_hci_rx_prepare(good_init), ESP_OK);
    LS_EQ_INT(ls_hci_test_calls, 0);
    LS_EQ_INT(ble_hci_rx_early_packets(), 4);
    LS_EQ_INT(hci_rx_handler(packet, sizeof(packet)), 73);
    LS_EQ_INT(ls_hci_test_calls, 1);
    LS_CHECK(ls_hci_test_buf == packet);
    LS_EQ_INT(ls_hci_test_len, sizeof(packet));
    LS_EQ_INT(ble_hci_rx_prepare(good_init), ESP_OK);
    LS_EQ_INT(init_calls, 2);
}
