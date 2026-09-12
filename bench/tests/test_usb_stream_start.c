/* LS_TEST_SOURCES: ${FW}/components/lakeshark/radio/esp_libusb.c */
#include "ls_test.h"

#include "esp_libusb_private.h"
#include "esp_heap_caps.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include <stdlib.h>

static unsigned s_allocs;
static unsigned s_frees;
static unsigned s_submits;
static bool s_fail_transfer_alloc;

esp_err_t usb_host_transfer_alloc(size_t size, int flags,
                                  usb_transfer_t **out)
{
    (void)flags;
    if (s_fail_transfer_alloc) {
        /* Let the host model represent the pump reaching its terminal state
         * after stream_start clears s_streaming on this failure path. */
        ls_shim_task_set_state(eSuspended);
        return ESP_ERR_NO_MEM;
    }
    usb_transfer_t *transfer = calloc(1, sizeof(*transfer));
    if (!transfer) return ESP_ERR_NO_MEM;
    transfer->data_buffer = malloc(size);
    if (!transfer->data_buffer) {
        free(transfer);
        return ESP_ERR_NO_MEM;
    }
    *out = transfer;
    ++s_allocs;
    return ESP_OK;
}

esp_err_t usb_host_transfer_free(usb_transfer_t *transfer)
{
    if (transfer) {
        free(transfer->data_buffer);
        free(transfer);
        ++s_frees;
    }
    return ESP_OK;
}

esp_err_t usb_host_transfer_submit(usb_transfer_t *transfer)
{
    (void)transfer;
    ++s_submits;
    return ESP_OK;
}

esp_err_t usb_host_transfer_submit_control(usb_host_client_handle_t client,
                                           usb_transfer_t *transfer)
{
    (void)client;
    return usb_host_transfer_submit(transfer);
}

esp_err_t usb_host_endpoint_halt(usb_device_handle_t device, uint8_t endpoint)
{
    (void)device; (void)endpoint;
    return ESP_OK;
}

esp_err_t usb_host_endpoint_flush(usb_device_handle_t device, uint8_t endpoint)
{
    (void)device; (void)endpoint;
    return ESP_OK;
}

esp_err_t usb_host_endpoint_clear(usb_device_handle_t device, uint8_t endpoint)
{
    (void)device; (void)endpoint;
    return ESP_OK;
}

void rtl_adapter_note_transport_fault(void) {}

LS_CASE(cold_start_resources_survive_failure_and_reentry)
{
    ls_shim_heap_reset();
    ls_shim_task_reset();
    ls_shim_queue_reset();
    /* Model the fragmented cold-entry heap.  Static transport resources must
     * remain available when ordinary queue creation would fail. */
    ls_shim_queue_fail_create(pdTRUE);
    ls_shim_task_fail_create(pdTRUE);
    s_allocs = s_frees = s_submits = 0;
    s_fail_transfer_alloc = false;

    class_driver_t driver = {.dev_hdl = (usb_device_handle_t)0x1234};
    LS_CHECK(esp_libusb_stream_start(&driver, 0x81) != 0);

    LS_EQ_STR(ls_shim_task_last_name(), "rtl_pump");
    LS_EQ_UINT(ls_shim_task_last_stack_depth(), 4096);
    LS_EQ_UINT(ls_shim_task_last_priority(), 12);
    LS_EQ_INT(ls_shim_task_last_core_id(), 1);
    LS_EQ_UINT(s_allocs, 0);
    LS_EQ_UINT(s_frees, 0);
    LS_EQ_UINT(s_submits, 0);
    LS_EQ_INT(esp_libusb_stream_slots(), 0);
    LS_CHECK(!esp_libusb_streaming());
    LS_EQ_UINT(ls_shim_queue_static_create_count(), 1);
    LS_EQ_UINT(ls_shim_task_static_create_count(), 1);
    LS_CHECK(ls_shim_task_last_static_stack() != NULL);

    ls_shim_task_fail_create(pdFALSE);
    s_fail_transfer_alloc = true;
    LS_EQ_INT(esp_libusb_stream_start(&driver, 0x81),
              ESP_LIBUSB_ERR_NO_MEM);
    LS_CHECK(!esp_libusb_streaming());
    LS_EQ_INT(esp_libusb_stream_slots(), 0);
    LS_EQ_UINT(s_allocs, 0);
    LS_EQ_UINT(s_submits, 0);
    LS_EQ_UINT(ls_shim_task_delete_count(), 1);

    s_fail_transfer_alloc = false;
    LS_EQ_INT(esp_libusb_stream_start(&driver, 0x81), 0);
    LS_CHECK(esp_libusb_streaming());
    LS_EQ_INT(esp_libusb_stream_slots(), 16);
    LS_EQ_UINT(s_allocs, 16);
    LS_EQ_UINT(s_submits, 16);

    ls_shim_task_set_state(eSuspended);
    esp_libusb_stream_stop();
    LS_CHECK(!esp_libusb_streaming());
    LS_EQ_INT(esp_libusb_stream_slots(), 0);
    LS_EQ_UINT(s_frees, 16);
    LS_EQ_UINT(ls_shim_task_delete_count(), 2);

    LS_EQ_INT(esp_libusb_stream_start(&driver, 0x81), 0);
    LS_CHECK(esp_libusb_streaming());
    LS_EQ_INT(esp_libusb_stream_slots(), 16);
    LS_EQ_UINT(s_allocs, 32);
    LS_EQ_UINT(s_submits, 32);
    LS_EQ_UINT(ls_shim_queue_static_create_count(), 1);
    LS_EQ_UINT(ls_shim_task_static_create_count(), 4);

    ls_shim_task_set_state(eSuspended);
    esp_libusb_stream_stop();
    LS_EQ_UINT(s_frees, 32);
    LS_EQ_UINT(ls_shim_task_delete_count(), 3);
}
