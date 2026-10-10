#pragma once
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Serialize UART selection/install/configuration across GPS and Flipper. */
esp_err_t ls_uart_claim_lock(void);
void ls_uart_claim_unlock(void);
#ifdef __cplusplus
}
#endif
