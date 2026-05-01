/*
 * app_main.c - boot orchestration for the ESP32-P4 SDR tool.
 *
 * Boot order matters:
 *   1. event_bus (everything publishes to it; even pre-boot logs need it ready)
 *   2. output sinks (subscribe to the bus before any events fire)
 *   3. settings (NVS open, before any app reads its saved freq/gain)
 *   4. apps register descriptors (no I/O yet, just metadata)
 *   5. audio init (boot beep proves codec is alive before USB enumeration)
 *   6. TUI task (if compiled in - owns screen and UART keys from this point)
 *   7. USB host + class driver (RTL-SDR enumeration and stream)
 *   8. perf heartbeat task
 *   9. main loop waits for quit button
 *
 * Output sinks are only built and installed when their Kconfig is set.
 * The TUI shares the same UART; if both are on, the TUI's log hook
 * captures ESP_LOGs into its log page so they don't trample the screen.
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_intr_alloc.h"
#include "usb/usb_host.h"
#include "driver/gpio.h"
#include "sdkconfig.h"

#include "app_registry.h"
#include "settings.h"
#include "event_bus.h"
#include "perf.h"
#include "audio_out.h"
#include "audio_events.h"
#include "sam_tts.h"

#ifdef CONFIG_ENABLE_TUI
#include "tui.h"
#include "tui_task.h"
#include "tui_log_hook.h"
#endif

#ifdef CONFIG_OUTPUT_HUMAN_LOG
#include "log_out.h"
#endif

#ifdef CONFIG_OUTPUT_EVENT_STREAM
#include "event_stream.h"
#endif

#include "diaglog.h"

#ifdef CONFIG_APP_ADSB
extern int adsb_app_register(void);
#endif
#ifdef CONFIG_APP_VHF
extern int vhf_app_register(void);
#endif
#ifdef CONFIG_APP_P25
extern int p25_app_register(void);
#endif

extern void class_driver_task(void *arg);
extern void class_driver_client_deregister(void);

#define HOST_LIB_TASK_PRIORITY  2
#define CLASS_TASK_PRIORITY     3
#define APP_QUIT_PIN            CONFIG_APP_QUIT_PIN
#define USB_VBUS_EN_GPIO        46

static const char *TAG = "main";

QueueHandle_t app_event_queue = NULL;

typedef enum { APP_EVENT = 0 } app_event_group_t;
typedef struct { app_event_group_t event_group; } app_event_queue_t;

static void gpio_cb(void *arg)
{
    const app_event_queue_t evt = { .event_group = APP_EVENT };
    BaseType_t woken = pdFALSE;
    if (app_event_queue) xQueueSendFromISR(app_event_queue, &evt, &woken);
    if (woken == pdTRUE) portYIELD_FROM_ISR();
}

static void usb_host_lib_task(void *arg)
{
    usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags     = ESP_INTR_FLAG_LEVEL1,
        .peripheral_map = BIT0,
    };
    ESP_ERROR_CHECK(usb_host_install(&host_config));
    xTaskNotifyGive(arg);

    bool has_clients = true;
    bool has_devices = false;
    while (has_clients) {
        uint32_t event_flags;
        ESP_ERROR_CHECK(usb_host_lib_handle_events(portMAX_DELAY, &event_flags));
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            if (ESP_OK == usb_host_device_free_all()) has_clients = false;
            else has_devices = true;
        }
        if (has_devices && (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE))
            has_clients = false;
    }
    ESP_ERROR_CHECK(usb_host_uninstall());
    vTaskSuspend(NULL);
}

void app_main(void)
{
    /* The USB host stack emits ISR-context messages via esp_rom_printf which
     * bypass any vprintf hook. Suppress them at the ESP_LOG layer to keep the
     * UART clean for the TUI or event stream. */
    esp_log_level_set("USBH",     ESP_LOG_NONE);
    esp_log_level_set("HUB",      ESP_LOG_NONE);
    esp_log_level_set("ENUM",     ESP_LOG_NONE);
    esp_log_level_set("USB_HOST", ESP_LOG_WARN);
    esp_log_level_set("CLASS",    ESP_LOG_WARN);

    event_bus_init();

