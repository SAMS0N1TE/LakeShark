/* SPDX-License-Identifier: GPL-3.0-or-later
   LakeShark original. Not librtlsdr - see UPSTREAM.md in this
   directory for which files here are third-party and which are ours. */

#include "usb_host.h"
#include "rtl_adapter_private.h"
#include "hackrf_adapter_private.h"
#include "event_bus.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>

static const char *TAG = "usb_host";

#define CLIENT_NUM_EVENT_MSG 5
#define DEV_MAX_COUNT        128

typedef enum {
    ACTION_OPEN_DEV        = (1 << 0),
    ACTION_GET_DEV_INFO    = (1 << 1),
    ACTION_GET_DEV_DESC    = (1 << 2),
    ACTION_GET_CONFIG_DESC = (1 << 3),
    ACTION_GET_STR_DESC    = (1 << 4),
    ACTION_CLOSE_DEV       = (1 << 5),
    ACTION_PROBE_DEV       = (1 << 6),
} action_t;

typedef struct {
    usb_host_client_handle_t client_hdl;
    uint8_t                  dev_addr;
    usb_device_handle_t      dev_hdl;
    action_t                 actions;
} usb_device_t;

typedef struct {
    struct {
        union {
            struct {
                uint8_t unhandled_devices : 1;
                uint8_t shutdown          : 1;
                uint8_t reserved6         : 6;
            };
            uint8_t val;
        } flags;
        usb_device_t device[DEV_MAX_COUNT];
    } mux_protected;
    struct {
        usb_host_client_handle_t client_hdl;
        SemaphoreHandle_t        mux_lock;
    } constant;
} class_driver_t;

/* USB_BOOT_RETRY_BEGIN: also compiled by the consolidated host tests. */
typedef struct {
    int64_t next_us;
    unsigned attempts;
    bool off, done;
} usb_boot_retry_t;

static void usb_boot_retry_tick(usb_boot_retry_t *retry, int64_t now, int devices)
{
    if(retry->done) return;
    /* A successful attach permanently ends boot recovery. Never disturb a
       working receiver, hub or any other registered USB peripheral.

       SAY SO WHEN IT HAPPENS. A ten-reset series recovered nine times;
       the tenth failed CHECK_SHORT_DEV_DESC at 4541 ms - the exact
       failure this incident records - and then no retry ever ran. The
       suspect is this line: a device that FAILED enumeration may still
       be counted by usb_host_lib_info, in which case devices > 0 is
       true, recovery concludes something attached and disables itself
       permanently, in precisely the case it exists for.

       That is a suspicion, not a finding, and changing the guard on a
       suspicion risks power-cycling a port with a working dongle on it.
       So this logs the inputs to the decision instead. The next
       occurrence says outright whether the count was nonzero, and the
       fix after that is evidence rather than a guess. */
    if(devices > 0 && !retry->off) {
        ESP_LOGW(TAG,"USB boot recovery stood down: %d device(s) "
                     "enumerated at %lld ms",devices,
                     (long long)(now/1000));
        retry->done=true;
        return;
    }
    if(devices < 0 || now < retry->next_us) return;
    if(retry->off) {
        esp_err_t err=usb_host_lib_set_root_port_power(true);
        retry->off=false;
        retry->next_us=now+5000000;
        if(err != ESP_OK) retry->done=true;
        ESP_LOGW(TAG,"USB boot retry %u: host port on: %s",retry->attempts,esp_err_to_name(err));
        return;
    }
    if(retry->attempts>=3) { retry->done=true; return; }
    retry->attempts++;
    esp_err_t err=usb_host_lib_set_root_port_power(false);
    retry->off=err==ESP_OK;
    retry->next_us=now+(retry->off?300000:5000000);
    ESP_LOGW(TAG,"USB boot retry %u: no enumerated devices, host port off: %s",retry->attempts,esp_err_to_name(err));
}
/* USB_BOOT_RETRY_END */

static class_driver_t *s_driver_obj;

