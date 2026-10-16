/* LakeShark LR: answers for the dongle, SX1262 and dongle-only app code the
   LR edition leaves out. Each one reports "not present" or does nothing. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "cell_performance.h"
#include "cell_iq.h"
#include "cell_report.h"
#include "rec_catch.h"
#include "rtlsdr_dev.h"
#include "usb_boot_retry.h"
#include "usb_port_cycle.h"
#include "rtl-sdr.h"
#include "ls_lora.h"

static int not_here(const char *what)
{
    printf("%s: not in LakeShark LR (needs %s)\n", what,
           what[0] == 'h' ? "a HackRF" : "an RTL-SDR dongle");
    return 1;
}

/* CELL WATCH needs an RTL-SDR. */
void cell_performance_boot(void) {}
bool cell_performance_active(void) { return false; }
void cell_performance_reboot(bool enable) { (void)enable; }
int cell_performance_command(int argc, char **argv) { (void)argc; (void)argv; return not_here("cell"); }
int cell_iq_command(int argc, char **argv) { (void)argc; (void)argv; return not_here("cell"); }
int cell_report_command(int argc, char **argv) { (void)argc; (void)argv; return not_here("cell"); }
void ls_cell_publish(void) {}

/* CATCH spots bursts with an RTL-SDR. */
int rec_catch_command(int argc, char **argv) { (void)argc; (void)argv; return not_here("catch"); }

/* lora sxprobe: the SX1262 driver is not in this build */
int ls_lora_sx126x_probe_command(int argc, char **argv) { (void)argc; (void)argv; return not_here("sxprobe"); }

/* HackRF. */
void ls_hackrf_print_status(void) { (void)not_here("hackrf"); }
void ls_hackrf_dump(void) { (void)not_here("hackrf"); }
void ls_hackrf_set_ppm(int ppm) { (void)ppm; (void)not_here("hackrf"); }
void ls_hackrf_snap(void) { (void)not_here("hackrf"); }

/* RTL-SDR and the USB host port. */
int rtl_adapter_port_attached(void) { return 0; }
bool rtl_adapter_port_reset_possible(void) { return false; }
usb_port_cycle_result_t rtl_adapter_port_reset(void) { return USB_PORT_CYCLE_NOTHING; }
void rtlsdr_dev_teardown(void) {}
uint32_t rtlsdr_stream_avail(void) { return 0; }
uint64_t rtlsdr_stream_dropped(void) { return 0; }
int rtlsdr_stream_slots(void) { return 0; }
bool usb_absent_notice_wanted(int attached) { (void)attached; return false; }
const char *usb_port_cycle_result_name(usb_port_cycle_result_t result)
{
    (void)result;
    return "no USB host in LakeShark LR";
}
