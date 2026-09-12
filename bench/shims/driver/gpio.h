/* Host shim. Minimal GPIO declarations for compiling non-hardware tests. */
#ifndef LS_SHIM_DRIVER_GPIO_H
#define LS_SHIM_DRIVER_GPIO_H

#include <stdint.h>
#include "esp_err.h"

typedef int gpio_num_t;

typedef struct {
    uint64_t pin_bit_mask;
    int mode;
    int pull_up_en;
    int pull_down_en;
    int intr_type;
} gpio_config_t;

#define GPIO_MODE_INPUT       1
#define GPIO_MODE_OUTPUT      2
#define GPIO_PULLUP_DISABLE   0
#define GPIO_PULLUP_ENABLE    1
#define GPIO_PULLDOWN_DISABLE 0
#define GPIO_PULLDOWN_ENABLE  1
#define GPIO_INTR_DISABLE     0

/* the pull mode as its own call, which is how a UART receive pin is
   held at idle - gpio_config takes a whole descriptor and would undo the
   routing uart_set_pin just did. */
typedef enum {
    GPIO_PULLUP_ONLY,
    GPIO_PULLDOWN_ONLY,
    GPIO_PULLUP_PULLDOWN,
    GPIO_FLOATING,
} gpio_pull_mode_t;

esp_err_t gpio_set_pull_mode(gpio_num_t gpio, gpio_pull_mode_t pull);

esp_err_t gpio_config(const gpio_config_t *config);
esp_err_t gpio_set_level(gpio_num_t gpio, uint32_t level);
int gpio_get_level(gpio_num_t gpio);

#endif
