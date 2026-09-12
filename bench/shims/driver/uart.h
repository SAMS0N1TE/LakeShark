/* Host shim. Minimal UART declarations for compiling the Flipper command
   parser; tests do not access a UART. */
#ifndef LS_SHIM_DRIVER_UART_H
#define LS_SHIM_DRIVER_UART_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    int baud_rate;
    int data_bits;
    int parity;
    int stop_bits;
    int flow_ctrl;
    int source_clk;
} uart_config_t;

/* The port is an int on the device too; the typedef exists so drivers can
   hold one without knowing that. SOC_UART_HP_NUM and the console number let a
   driver pick a free port, which the GPS reader does because the Flipper link
   claims one at boot and does not always land on the same one. */
typedef int uart_port_t;
#ifndef SOC_UART_HP_NUM
#define SOC_UART_HP_NUM 5
#endif
#ifndef CONFIG_ESP_CONSOLE_UART_NUM
#define CONFIG_ESP_CONSOLE_UART_NUM 0
#endif

#define UART_DATA_8_BITS        8
#define UART_PARITY_DISABLE     0
#define UART_STOP_BITS_1        1
#define UART_HW_FLOWCTRL_DISABLE 0
#define UART_SCLK_DEFAULT       0
#define UART_PIN_NO_CHANGE      (-1)

bool uart_is_driver_installed(int uart_num);
esp_err_t uart_driver_install(int uart_num, int rx_buffer_size,
                              int tx_buffer_size, int queue_size,
                              void *queue, int intr_alloc_flags);
esp_err_t uart_driver_delete(int uart_num);
esp_err_t uart_param_config(int uart_num, const uart_config_t *config);
esp_err_t uart_set_pin(int uart_num, int tx, int rx, int rts, int cts);
esp_err_t uart_flush_input(int uart_num);
esp_err_t uart_get_buffered_data_len(int uart_num, size_t *size);
int uart_read_bytes(int uart_num, void *buffer, uint32_t length,
                    uint32_t ticks_to_wait);
int uart_write_bytes(int uart_num, const void *source, size_t size);

#endif