static void client_event_cb(const usb_host_client_event_msg_t *event_msg, void *arg)
{
    class_driver_t *driver_obj = (class_driver_t *)arg;
    switch (event_msg->event) {
    case USB_HOST_CLIENT_EVENT_NEW_DEV:
        xSemaphoreTake(driver_obj->constant.mux_lock, portMAX_DELAY);
        driver_obj->mux_protected.device[event_msg->new_dev.address].dev_addr =
            event_msg->new_dev.address;
        event_bus_publish_simple(EVT_DEVICE_ATTACHED, "usb");
        driver_obj->mux_protected.device[event_msg->new_dev.address].actions =
            ACTION_PROBE_DEV;
        driver_obj->mux_protected.flags.unhandled_devices = 1;
        xSemaphoreGive(driver_obj->constant.mux_lock);
        break;
    case USB_HOST_CLIENT_EVENT_DEV_GONE:
        /**/
        (void)rtl_adapter_note_removed(event_msg->dev_gone.dev_hdl);
        (void)hackrf_adapter_note_removed(event_msg->dev_gone.dev_hdl);
        xSemaphoreTake(driver_obj->constant.mux_lock, portMAX_DELAY);
        for (uint8_t i = 0; i < DEV_MAX_COUNT; i++) {
            if (driver_obj->mux_protected.device[i].dev_hdl ==
                event_msg->dev_gone.dev_hdl) {
                driver_obj->mux_protected.device[i].actions = ACTION_CLOSE_DEV;
                driver_obj->mux_protected.flags.unhandled_devices = 1;
            }
        }
        event_bus_publish_simple(EVT_DEVICE_DETACHED, "usb");
        xSemaphoreGive(driver_obj->constant.mux_lock);
        break;
    /**/
    default:
        ESP_LOGW(TAG, "unhandled USB client event %d", (int)event_msg->event);
        break;
    }
}

/**/
static void action_open_dev(usb_device_t *d)
{
    if (d->dev_addr == 0) return;
    esp_err_t e = usb_host_device_open(d->client_hdl, d->dev_addr, &d->dev_hdl);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "device_open(addr=%u) failed: %s - dropping it",
                 d->dev_addr, esp_err_to_name(e));
        d->dev_hdl = NULL; d->dev_addr = 0;
        return;
    }
    d->actions |= ACTION_GET_DEV_INFO;
}
static void action_get_info(usb_device_t *d)
{
    usb_device_info_t i;
    if (!d->dev_hdl || usb_host_device_info(d->dev_hdl, &i) != ESP_OK) return;
    const char *sp = (i.speed == USB_SPEED_HIGH) ? "HIGH(480M)"
                   : (i.speed == USB_SPEED_FULL) ? "FULL(12M)"
                   : (i.speed == USB_SPEED_LOW)  ? "LOW(1.5M)" : "?";
    ESP_LOGW(TAG, "*** USB negotiated speed: %s (enum=%d) ***", sp, (int)i.speed);
    d->actions |= ACTION_GET_DEV_DESC;
}
static void action_get_dev_desc(usb_device_t *d)
{
    const usb_device_desc_t *dd;
    if (!d->dev_hdl || usb_host_get_device_descriptor(d->dev_hdl, &dd) != ESP_OK) return;
    d->actions |= ACTION_GET_CONFIG_DESC;
}
static void action_get_config_desc(usb_device_t *d)
{
    const usb_config_desc_t *cd;
    if (!d->dev_hdl ||
        usb_host_get_active_config_descriptor(d->dev_hdl, &cd) != ESP_OK) return;
    d->actions |= ACTION_GET_STR_DESC;
}
static void action_get_str_desc(usb_device_t *d)
{
    usb_device_info_t i;
    if (d->dev_hdl) usb_host_device_info(d->dev_hdl, &i);
}
static void action_close_dev(usb_device_t *d)
{
    if (d->dev_hdl) {
        esp_err_t e = usb_host_device_close(d->client_hdl, d->dev_hdl);
        if (e != ESP_OK) {
            ESP_LOGW(TAG, "device_close failed: %s", esp_err_to_name(e));
        }
    }
    d->dev_hdl = NULL; d->dev_addr = 0;
}

static void action_probe_dev(usb_device_t *d)
{
    usb_device_handle_t device = NULL;
    esp_err_t error = usb_host_device_open(d->client_hdl, d->dev_addr, &device);
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "probe open(addr=%u): %s", d->dev_addr, esp_err_to_name(error));
        return;
    }
    const usb_device_desc_t *descriptor = NULL;
    error = usb_host_get_device_descriptor(device, &descriptor);
    bool valid = error == ESP_OK && descriptor;
    bool hackrf = valid && hackrf_adapter_matches(descriptor->idVendor,
                                                   descriptor->idProduct);
    /* Both adapters share this USB client. Release the descriptor probe before
     * starting exactly one adapter; overlapping opens return INVALID_STATE. */
    error = usb_host_device_close(d->client_hdl, device);
    if (error != ESP_OK || !valid) {
        ESP_LOGW(TAG, "USB descriptor probe could not hand off addr=%u", d->dev_addr);
        return;
    }
    if (hackrf) hackrf_adapter_probe_async(d->dev_addr, d->client_hdl);
    else rtl_adapter_probe_async(d->dev_addr, d->client_hdl);
}

