#include "ls_test.h"
#include "../../managed_components/espressif__esp_hosted/host/drivers/transport/sdio/sdio_rx_watchdog.h"

LS_CASE(stalled_rx_reports_once_and_does_not_flood_the_console) {
    sdio_rx_watchdog_t w = {0};
    int logs = 0, stalls = 0;
    for (int i = 0; i < 214 * 60; i++) {
        int notice = sdio_rx_watchdog_poll(&w, true, 0, 1000000 + i * 1000000LL / 214);
        logs += notice != SDIO_RX_QUIET;
        stalls += notice == SDIO_RX_STALLED;
    }
    LS_EQ_INT(stalls, 1);
    LS_CHECK(logs <= 6 && w.reported && w.dropped == 214 * 60);
    LS_CHECK(sdio_rx_watchdog_poll(&w, false, 1, 62000000) == SDIO_RX_QUIET);
    LS_CHECK(!w.active && !w.reported);
    sdio_rx_watchdog_poll(&w, true, 1, 63000000);
    LS_EQ_INT(sdio_rx_watchdog_poll(&w, true, 1, 68000000), SDIO_RX_STALLED);
}
LS_CASE(busy_rx_with_progress_is_not_a_stall_including_counter_wrap) {
    sdio_rx_watchdog_t w = {0};
    for (int i = 0; i < 100; i++) {
        LS_CHECK(sdio_rx_watchdog_poll(&w, true, UINT32_MAX - 20u + i,
                                     1000000 + i * 100000LL) != SDIO_RX_STALLED);
        LS_CHECK(!w.reported);
    }
}
