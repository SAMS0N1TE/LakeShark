/* usb_host.c - USB host client task. Device-specific setup is delegated
 * to rtlsdr_dev_setup_async() from the new-device callback. */

#include "usb_host.h"
#include "rtlsdr_dev.h"
#include "event_bus.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
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

usb_host_client_handle_t class_driver_client_handle(void)
{
    return s_driver_obj ? s_driver_obj->constant.client_hdl : NULL;
}

static void client_event_cb(const usb_host_client_event_msg_t *event_msg, void *arg)
{
    class_driver_t *driver_obj = (class_driver_t *)arg;
    switch (event_msg->event) {
    case USB_HOST_CLIENT_EVENT_NEW_DEV:
        xSemaphoreTake(driver_obj->constant.mux_lock, portMAX_DELAY);
        driver_obj->mux_protected.device[event_msg->new_dev.address].dev_addr =
            event_msg->new_dev.address;
        event_bus_publish_simple(EVT_DEVICE_ATTACHED, "usb");
        rtlsdr_dev_setup_async(event_msg->new_dev.address,
                               driver_obj->constant.client_hdl);
        xSemaphoreGive(driver_obj->constant.mux_lock);
        break;
    case USB_HOST_CLIENT_EVENT_DEV_GONE:
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
    default: abort();
    }
}

static void action_open_dev(usb_device_t *d)
{
    assert(d->dev_addr != 0);
    ESP_ERROR_CHECK(usb_host_device_open(d->client_hdl, d->dev_addr, &d->dev_hdl));
    d->actions |= ACTION_GET_DEV_INFO;
}
static void action_get_info(usb_device_t *d)
{
    usb_device_info_t i;
    ESP_ERROR_CHECK(usb_host_device_info(d->dev_hdl, &i));
    if (i.parent.dev_hdl) {
        usb_device_info_t p;
        ESP_ERROR_CHECK(usb_host_device_info(i.parent.dev_hdl, &p));
    }
    d->actions |= ACTION_GET_DEV_DESC;
}
static void action_get_dev_desc(usb_device_t *d)
{
    const usb_device_desc_t *dd;
    ESP_ERROR_CHECK(usb_host_get_device_descriptor(d->dev_hdl, &dd));
    d->actions |= ACTION_GET_CONFIG_DESC;
}
static void action_get_config_desc(usb_device_t *d)
{
    const usb_config_desc_t *cd;
    ESP_ERROR_CHECK(usb_host_get_active_config_descriptor(d->dev_hdl, &cd));
    d->actions |= ACTION_GET_STR_DESC;
}
static void action_get_str_desc(usb_device_t *d)
{
    usb_device_info_t i;
    ESP_ERROR_CHECK(usb_host_device_info(d->dev_hdl, &i));
}
static void action_close_dev(usb_device_t *d)
{
    ESP_ERROR_CHECK(usb_host_device_close(d->client_hdl, d->dev_hdl));
    d->dev_hdl = NULL; d->dev_addr = 0;
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

    while (1) {
        if (obj.mux_protected.flags.unhandled_devices) {
            xSemaphoreTake(obj.constant.mux_lock, portMAX_DELAY);
            for (uint8_t i = 0; i < DEV_MAX_COUNT; i++)
                if (obj.mux_protected.device[i].actions)
                    device_handle(&obj.mux_protected.device[i]);
            obj.mux_protected.flags.unhandled_devices = 0;
            xSemaphoreGive(obj.constant.mux_lock);
        } else {
            if (!obj.mux_protected.flags.shutdown)
                usb_host_client_handle_events(hdl, portMAX_DELAY);
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
