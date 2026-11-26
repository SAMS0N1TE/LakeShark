#include "tk21.h"
#include "driver/gpio.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_check.h"
#include "esp_log.h"
#include <math.h>

enum { X, Y, LADDER, ANALOG_INPUTS };
static const char *TAG = "tk21";
static gpio_num_t key;
static struct {
    adc_unit_t unit;
    adc_channel_t channel;
} analog[ANALOG_INPUTS];
static adc_oneshot_unit_handle_t units[SOC_ADC_PERIPH_NUM];
static adc_cali_handle_t ladder_cal;
static int center[2], raw_axis[2], ladder_mv;
static float filtered[2], direction[2];
static uint8_t held, candidate, stable_count;

static int average(int signal, int samples, int fallback)
{
    int sum = 0, good = 0;
    for (int i = 0; i < samples; ++i) {
        int raw;
        if (adc_oneshot_read(units[analog[signal].unit], analog[signal].channel, &raw) == ESP_OK) {
            sum += raw;
            ++good;
        }
    }
    return good ? sum / good : fallback;
}

esp_err_t tk21_init(const tk21_pinout *pins)
{
    if (!pins || !GPIO_IS_VALID_GPIO(pins->key_gpio)) return ESP_ERR_INVALID_ARG;
    if (ladder_cal) return ESP_ERR_INVALID_STATE;

    const int pin[] = {pins->x_gpio, pins->y_gpio, pins->buttons_gpio};
    const adc_oneshot_chan_cfg_t cfg = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12 };
    for (int i = 0; i < ANALOG_INPUTS; ++i) {
        if (pin[i] == pins->key_gpio) return ESP_ERR_INVALID_ARG;
        for (int j = 0; j < i; ++j)
            if (pin[i] == pin[j]) return ESP_ERR_INVALID_ARG;
        ESP_RETURN_ON_ERROR(adc_oneshot_io_to_channel(pin[i], &analog[i].unit, &analog[i].channel),
                            TAG, "TK21 needs three ADC pins");
    }

    for (int i = 0; i < ANALOG_INPUTS; ++i) {
        adc_unit_t unit = analog[i].unit;
        if (!units[unit]) {
            const adc_oneshot_unit_init_cfg_t init = { .unit_id = unit };
            ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&init, &units[unit]), TAG, "ADC unit");
        }
        ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(units[unit], analog[i].channel, &cfg),
                            TAG, "ADC channel");
    }
    for (int i = 0; i < 2; ++i) {
        center[i] = raw_axis[i] = average(i, 24, 2048);
        if (center[i] < 700 || center[i] > 3400) center[i] = 2048;
        filtered[i] = center[i];
    }
    direction[X] = pins->invert_x ? -1 : 1;
    direction[Y] = pins->invert_y ? -1 : 1;
    key = pins->key_gpio;
    /* The TK21 has its own 10 kOhm pull-down and drives KEY high on click. */
    const gpio_config_t key_cfg = { .pin_bit_mask = 1ULL << key, .mode = GPIO_MODE_INPUT };
    ESP_RETURN_ON_ERROR(gpio_config(&key_cfg), TAG, "stick switch");

    const adc_cali_curve_fitting_config_t cal = {
        .unit_id = analog[LADDER].unit, .chan = analog[LADDER].channel,
        .atten = cfg.atten, .bitwidth = cfg.bitwidth,
    };
    ESP_RETURN_ON_ERROR(adc_cali_create_scheme_curve_fitting(&cal, &ladder_cal), TAG,
                        "ladder calibration");
    ESP_LOGI(TAG, "GPIO%d/%d/%d/%d ready, stick center %d/%d",
             pin[X], pin[Y], pin[LADDER], key, center[X], center[Y]);
    return ESP_OK;
}

static float normalized(float value, int zero)
{
    int range = value < zero ? zero : 4095 - zero;
    float n = (value - zero) / range;
    const float dead = 0.13f;
    if (fabsf(n) < dead) return 0;
    return copysignf(fminf(1, (fabsf(n) - dead) / (0.78f - dead)), n);
}

tk21_state tk21_poll(void)
{
    if (!ladder_cal) return (tk21_state){0};
    const int threshold[] = {2475, 1375, 963, 743, 605, 275};
    uint8_t raw_buttons = 0;
    for (int i = 0; i < 2; ++i) {
        raw_axis[i] = average(i, 4, raw_axis[i]);
        filtered[i] += (raw_axis[i] - filtered[i]) * 0.45f;
    }
    int raw = average(LADDER, 8, -1);
    if (raw >= 0 && adc_cali_raw_to_voltage(ladder_cal, raw, &ladder_mv) == ESP_OK) {
        for (int i = 0; i < 6; ++i) {
            if (ladder_mv >= threshold[i]) {
                raw_buttons = 1u << i;
                break;
            }
        }
    } else {
        raw_buttons = candidate & ~TK21_KEY;
    }
    if (gpio_get_level(key)) raw_buttons |= TK21_KEY;
    uint8_t previous = held;
    if (raw_buttons != candidate) {
        candidate = raw_buttons;
        stable_count = 1;
    } else if (stable_count < 2 && ++stable_count == 2) {
        held = candidate;
    }
    return (tk21_state){
        direction[X] * normalized(filtered[X], center[X]),
        direction[Y] * normalized(filtered[Y], center[Y]), held, held & ~previous,
    };
}

tk21_raw_state tk21_read_raw(void)
{
    return (tk21_raw_state){
        .axis = {(int)(filtered[X] + 0.5f), (int)(filtered[Y] + 0.5f)},
        .center = {center[X], center[Y]}, .ladder_mv = ladder_mv,
    };
}

void tk21_log(void)
{
    if (!ladder_cal) return;
    ESP_LOGI(TAG, "raw=%d/%d ladder=%dmV key=%d held=0x%02x", raw_axis[X], raw_axis[Y],
             ladder_mv, gpio_get_level(key), held);
}
