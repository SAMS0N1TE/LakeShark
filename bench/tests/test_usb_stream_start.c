/* LS_TEST_SOURCES: ${FW}/components/lakeshark/radio/esp_libusb.c */
#include "ls_test.h"

#include "esp_libusb_private.h"
#include "esp_heap_caps.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include <stdlib.h>

#define TEST_STREAM_SLOTS 15u

static unsigned s_allocs;
static unsigned s_frees;
static unsigned s_submits;
static bool s_fail_transfer_alloc;
static bool s_hold_completions;
static usb_transfer_t *s_inflight[32];
static void complete_usb(void)
{
    for(unsigned i=0;i<32;i++)if(s_inflight[i]) {
        usb_transfer_t *t=s_inflight[i];s_inflight[i]=NULL;
        t->status=USB_TRANSFER_STATUS_CANCELED;t->actual_num_bytes=0;t->callback(t);
    }
}

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
        for(unsigned i=0;i<32;i++)LS_CHECK(s_inflight[i]!=transfer);
        free(transfer->data_buffer);
        free(transfer);
        ++s_frees;
    }
    return ESP_OK;
}

esp_err_t usb_host_transfer_submit(usb_transfer_t *transfer)
{
    for(unsigned i=0;i<32;i++)if(!s_inflight[i]) {s_inflight[i]=transfer;break;}
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
    if(!s_hold_completions)complete_usb();
    return ESP_OK;
}

esp_err_t usb_host_endpoint_clear(usb_device_handle_t device, uint8_t endpoint)
{
    (void)device; (void)endpoint;
    return ESP_OK;
}

LS_CASE(usb_buffers_wait_for_delayed_flush_callbacks_before_reuse)
{
    ls_shim_task_fail_create(pdFALSE);s_fail_transfer_alloc=false;s_hold_completions=false;
    class_driver_t driver={.dev_hdl=(usb_device_handle_t)0x2468};
    LS_EQ_INT(esp_libusb_stream_start(&driver,0x81),0);
    unsigned allocated=s_allocs,freed=s_frees,submitted=s_submits;
    s_hold_completions=true;ls_shim_task_set_state(eSuspended);
    esp_libusb_stream_stop();
    LS_EQ_UINT(s_frees,freed);LS_EQ_INT(esp_libusb_stream_slots(),TEST_STREAM_SLOTS);
    LS_EQ_INT(esp_libusb_stream_start(&driver,0x81),ESP_LIBUSB_ERR_BUSY);
    LS_EQ_UINT(s_submits,submitted);LS_EQ_UINT(s_allocs,allocated);
    complete_usb();s_hold_completions=false;
    LS_EQ_INT(esp_libusb_stream_start(&driver,0x81),0);
    LS_EQ_UINT(s_frees,freed+TEST_STREAM_SLOTS);LS_EQ_INT(esp_libusb_stream_slots(),TEST_STREAM_SLOTS);
    ls_shim_task_set_state(eSuspended);esp_libusb_stream_stop();
    LS_EQ_UINT(s_frees,freed+2*TEST_STREAM_SLOTS);
}

void rtl_adapter_note_transport_fault(void) {}