#ifdef CONFIG_ENABLE_TUI
    tui_install_log_hook();
#endif
#ifdef CONFIG_OUTPUT_HUMAN_LOG
    log_out_init();
#endif
#ifdef CONFIG_OUTPUT_EVENT_STREAM
    event_stream_init();
#endif

    diaglog_init();

    ESP_LOGI(TAG, "ESP32-P4 SDR tool starting");

    settings_init();

#ifdef CONFIG_APP_ADSB
    adsb_app_register();
#endif
#ifdef CONFIG_APP_VHF
    vhf_app_register();
#endif
#ifdef CONFIG_APP_P25
    p25_app_register();
#endif

    /* Spin up the deferred-switch worker now that all apps are
     * registered. After this, any caller (TUI key handler, settings
     * page) that requests an app switch returns immediately and the
     * worker runs on_exit/on_enter on a dedicated thread, so the TUI
     * stays responsive even if an app's drain wait takes a few
     * seconds. */
    app_switch_worker_start();

    {
        int p  = settings_voice_preset_get();
        int lp = settings_voice_lowpass_get();
        int sh = settings_voice_lowshelf_get();
        if (p >= 0 && p < SAM_PRESET_COUNT) sam_tts_set_preset((sam_tts_voice_preset_t)p);
        sam_tts_set_lowpass(lp);
        sam_tts_set_lowshelf(sh);
    }

    if (audio_out_init() == ESP_OK) {
        audio_events_init();
        audio_events_play_boot();
    }

    perf_init();

#ifdef CONFIG_ENABLE_TUI
    tui_task_start();
#endif

    event_bus_publish_simple(EVT_BOOT, "main");

    gpio_config_t vbus_cfg = {
        .pin_bit_mask = (1ULL << USB_VBUS_EN_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&vbus_cfg));
    ESP_ERROR_CHECK(gpio_set_level(USB_VBUS_EN_GPIO, 1));
    vTaskDelay(pdMS_TO_TICKS(500));

    const gpio_config_t input_pin = {
        .pin_bit_mask = BIT64(APP_QUIT_PIN),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .intr_type    = GPIO_INTR_NEGEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&input_pin));
    ESP_ERROR_CHECK(gpio_install_isr_service(ESP_INTR_FLAG_LEVEL1));
    ESP_ERROR_CHECK(gpio_isr_handler_add(APP_QUIT_PIN, gpio_cb, NULL));

    app_event_queue = xQueueCreate(10, sizeof(app_event_queue_t));
    app_event_queue_t evt_queue;

    TaskHandle_t host_lib_hdl, class_drv_hdl;
    BaseType_t ok;
    ok = xTaskCreatePinnedToCore(usb_host_lib_task, "usb_host", 4096,
                                 xTaskGetCurrentTaskHandle(),
                                 HOST_LIB_TASK_PRIORITY, &host_lib_hdl, 0);
    assert(ok == pdTRUE);
    ulTaskNotifyTake(false, 1000);

    ok = xTaskCreatePinnedToCore(class_driver_task, "class", 5 * 1024, NULL,
                                 CLASS_TASK_PRIORITY, &class_drv_hdl, 0);
    assert(ok == pdTRUE);
    vTaskDelay(10);

    perf_start_heartbeat_task(app_current() ? app_current()->name : "main", 30000);

    while (1) {
        if (xQueueReceive(app_event_queue, &evt_queue, portMAX_DELAY)) {
            if (APP_EVENT == evt_queue.event_group) {
                usb_host_lib_info_t lib_info;
                ESP_ERROR_CHECK(usb_host_lib_info(&lib_info));
                if (lib_info.num_devices != 0)
                    ESP_LOGW(TAG, "Shutdown with attached devices.");
                break;
            }
        }
    }

    event_bus_publish_simple(EVT_SHUTDOWN, "main");
    class_driver_client_deregister();
    vTaskDelay(10);
    vTaskDelete(class_drv_hdl);
    vTaskDelete(host_lib_hdl);
    gpio_isr_handler_remove(APP_QUIT_PIN);
    xQueueReset(app_event_queue);
    vQueueDelete(app_event_queue);
}
