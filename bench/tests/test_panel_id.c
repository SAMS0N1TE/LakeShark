#include "ls_test.h"
#include <stdint.h>
#include <stdbool.h>

typedef int dsi_host_dev_t;
enum { MIPI_DSI_DT_SET_MAXIMUM_RETURN_PKT, MIPI_DSI_DT_DCS_READ_0 };
#define MIPI_DSI_LL_GET_HOST(n) ((dsi_host_dev_t *)0)
static int64_t now_us;
static unsigned phase, words, headers;
static bool full, busy, empty, endless;
static uint8_t reply;
static int64_t esp_timer_get_time(void) { return now_us; }
static void vTaskDelay(int ticks) { now_us += ticks * 1000; }
static bool mipi_dsi_host_ll_gen_is_cmd_fifo_full(dsi_host_dev_t *h)
{ (void)h; return full || (phase == 1 && headers == 1); }
static void mipi_dsi_host_ll_gen_set_packet_header(dsi_host_dev_t *h,
    int vc, int dt, int msb, int lsb)
{
    (void)h;
    LS_CHECK(vc == 0 && msb == 0);
    LS_CHECK((headers == 0 && dt == MIPI_DSI_DT_SET_MAXIMUM_RETURN_PKT && lsb == 1)
          || (headers == 1 && dt == MIPI_DSI_DT_DCS_READ_0 && lsb == 0xA1));
    ++headers;
}
static void mipi_dsi_host_ll_enable_video_mode(dsi_host_dev_t *h, bool on)
{ (void)h; LS_CHECK(!on); }
static void mipi_dsi_host_ll_enable_bta(dsi_host_dev_t *h, bool on)
{ (void)h; LS_CHECK(on); }
static void mipi_dsi_host_ll_gen_set_rx_vcid(dsi_host_dev_t *h, int vc)
{ (void)h; LS_CHECK(vc == 0); }
static bool mipi_dsi_host_ll_gen_is_read_cmd_busy(dsi_host_dev_t *h)
{ (void)h; return busy; }
static bool mipi_dsi_host_ll_gen_is_read_fifo_empty(dsi_host_dev_t *h)
{ (void)h; return empty || (!words && !endless); }
static uint32_t mipi_dsi_host_ll_gen_read_payload_fifo(dsi_host_dev_t *h)
{ (void)h; if (words) --words; return reply; }
#include "ls_panel_dsi_id.h"

static void reset(void)
{
    now_us = 0; phase = 0; headers = 0; words = 1; reply = 1;
    full = busy = empty = endless = false;
}
LS_CASE(missing_panel_and_stalled_fifos_do_not_hang_boot)
{
    for (int fault = 0; fault < 5; ++fault) {
        reset();
        if (fault == 0) full = true;
        if (fault == 1) phase = 1;
        if (fault == 2) busy = true;
        if (fault == 3) empty = true;
        if (fault == 4) endless = true;
        uint8_t id = 1;
        LS_CHECK(!ls_panel_read_id(&id));
        LS_CHECK(id == 0 && now_us == 250000);
    }
}
LS_CASE(valid_and_unexpected_ids_are_returned_without_fabrication)
{
    reset();
    uint8_t id;
    LS_CHECK(ls_panel_read_id(&id) && id == 1 && headers == 2);
    reset(); reply = 0xa5; words = 2;
    LS_CHECK(ls_panel_read_id(&id) && id == 0xa5 && words == 0);
}