LS_CASE(completed_in_transfers_repost_without_scheduling_the_recovery_pump)
{
    ls_shim_task_fail_create(pdFALSE);s_fail_transfer_alloc=false;s_hold_completions=false;
    class_driver_t driver={.dev_hdl=(usb_device_handle_t)0x1973};
    LS_EQ_INT(esp_libusb_stream_start(&driver,0x81),0);
    unsigned submitted=s_submits;
    /* The host shim never executes the pump. Each completion must preserve
     * the posted window, even across repeated complete/refill cycles. */
    for(unsigned n=0;n<64;n++) {
        usb_transfer_t *t=NULL;
        for(unsigned i=0;i<32;i++)if(s_inflight[i]) {t=s_inflight[i];s_inflight[i]=NULL;break;}
        LS_CHECK(t!=NULL);if(!t)break;
        memset(t->data_buffer,(int)n,16);t->actual_num_bytes=16;
        t->status=USB_TRANSFER_STATUS_COMPLETED;t->callback(t);
        LS_EQ_UINT(s_submits,submitted+n+1);
        unsigned active=0;for(unsigned i=0;i<32;i++)if(s_inflight[i])active++;
        LS_EQ_UINT(active,TEST_STREAM_SLOTS);
        uint8_t received[16];LS_EQ_INT(esp_libusb_stream_read(received,16),16);
        for(unsigned i=0;i<16;i++)LS_EQ_UINT(received[i],n);
    }
    LS_EQ_UINT(esp_libusb_stream_dropped(),0);
    ls_shim_task_set_state(eSuspended);esp_libusb_stream_stop();
}

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
    LS_EQ_INT(esp_libusb_stream_slots(), TEST_STREAM_SLOTS);
    LS_EQ_UINT(s_allocs, TEST_STREAM_SLOTS);
    LS_EQ_UINT(s_submits, TEST_STREAM_SLOTS);

    ls_shim_task_set_state(eSuspended);
    esp_libusb_stream_stop();
    LS_CHECK(!esp_libusb_streaming());
    LS_EQ_INT(esp_libusb_stream_slots(), 0);
    LS_EQ_UINT(s_frees, TEST_STREAM_SLOTS);
    LS_EQ_UINT(ls_shim_task_delete_count(), 2);

    LS_EQ_INT(esp_libusb_stream_start(&driver, 0x81), 0);
    LS_CHECK(esp_libusb_streaming());
    LS_EQ_INT(esp_libusb_stream_slots(), TEST_STREAM_SLOTS);
    LS_EQ_UINT(s_allocs, 2 * TEST_STREAM_SLOTS);
    LS_EQ_UINT(s_submits, 2 * TEST_STREAM_SLOTS);
    LS_EQ_UINT(ls_shim_queue_static_create_count(), 1);
    LS_EQ_UINT(ls_shim_task_static_create_count(), 4);

    ls_shim_task_set_state(eSuspended);
    esp_libusb_stream_stop();
    LS_EQ_UINT(s_frees, 2 * TEST_STREAM_SLOTS);
    LS_EQ_UINT(ls_shim_task_delete_count(), 3);
}

/* A control transfer whose wait gave up is still owned by the host, and
   closing the device under it trips usbh.c's num_ctrl_xfers_inflight
   assert. The shim must report it pending until the host hands it back,
   and must not touch or resubmit it in the meantime. */
LS_CASE(a_timed_out_control_transfer_stays_pending_until_the_host_returns_it)
{
    init_adsb_dev();
    class_driver_t drv = {0};
    drv.dev_hdl = (usb_device_handle_t)(uintptr_t)0x1234;
    unsigned char buf[2] = {0};

    LS_CHECK(!esp_libusb_ctrl_pending(drv.dev_hdl));
    const unsigned before = s_submits;
    LS_EQ_INT(-1, esp_libusb_control_transfer(&drv, CTRL_IN, 0, 0, 0, buf, 1, 0));
    LS_EQ_UINT(before + 1, s_submits);
    LS_CHECK(esp_libusb_ctrl_pending(drv.dev_hdl));
    LS_CHECK(!esp_libusb_ctrl_pending((usb_device_handle_t)(uintptr_t)0x5678));

    /* Refused without resubmitting the transfer the host still holds. */
    LS_EQ_INT(-1, esp_libusb_control_transfer(&drv, CTRL_IN, 0, 0, 0, buf, 1, 0));
    LS_EQ_UINT(before + 1, s_submits);

    /* The lock is free again once the timed-out call has returned. */
    LS_CHECK(esp_libusb_ctrl_lock(10));
    esp_libusb_ctrl_unlock();

    /* The host retires it - as it does when the port drops - and it clears. */
    complete_usb();
    LS_CHECK(!esp_libusb_ctrl_pending(drv.dev_hdl));
}
