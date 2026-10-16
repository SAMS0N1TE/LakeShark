/* LS_TEST_SOURCES: ${FW}/components/lakeshark/radio/usb_boot_retry.c */
#include "ls_test.h"
#include "usb_boot_retry.h"

#include <string.h>

/* Boot recovery against a scripted root port. 'F' is a power-off, 'N' a
   power-on, so a case asserts the sequence itself. The rule under test: a
   dongle that is plugged in and failed to enumerate is still recovered
   (USB-BOOT-ENUM), and a port with nothing in it is left alone. */
static char s_log[32];
static size_t s_n;
static esp_err_t s_fail;

static esp_err_t fake_power(bool on)
{
    if (s_n + 1 < sizeof(s_log)) {
        s_log[s_n++] = on ? 'N' : 'F';
        s_log[s_n] = '\0';
    }
    return s_fail;
}

static void fresh(void)
{
    memset(s_log, 0, sizeof(s_log));
    s_n = 0;
    s_fail = ESP_OK;
}

/* Boot at 0; the first look is at 10 s, as usb_host.c arms it. */
static usb_boot_retry_t armed(void)
{
    return (usb_boot_retry_t){ .next_us = 10000000 };
}

/* Step the machine through ten minutes in 250 ms, the way the class
   driver loop does, with the port reading `attached` throughout. */
static void run_to_the_end(usb_boot_retry_t *r, int attached, int devices)
{
    for (int64_t t = 0; t < 600000000; t += 250000)
        usb_boot_retry_tick(r, t, devices, attached, fake_power);
}

LS_CASE(nothing_attached_never_cycles_the_port)
{
    fresh();
    usb_boot_retry_t r = armed();
    run_to_the_end(&r, USB_PORT_ATTACH_NONE, 0);
    LS_EQ_STR(s_log, "");
    LS_CHECK(r.done);
    LS_CHECK(r.skipped);
    LS_EQ_INT(r.attempts, 0);
}

LS_CASE(nothing_attached_waits_for_the_first_look_then_stops_quietly)
{
    fresh();
    usb_boot_retry_t r = armed();
    usb_boot_retry_tick(&r, 9999999, 0, USB_PORT_ATTACH_NONE, fake_power);
    LS_CHECK(!r.done);
    usb_boot_retry_tick(&r, 10000000, 0, USB_PORT_ATTACH_NONE, fake_power);
    LS_CHECK(r.done && r.skipped);
    LS_EQ_STR(s_log, "");
}

LS_CASE(attached_but_not_enumerated_still_runs_all_three_cycles)
{
    fresh();
    usb_boot_retry_t r = armed();
    run_to_the_end(&r, USB_PORT_ATTACH_PRESENT, 0);
    LS_EQ_STR(s_log, "FNFNFN");
    LS_CHECK(r.done);
    LS_CHECK(!r.skipped);
    LS_EQ_INT(r.attempts, 3);
}

LS_CASE(the_cycle_timing_is_unchanged_for_an_attached_device)
{
    fresh();
    usb_boot_retry_t r = armed();
    for (int i = 0; i < 3; i++) {
        usb_boot_retry_tick(&r, r.next_us, 0, USB_PORT_ATTACH_PRESENT, fake_power);
        LS_CHECK(r.off);
        LS_EQ_INT((int)s_n, 2 * i + 1);
        /* Not before the 300 ms hold. */
        usb_boot_retry_tick(&r, r.next_us - 1, 0, USB_PORT_ATTACH_PRESENT, fake_power);
        LS_EQ_INT((int)s_n, 2 * i + 1);
        usb_boot_retry_tick(&r, r.next_us, 0, USB_PORT_ATTACH_PRESENT, fake_power);
        LS_CHECK(!r.off);
        LS_EQ_INT((int)s_n, 2 * i + 2);
    }
    usb_boot_retry_tick(&r, r.next_us, 0, USB_PORT_ATTACH_PRESENT, fake_power);
    LS_CHECK(r.done);
    LS_EQ_STR(s_log, "FNFNFN");
}

LS_CASE(a_port_that_cannot_say_is_treated_as_attached)
{
    fresh();
    usb_boot_retry_t r = armed();
    run_to_the_end(&r, USB_PORT_ATTACH_UNKNOWN, 0);
    LS_EQ_STR(s_log, "FNFNFN");
    LS_CHECK(!r.skipped);
}

LS_CASE(the_device_leaving_mid_recovery_ends_it_without_another_cycle)
{
    fresh();
    usb_boot_retry_t r = armed();
    usb_boot_retry_tick(&r, r.next_us, 0, USB_PORT_ATTACH_PRESENT, fake_power);
    usb_boot_retry_tick(&r, r.next_us, 0, USB_PORT_ATTACH_UNKNOWN, fake_power);
    LS_EQ_STR(s_log, "FN");
    /* Unplugged while the port was back on and waiting. */
    usb_boot_retry_tick(&r, r.next_us, 0, USB_PORT_ATTACH_NONE, fake_power);
    LS_CHECK(r.done && r.skipped);
    LS_EQ_STR(s_log, "FN");
}

LS_CASE(an_empty_reading_while_the_port_is_off_never_leaves_it_off)
{
    /* The caller passes UNKNOWN while it holds the port unpowered; even if
       it passed NONE, the power-on comes before any attached decision. */
    fresh();
    usb_boot_retry_t r = armed();
    usb_boot_retry_tick(&r, r.next_us, 0, USB_PORT_ATTACH_PRESENT, fake_power);
    LS_CHECK(r.off);
    usb_boot_retry_tick(&r, r.next_us, 0, USB_PORT_ATTACH_NONE, fake_power);
    LS_CHECK(!r.off);
    LS_EQ_STR(s_log, "FN");
}

LS_CASE(an_enumerated_device_stands_recovery_down)
{
    fresh();
    usb_boot_retry_t r = armed();
    usb_boot_retry_tick(&r, 0, 1, USB_PORT_ATTACH_PRESENT, fake_power);
    LS_CHECK(r.done && !r.skipped);
    usb_boot_retry_tick(&r, 100000000, 0, USB_PORT_ATTACH_PRESENT, fake_power);
    LS_EQ_STR(s_log, "");
}

LS_CASE(a_power_off_the_stack_refuses_ends_it_without_a_retry_storm)
{
    fresh();
    s_fail = ESP_FAIL;
    usb_boot_retry_t r = armed();
    run_to_the_end(&r, USB_PORT_ATTACH_PRESENT, 0);
    LS_EQ_STR(s_log, "FFF");
    LS_CHECK(!r.off);
}

LS_CASE(the_absent_dongle_notice_needs_something_attached)
{
    LS_CHECK(!usb_absent_notice_wanted(USB_PORT_ATTACH_NONE));
    LS_CHECK(usb_absent_notice_wanted(USB_PORT_ATTACH_PRESENT));
    LS_CHECK(usb_absent_notice_wanted(USB_PORT_ATTACH_UNKNOWN));
}
