#include <stddef.h>
#include <stdint.h>

unsigned ls_hci_test_calls;
uint8_t *ls_hci_test_buf;
size_t ls_hci_test_len;
uint8_t ls_transport_test_ready;
int ls_transport_test_result, ls_transport_test_calls;

uint8_t is_transport_tx_ready(void) { return ls_transport_test_ready; }
int transport_drv_reconfigure(void)
{
    ls_transport_test_calls++;
    return ls_transport_test_result;
}

int hci_rx_handler(uint8_t *buf, size_t len)
{
    ls_hci_test_calls++;
    ls_hci_test_buf = buf;
    ls_hci_test_len = len;
    return 73;
}
