/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "cell_performance.h"
#include "cell_monitor.h"
#include "cell_iq.h"
#include "esp_attr.h"
#include "esp_system.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define PERF_TICKET UINT32_C(0x48495246)
/* One explicitly requested software restart. Consume before startup so a
 * crash, ordinary reset or power cycle cannot trap the device in this mode. */
static RTC_NOINIT_ATTR uint32_t ticket, ticket_inverse;
static bool active;
void cell_performance_boot(void)
{
    active=esp_reset_reason()==ESP_RST_SW && ticket==PERF_TICKET && ticket_inverse==~PERF_TICKET;
    ticket=ticket_inverse=0;
}
bool cell_performance_active(void) {return active;}
void cell_performance_reboot(bool enable)
{
    cell_iq_status_t capture;cell_iq_get_status(&capture);
    if(capture.busy){puts("CELL PERFORMANCE: wait for the current capture to finish");return;}
    cell_monitor_wait_stopped();
    ticket=enable?PERF_TICKET:0;
    ticket_inverse=enable?~PERF_TICKET:0;
    printf("CELL PERFORMANCE: restarting into %s\n",enable?"HackRF high-rate session":"normal OS");
    fflush(stdout);
    esp_restart();
}
int cell_performance_command(int argc,char **argv)
{
    if(argc==2 && !strcmp(argv[1],"on"))cell_performance_reboot(true);
    else if(argc==2 && !strcmp(argv[1],"off"))cell_performance_reboot(false);
    else printf("CELL PERFORMANCE: %s; cellperf on|off restarts. GPS, IMU, LoRa and display retained; C6/BLE and audio paused.\n",active?"HIGH RATE":"NORMAL");
    return 0;
}