static void device_handle(usb_device_t *d)
{
    uint8_t actions = d->actions;
    d->actions = 0;
    while (actions) {
        if (actions & ACTION_OPEN_DEV)        action_open_dev(d);
        if (actions & ACTION_GET_DEV_INFO)    action_get_info(d);
        if (actions & ACTION_GET_DEV_DESC)    action_get_dev_desc(d);
        if (actions & ACTION_GET_CONFIG_DESC) action_get_config_desc(d);
        if (actions & ACTION_GET_STR_DESC)    action_get_str_desc(d);
        if (actions & ACTION_CLOSE_DEV)       action_close_dev(d);
        if (actions & ACTION_PROBE_DEV)       action_probe_dev(d);
        actions = d->actions; d->actions = 0;
    }
}

void class_driver_task(void *arg)
{
    class_driver_t           obj = {0};
    usb_host_client_handle_t hdl = NULL;

    SemaphoreHandle_t mux = xSemaphoreCreateMutex();
    if (!mux) { ESP_LOGE(TAG, "mutex fail"); vTaskSuspend(NULL); return; }

    usb_host_client_config_t cfg = {
        .is_synchronous    = false,
        .max_num_event_msg = CLIENT_NUM_EVENT_MSG,
        .async = { .client_event_callback = client_event_cb,
                   .callback_arg          = (void *)&obj },
    };
    ESP_ERROR_CHECK(usb_host_client_register(&cfg, &hdl));

    obj.constant.mux_lock   = mux;
    obj.constant.client_hdl = hdl;
    for (uint8_t i = 0; i < DEV_MAX_COUNT; i++)
        obj.mux_protected.device[i].client_hdl = hdl;
    s_driver_obj = &obj;
    usb_boot_retry_t boot_retry={.next_us=esp_timer_get_time()+10000000};

    while (1) {
        if(!boot_retry.done) {
            usb_host_lib_info_t info;
            int devices=usb_host_lib_info(&info)==ESP_OK?(int)info.num_devices:-1;
            usb_boot_retry_tick(&boot_retry,esp_timer_get_time(),devices);
        }
        if (obj.mux_protected.flags.unhandled_devices) {
            xSemaphoreTake(obj.constant.mux_lock, portMAX_DELAY);
            for (uint8_t i = 0; i < DEV_MAX_COUNT; i++)
                if (obj.mux_protected.device[i].actions)
                    device_handle(&obj.mux_protected.device[i]);
            obj.mux_protected.flags.unhandled_devices = 0;
            xSemaphoreGive(obj.constant.mux_lock);
        } else {
            if (!obj.mux_protected.flags.shutdown)
                usb_host_client_handle_events(hdl, boot_retry.done ? portMAX_DELAY : pdMS_TO_TICKS(250));
            else break;
        }
    }

    ESP_ERROR_CHECK(usb_host_client_deregister(hdl));
    if (mux) vSemaphoreDelete(mux);
    vTaskSuspend(NULL);
}

void class_driver_client_deregister(void)
{
    if (!s_driver_obj) return;
    xSemaphoreTake(s_driver_obj->constant.mux_lock, portMAX_DELAY);
    for (uint8_t i = 0; i < DEV_MAX_COUNT; i++) {
        if (s_driver_obj->mux_protected.device[i].dev_hdl != NULL) {
            s_driver_obj->mux_protected.device[i].actions |= ACTION_CLOSE_DEV;
            s_driver_obj->mux_protected.flags.unhandled_devices = 1;
        }
    }
    s_driver_obj->mux_protected.flags.shutdown = 1;
    xSemaphoreGive(s_driver_obj->constant.mux_lock);
    ESP_ERROR_CHECK(usb_host_client_unblock(s_driver_obj->constant.client_hdl));
}

static esp_err_t root_port_set_power(bool on)
{
    return usb_host_lib_set_root_port_power(on);
}

static int root_port_device_count(void)
{
    usb_host_lib_info_t info;
    if (usb_host_lib_info(&info) != ESP_OK) return -1;
    return info.num_devices;
}

static void root_port_delay_ms(uint32_t ms)
{
    TickType_t ticks = pdMS_TO_TICKS(ms);
    vTaskDelay(ticks ? ticks : 1);
}

bool usb_host_root_port_has_device(void)
{
    return root_port_device_count() > 0;
}

/**/
usb_port_cycle_result_t usb_host_root_port_cycle(void)
{
    static const usb_port_cycle_ops_t ops = {
        .set_power = root_port_set_power,
        .device_count = root_port_device_count,
        .delay_ms = root_port_delay_ms,
    };
    return usb_port_cycle_run(&ops);
}
