/* SPDX-License-Identifier: GPL-3.0-or-later
   LakeShark original. Not librtlsdr - see UPSTREAM.md in this
   directory for which files here are third-party and which are ours. */

#include "usb_host.h"
#include "usb_boot_retry.h"
#include "rtl_adapter_private.h"
#include "hackrf_adapter_private.h"
#include "event_bus.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "hal/usb_dwc_ll.h"
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


static class_driver_t *s_driver_obj;
/* Devices that reached this client. A device that fails enumeration is
   still counted by usb_host_lib_info, but never arrives here. */
static volatile int s_client_devices;
/* The root port is unpowered by us (boot recovery, a port cycle). Its
   connect status reads empty then whatever is plugged in. */
static volatile bool s_root_port_off;
static esp_err_t root_port_set_power(bool on);

static void client_event_cb(const usb_host_client_event_msg_t *event_msg, void *arg)
{
    class_driver_t *driver_obj = (class_driver_t *)arg;
    switch (event_msg->event) {
    case USB_HOST_CLIENT_EVENT_NEW_DEV:
        s_client_devices++;
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
    if (!mux) { ESP_LOGE(TAG, "mutex fail"); vTaskDelete(NULL); return; }

    usb_host_client_config_t cfg = {
        .is_synchronous    = false,
        .max_num_event_msg = CLIENT_NUM_EVENT_MSG,
        .async = { .client_event_callback = client_event_cb,
                   .callback_arg          = (void *)&obj },
    };
    /* Registering allocates from internal RAM, which boot can run short of.
       A failure costs the USB radio, not the unit: retry for a while, then
       say so and stop, instead of aborting into a crash loop. */
    esp_err_t reg = ESP_FAIL;
    for (int i = 0; i < 10; i++) {
        reg = usb_host_client_register(&cfg, &hdl);
        if (reg == ESP_OK) break;
        ESP_LOGW(TAG, "client register failed: %s - retrying", esp_err_to_name(reg));
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    if (reg != ESP_OK) {
        ESP_LOGE(TAG, "client register failed: %s - USB radios unavailable until reboot",
                 esp_err_to_name(reg));
        vSemaphoreDelete(mux);
        vTaskDelete(NULL);
        return;
    }

    obj.constant.mux_lock   = mux;
    obj.constant.client_hdl = hdl;
    for (uint8_t i = 0; i < DEV_MAX_COUNT; i++)
        obj.mux_protected.device[i].client_hdl = hdl;
    s_driver_obj = &obj;
    usb_boot_retry_t boot_retry={.next_us=esp_timer_get_time()+10000000};

    while (1) {
        if(!boot_retry.done) {
            const int64_t now=esp_timer_get_time();
            /* The port is only read once the retry is due to act on it. */
            const int attached=now>=boot_retry.next_us && !boot_retry.off
                ? usb_host_root_port_attached() : USB_PORT_ATTACH_UNKNOWN;
            usb_boot_retry_tick(&boot_retry,now,s_client_devices,attached,
                                root_port_set_power);
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
    const esp_err_t err = usb_host_lib_set_root_port_power(on);
    if (err == ESP_OK) s_root_port_off = !on;
    return err;
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

/* Whether anything is plugged into the root port, enumerated or not. The
   host library cannot say: a device that fails before it is registered
   (a failed port reset) is not in usb_host_lib_info, and one that fails
   later still is. The controller's own port status (HPRT.PRTCONNSTS, the
   bit the IDF HCD raises its connection event from) is set by the plug
   alone. Read only: no write-1-to-clear bit is touched. HS controller,
   because the IDF host runs on port 0 and the lib is installed with the
   default peripheral map. UNKNOWN until the client exists (the host is
   installed) and while we hold the port unpowered, when it reads empty
   whatever is plugged in. */
int usb_host_root_port_attached(void)
{
    if (!s_driver_obj || s_root_port_off) return USB_PORT_ATTACH_UNKNOWN;
    if (root_port_device_count() > 0) return USB_PORT_ATTACH_PRESENT;
    return usb_dwc_ll_hprt_get_conn_status(USB_DWC_LL_GET_HW(0))
        ? USB_PORT_ATTACH_PRESENT : USB_PORT_ATTACH_NONE;
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
