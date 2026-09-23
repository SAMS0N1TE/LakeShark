/* LS_TEST_SOURCES: ${FW}/components/lakeshark/radio/usb_port_cycle.c */
#include "ls_test.h"
#include "usb_port_cycle.h"

#include <stdbool.h>
#include <string.h>

/* a scripted root port. Power commands are logged in order ('F' is
   off, 'N' is an on attempt) so a case can assert the sequence itself, not
   just the verdict - the ordering is what keeps IDF 5.4.3 from aborting. */
typedef struct {
    int count;          /* what device_count() reports before the power-off */
    int release_after;  /* polls after power-off until it reports 0; -1 never */
    int on_refusals;    /* INVALID_STATE this many times first; -1 always */
    esp_err_t off_result;
    int polls_after_off;
    bool powered_off;
    bool released_before_first_on;
    unsigned delayed_ms;
    char log[128];
    size_t n;
} fake_port_t;

static fake_port_t F;

static void fake_reset(void)
{
    memset(&F, 0, sizeof(F));
    F.count = 1;
    F.off_result = ESP_OK;
}

static void note(char c)
{
    if (F.n + 1 < sizeof(F.log)) {
        F.log[F.n++] = c;
        F.log[F.n] = '\0';
    }
}

static int fake_device_count(void)
{
    if (!F.powered_off) return F.count;
    ++F.polls_after_off;
    if (F.release_after >= 0 && F.polls_after_off > F.release_after) return 0;
    return F.count;
}

static esp_err_t fake_set_power(bool on)
{
    if (!on) {
        note('F');
        if (F.off_result != ESP_OK) return F.off_result;
        F.powered_off = true;
        return ESP_OK;
    }
    if (!strchr(F.log, 'N'))
        F.released_before_first_on = F.release_after >= 0 &&
                                     F.polls_after_off > F.release_after;
    note('N');
    if (F.on_refusals < 0) return ESP_ERR_INVALID_STATE;
    if (F.on_refusals > 0) {
        --F.on_refusals;
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

static void fake_delay(uint32_t ms) { F.delayed_ms += ms; }

static const usb_port_cycle_ops_t OPS = {
    .set_power = fake_set_power,
    .device_count = fake_device_count,
    .delay_ms = fake_delay,
};

LS_CASE(a_port_with_nothing_enumerated_is_never_powered_off)
{
    /* IDF 5.4.3 ESP_ERROR_CHECKs a device node that is not there when an
       unpowered port sees a disconnect with nothing enumerated. */
    fake_reset();
    F.count = 0;
    LS_EQ_INT(usb_port_cycle_run(&OPS), USB_PORT_CYCLE_NOTHING);
    LS_EQ_STR(F.log, "");
}

LS_CASE(a_count_the_stack_cannot_give_is_treated_as_nothing)
{
    fake_reset();
    F.count = -1;
    LS_EQ_INT(usb_port_cycle_run(&OPS), USB_PORT_CYCLE_NOTHING);
    LS_EQ_STR(F.log, "");
}

LS_CASE(the_port_comes_back_on_only_after_the_device_is_released)
{
    fake_reset();
    F.release_after = 3;
    LS_EQ_INT(usb_port_cycle_run(&OPS), USB_PORT_CYCLE_DONE);
    LS_EQ_STR(F.log, "FN");
    LS_CHECK(F.released_before_first_on);
    LS_EQ_INT(F.polls_after_off, 4);
}

LS_CASE(power_on_is_retried_while_the_hub_is_still_recycling_the_port)
{
    fake_reset();
    F.release_after = 0;
    F.on_refusals = 3;
    LS_EQ_INT(usb_port_cycle_run(&OPS), USB_PORT_CYCLE_DONE);
    LS_EQ_STR(F.log, "FNNNN");
}

LS_CASE(a_device_that_outlives_the_power_off_still_gets_its_port_back)
{
    fake_reset();
    F.release_after = -1;
    LS_EQ_INT(usb_port_cycle_run(&OPS), USB_PORT_CYCLE_NOT_RELEASED);
    LS_EQ_STR(F.log, "FN");
    LS_CHECK(!F.released_before_first_on);
    /* Bounded: the release wait gave up, it did not spin. */
    LS_EQ_UINT(F.delayed_ms,
               USB_PORT_CYCLE_RELEASE_MS + USB_PORT_CYCLE_HOLD_OFF_MS);
}

LS_CASE(a_port_that_never_powers_back_on_is_reported_not_retried_forever)
{
    fake_reset();
    F.release_after = 0;
    F.on_refusals = -1;
    LS_EQ_INT(usb_port_cycle_run(&OPS), USB_PORT_CYCLE_STILL_OFF);
    LS_EQ_UINT(strlen(F.log),
               1 + USB_PORT_CYCLE_POWER_ON_MS / USB_PORT_CYCLE_POLL_MS + 1);
    LS_EQ_INT(F.log[0], 'F');
}

LS_CASE(a_refused_power_off_leaves_the_port_alone)
{
    fake_reset();
    F.off_result = ESP_ERR_INVALID_STATE;
    LS_EQ_INT(usb_port_cycle_run(&OPS), USB_PORT_CYCLE_OFF_FAILED);
    LS_EQ_STR(F.log, "F");
}

LS_CASE(every_result_has_a_name)
{
    for (int r = USB_PORT_CYCLE_DONE; r <= USB_PORT_CYCLE_STILL_OFF; ++r)
        LS_CHECK(strcmp(usb_port_cycle_result_name(
                            (usb_port_cycle_result_t)r), "?") != 0);
}
