/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

/* Boot-only RM69A10 ID read. The caller owns DSI bus 0 and includes the IDF
 * DSI LL, timer and FreeRTOS headers. IDF 5.4's generic read spins forever
 * when the panel never supplies data; every wait here shares a 250 ms bound.
 * Keep this separate so the no-response paths can run against fake registers.
 */
static bool ls_panel_id_wait(int64_t deadline)
{
    if (esp_timer_get_time() >= deadline) return false;
    vTaskDelay(1);
    return true;
}

static bool ls_panel_read_id(uint8_t *id)
{
    const int64_t deadline = esp_timer_get_time() + 250000;
    dsi_host_dev_t *host = MIPI_DSI_LL_GET_HOST(0);
    *id = 0;
    while (mipi_dsi_host_ll_gen_is_cmd_fifo_full(host))
        if (!ls_panel_id_wait(deadline)) return false;
    mipi_dsi_host_ll_gen_set_packet_header(host, 0,
        MIPI_DSI_DT_SET_MAXIMUM_RETURN_PKT, 0, 1);
    mipi_dsi_host_ll_enable_video_mode(host, false);
    mipi_dsi_host_ll_enable_bta(host, true);
    mipi_dsi_host_ll_gen_set_rx_vcid(host, 0);
    while (mipi_dsi_host_ll_gen_is_cmd_fifo_full(host))
        if (!ls_panel_id_wait(deadline)) return false;
    mipi_dsi_host_ll_gen_set_packet_header(host, 0, MIPI_DSI_DT_DCS_READ_0, 0, 0xA1);
    while (mipi_dsi_host_ll_gen_is_read_cmd_busy(host))
        if (!ls_panel_id_wait(deadline)) return false;
    while (mipi_dsi_host_ll_gen_is_read_fifo_empty(host))
        if (!ls_panel_id_wait(deadline)) return false;
    const uint8_t response = mipi_dsi_host_ll_gen_read_payload_fifo(host) & 0xff;
    while (!mipi_dsi_host_ll_gen_is_read_fifo_empty(host)) {
        (void)mipi_dsi_host_ll_gen_read_payload_fifo(host);
        if (!ls_panel_id_wait(deadline)) return false;
    }
    *id = response;
    return true;
}
