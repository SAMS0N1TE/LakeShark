/* Early safe mode. */

#include "ls_test.h"
#include "ls_safe_mode.h"

#include <stddef.h>
#include <string.h>

/* ------------------------------------------------------------ helpers --- */

/* One boot. Runs the decision, and - unless the boot is told to fault - marks
   it healthy the way the device's one-shot timer does. Returns the verdict. */
static ls_safe_boot_t boot(ls_safe_state_t *st, ls_safe_reset_t reset,
                           int survive)
{
    ls_safe_boot_t b;
    ls_safe_decide(st, reset, &b);
    if (!b.safe && survive) {
        LS_CHECK(ls_safe_healthy_due(LS_SAFE_HEALTHY_MS));
        ls_safe_mark_healthy(st);
    }
    return b;
}

/* ------------------------------------------------------- classification -- */

LS_CASE(classify_covers_every_reset_reason)
{
    /* A reason this build does not know about must never be silently treated
       as clean - that would disarm the guard on exactly the chip revision
       nobody has tested. */
    for (int r = 0; r < LS_SAFE_RESET__COUNT; r++) {
        ls_safe_class_t c = ls_safe_classify((ls_safe_reset_t)r);
        LS_CHECK_MSG(c == LS_SAFE_CLASS_CLEAN || c == LS_SAFE_CLASS_FAULT ||
                     c == LS_SAFE_CLASS_POWER || c == LS_SAFE_CLASS_UNKNOWN,
                     "reset %d classified as %d", r, (int)c);
        LS_CHECK(ls_safe_reset_name((ls_safe_reset_t)r) != NULL);
    }

    LS_EQ_INT(ls_safe_classify(LS_SAFE_RESET_PANIC),      LS_SAFE_CLASS_FAULT);
    LS_EQ_INT(ls_safe_classify(LS_SAFE_RESET_INT_WDT),    LS_SAFE_CLASS_FAULT);
    LS_EQ_INT(ls_safe_classify(LS_SAFE_RESET_TASK_WDT),   LS_SAFE_CLASS_FAULT);
    LS_EQ_INT(ls_safe_classify(LS_SAFE_RESET_WDT),        LS_SAFE_CLASS_FAULT);
    LS_EQ_INT(ls_safe_classify(LS_SAFE_RESET_CPU_LOCKUP), LS_SAFE_CLASS_FAULT);

    /* 's reasoning, carried forward: a reflash, the reset button or a
       power cycle are not evidence that the firmware is broken. */
    LS_EQ_INT(ls_safe_classify(LS_SAFE_RESET_POWERON),   LS_SAFE_CLASS_CLEAN);
    LS_EQ_INT(ls_safe_classify(LS_SAFE_RESET_EXT),       LS_SAFE_CLASS_CLEAN);
    LS_EQ_INT(ls_safe_classify(LS_SAFE_RESET_SW),        LS_SAFE_CLASS_CLEAN);
    LS_EQ_INT(ls_safe_classify(LS_SAFE_RESET_USB),       LS_SAFE_CLASS_CLEAN);
    LS_EQ_INT(ls_safe_classify(LS_SAFE_RESET_JTAG),      LS_SAFE_CLASS_CLEAN);
    LS_EQ_INT(ls_safe_classify(LS_SAFE_RESET_DEEPSLEEP), LS_SAFE_CLASS_CLEAN);

    /* A sagging battery is a supply problem. Counting it as an app fault
       hides the real cause behind a wrong diagnosis. */
    LS_EQ_INT(ls_safe_classify(LS_SAFE_RESET_BROWNOUT),   LS_SAFE_CLASS_POWER);
    LS_EQ_INT(ls_safe_classify(LS_SAFE_RESET_PWR_GLITCH), LS_SAFE_CLASS_POWER);

    LS_EQ_INT(ls_safe_classify(LS_SAFE_RESET_UNKNOWN), LS_SAFE_CLASS_UNKNOWN);
}

/* -------------------------------------------------------- corrupt marker - */

LS_CASE(garbage_record_boots_normally_and_says_so)
{
    /* RTC_NOINIT memory is garbage on the first power-up of a board that has
       never run this firmware. Entering safe mode there would be a worse
       defect than the one this exists to catch. */
    ls_safe_state_t st;
    memset(&st, 0xA5, sizeof(st));

    ls_safe_boot_t b = boot(&st, LS_SAFE_RESET_POWERON, 0);
    LS_CHECK(!b.safe);
    LS_CHECK(b.marker_corrupt);
    LS_EQ_UINT(b.faults, 0);
    LS_EQ_INT(b.prev_stage, LS_SAFE_STAGE_NONE);
    LS_EQ_STR(b.app, "");
    /* And the record it left behind must be usable. */
    LS_CHECK(ls_safe_state_valid(&st));
}

LS_CASE(a_flipped_bit_invalidates_the_record)
{
    ls_safe_state_t st;
    ls_safe_state_reset(&st);
    ls_safe_note_stage(&st, LS_SAFE_STAGE_BACKEND);
    LS_CHECK(ls_safe_state_valid(&st));

    st.faults ^= 0x40u;                       /* body changed, CRC stale */
    LS_CHECK(!ls_safe_state_valid(&st));

    ls_safe_state_reset(&st);
    st.crc ^= 1u;                             /* CRC changed, body stale */
    LS_CHECK(!ls_safe_state_valid(&st));

    /* A record written by another build: the CRC is internally consistent and
       it must still be rejected, because none of the fields mean what this
       build thinks they mean. */
    ls_safe_state_reset(&st);
    st.version = (uint16_t)(LS_SAFE_VERSION + 1);
    st.crc = ls_safe_state_crc(&st);
    LS_CHECK(!ls_safe_state_valid(&st));

    ls_safe_state_reset(&st);
    st.size = (uint16_t)(sizeof(ls_safe_state_t) + 4);
    st.crc = ls_safe_state_crc(&st);
    LS_CHECK(!ls_safe_state_valid(&st));

    ls_safe_state_reset(&st);
    st.magic = 0;
    st.crc = ls_safe_state_crc(&st);
    LS_CHECK(!ls_safe_state_valid(&st));
}

LS_CASE(corrupt_record_never_leaves_a_report_reading_past_its_field)
{
    /* A record whose app[] has no terminator would run the report off the end
       of the struct. Validation rejects it before anything formats it. */
    ls_safe_state_t st;
    ls_safe_state_reset(&st);
    memset(st.app, 'X', sizeof(st.app));
    ls_safe_state_seal(&st);
    LS_CHECK(ls_safe_state_valid(&st));   /* seal re-terminates it */
    LS_EQ_INT((int)strlen(st.app), LS_SAFE_APP_MAX - 1);

    memset(st.app, 'X', sizeof(st.app));
    st.crc = ls_safe_state_crc(&st);      /* CRC agrees, terminator does not */
    LS_CHECK(!ls_safe_state_valid(&st));

    /* Same for a stage index that would run off the name table. */
    ls_safe_state_reset(&st);
    st.stage = (uint8_t)LS_SAFE_STAGE__COUNT;
    st.crc = ls_safe_state_crc(&st);
    LS_CHECK(!ls_safe_state_valid(&st));
}

/* -------------------------------------------------------- fault counting - */

LS_CASE(repeated_startup_faults_reach_safe_mode)
{
    ls_safe_state_t st;
    ls_safe_state_reset(&st);

    /* A first boot that dies before the healthy interval. */
    ls_safe_boot_t b = boot(&st, LS_SAFE_RESET_POWERON, 0);
    LS_CHECK(!b.safe);
    LS_EQ_UINT(b.faults, 0);

    for (uint32_t i = 1; i < LS_SAFE_FAULT_LIMIT; i++) {
        b = boot(&st, LS_SAFE_RESET_PANIC, 0);
        LS_CHECK_MSG(!b.safe, "safe mode entered after only %lu fault(s)",
                     (unsigned long)i);
        LS_EQ_UINT(b.faults, i);
    }

    b = boot(&st, LS_SAFE_RESET_PANIC, 0);
    LS_CHECK(b.safe);
    LS_EQ_INT(b.entry, LS_SAFE_ENTRY_REPEATED_FAULT);
    LS_EQ_UINT(b.faults, LS_SAFE_FAULT_LIMIT);
    LS_EQ_INT(b.reset_class, LS_SAFE_CLASS_FAULT);
}

LS_CASE(a_watchdog_counts_the_same_as_a_panic)
{
    const ls_safe_reset_t mix[] = {
        LS_SAFE_RESET_TASK_WDT, LS_SAFE_RESET_INT_WDT, LS_SAFE_RESET_CPU_LOCKUP
    };
    ls_safe_state_t st;
    ls_safe_state_reset(&st);
    ls_safe_boot_t b = boot(&st, LS_SAFE_RESET_POWERON, 0);

    for (unsigned i = 0; i < sizeof(mix) / sizeof(mix[0]); i++)
        b = boot(&st, mix[i], 0);

    LS_CHECK(b.safe);
    LS_EQ_UINT(b.faults, 3);
}

LS_CASE(healthy_boot_clears_the_counter)
{
    ls_safe_state_t st;
    ls_safe_state_reset(&st);

    boot(&st, LS_SAFE_RESET_POWERON, 0);
    ls_safe_boot_t b = boot(&st, LS_SAFE_RESET_PANIC, 0);
    LS_EQ_UINT(b.faults, 1);

    /* This one survives long enough to be trusted. */
    b = boot(&st, LS_SAFE_RESET_PANIC, 1);
    LS_EQ_UINT(b.faults, 2);
    LS_CHECK(!b.safe);

    b = boot(&st, LS_SAFE_RESET_PANIC, 0);
    LS_EQ_UINT(b.faults, 0);
    LS_CHECK(!b.safe);
}

LS_CASE(healthy_needs_the_whole_interval)
{
    LS_CHECK(!ls_safe_healthy_due(0));
    LS_CHECK(!ls_safe_healthy_due(LS_SAFE_HEALTHY_MS - 1));
    LS_CHECK(ls_safe_healthy_due(LS_SAFE_HEALTHY_MS));
    LS_CHECK(ls_safe_healthy_due(LS_SAFE_HEALTHY_MS + 1));
    /* The end of app_main is nowhere near it: most of what kills this board
       kills it a few seconds into the first app. */
    LS_CHECK(!ls_safe_healthy_due(3000));
}

LS_CASE(deliberate_resets_are_not_counted)
{
    /* A reflash, the reset button, and the USB-dongle recovery restart all
       land mid-boot with the guard armed. Counting them is what made the
       guard cry wolf; three of them must not reach safe mode. */
    const ls_safe_reset_t clean[] = {
        LS_SAFE_RESET_SW, LS_SAFE_RESET_EXT, LS_SAFE_RESET_POWERON,
        LS_SAFE_RESET_USB, LS_SAFE_RESET_JTAG, LS_SAFE_RESET_DEEPSLEEP
    };
    ls_safe_state_t st;
    ls_safe_state_reset(&st);

    for (unsigned i = 0; i < sizeof(clean) / sizeof(clean[0]); i++) {
        ls_safe_boot_t b = boot(&st, clean[i], 0);
        LS_CHECK_MSG(!b.safe, "clean reset %d entered safe mode", (int)clean[i]);
        LS_EQ_UINT(b.faults, 0);
    }
}

LS_CASE(brownouts_are_reported_but_never_blamed_on_the_firmware)
{
    ls_safe_state_t st;
    ls_safe_state_reset(&st);

    ls_safe_boot_t b = boot(&st, LS_SAFE_RESET_POWERON, 0);
    for (int i = 0; i < 6; i++) {
        b = boot(&st, LS_SAFE_RESET_BROWNOUT, 0);
        LS_CHECK_MSG(!b.safe, "brownout %d was treated as an app defect", i);
        LS_EQ_UINT(b.faults, 0);
    }
    LS_EQ_UINT(b.power_events, 6);
    LS_EQ_INT(b.reset_class, LS_SAFE_CLASS_POWER);

    /* And the report keeps them apart. */
    char buf[LS_SAFE_REPORT_MAX];
    ls_safe_format(buf, sizeof(buf), &b, "fw", LS_SAFE_DUMP_NONE);
    LS_CHECK(strstr(buf, "power resets: 6") != NULL);
    LS_CHECK(strstr(buf, "not counted as a firmware fault") != NULL);
}

LS_CASE(an_unattributed_reset_still_breaks_the_loop)
{
    /* The chip cannot always say why it reset. Escaping the loop matters more
       than attribution - but the report must call it unknown, not a panic. */
    ls_safe_state_t st;
    ls_safe_state_reset(&st);
    boot(&st, LS_SAFE_RESET_POWERON, 0);

    ls_safe_boot_t b = {0};
    for (uint32_t i = 0; i < LS_SAFE_FAULT_LIMIT; i++)
        b = boot(&st, LS_SAFE_RESET_UNKNOWN, 0);
    LS_CHECK(b.safe);

    char buf[LS_SAFE_REPORT_MAX];
    ls_safe_format(buf, sizeof(buf), &b, NULL, LS_SAFE_DUMP_UNKNOWN);
    LS_CHECK(strstr(buf, "unknown") != NULL);
    LS_CHECK(strstr(buf, "panic") == NULL);
}

/* ------------------------------------------------------- forced entry ---- */

LS_CASE(forced_entry_is_immediate_and_sticky)
{
    ls_safe_state_t st;
    ls_safe_state_reset(&st);
    boot(&st, LS_SAFE_RESET_POWERON, 1);

    ls_safe_request_force(&st, true);
    ls_safe_boot_t b = boot(&st, LS_SAFE_RESET_SW, 0);
    LS_CHECK(b.safe);
    LS_EQ_INT(b.entry, LS_SAFE_ENTRY_FORCED);

    LS_EQ_UINT(b.faults, 0);

    /* Power-cycling out of it is not enough: it was asked for. */
    b = boot(&st, LS_SAFE_RESET_POWERON, 0);
    LS_CHECK(b.safe);
    LS_EQ_INT(b.entry, LS_SAFE_ENTRY_FORCED);

    ls_safe_request_force(&st, false);
    b = boot(&st, LS_SAFE_RESET_SW, 0);
    LS_CHECK(!b.safe);
}

LS_CASE(leaving_safe_mode_clears_a_stale_counter)
{
    ls_safe_state_t st;
    ls_safe_state_reset(&st);
    boot(&st, LS_SAFE_RESET_POWERON, 0);
    for (uint32_t i = 0; i < LS_SAFE_FAULT_LIMIT; i++)
        boot(&st, LS_SAFE_RESET_PANIC, 0);

    /* Without this, `safemode off` would be answered by walking straight back
       into safe mode on the next boot. */
    ls_safe_request_force(&st, false);
    ls_safe_boot_t b = boot(&st, LS_SAFE_RESET_SW, 0);
    LS_CHECK(!b.safe);
    LS_EQ_UINT(b.faults, 0);
}

LS_CASE(safe_mode_does_not_count_its_own_resets)
{
    ls_safe_state_t st;
    ls_safe_state_reset(&st);
    ls_safe_request_force(&st, true);

    ls_safe_boot_t b = boot(&st, LS_SAFE_RESET_SW, 0);
    LS_CHECK(b.safe);

    /* A brownout or a watchdog while sitting in safe mode is not a failed
       start: safe mode started nothing. */
    b = boot(&st, LS_SAFE_RESET_TASK_WDT, 0);
    LS_EQ_UINT(b.faults, 0);
    LS_CHECK(b.prev_was_safe);
}

/* --------------------------------------------------------- retry path ---- */

LS_CASE(try_normal_boot_runs_normally_and_re_arms)
{
    ls_safe_state_t st;
    ls_safe_state_reset(&st);
    boot(&st, LS_SAFE_RESET_POWERON, 0);
    ls_safe_boot_t b = {0};
    for (uint32_t i = 0; i < LS_SAFE_FAULT_LIMIT; i++)
        b = boot(&st, LS_SAFE_RESET_PANIC, 0);
    LS_CHECK(b.safe);

    /* TRY NORMAL BOOT, then the software restart it causes. */
    ls_safe_request_retry(&st);
    b = boot(&st, LS_SAFE_RESET_SW, 0);
    LS_CHECK(!b.safe);
    LS_EQ_INT(b.entry, LS_SAFE_ENTRY_NONE);

    b = boot(&st, LS_SAFE_RESET_PANIC, 0);
    LS_CHECK(b.safe);
    LS_EQ_INT(b.entry, LS_SAFE_ENTRY_RETRY_FAILED);

    char buf[LS_SAFE_REPORT_MAX];
    ls_safe_format(buf, sizeof(buf), &b, "fw", LS_SAFE_DUMP_PRESENT);
    LS_CHECK(strstr(buf, "retry") != NULL);
}

LS_CASE(a_retry_that_survives_clears_everything)
{
    ls_safe_state_t st;
    ls_safe_state_reset(&st);
    boot(&st, LS_SAFE_RESET_POWERON, 0);
    for (uint32_t i = 0; i < LS_SAFE_FAULT_LIMIT; i++)
        boot(&st, LS_SAFE_RESET_PANIC, 0);

    ls_safe_request_retry(&st);
    ls_safe_boot_t b = boot(&st, LS_SAFE_RESET_SW, 1);   /* survives */
    LS_CHECK(!b.safe);

    /* A later single fault must not be read as the retry failing. */
    b = boot(&st, LS_SAFE_RESET_PANIC, 0);
    LS_CHECK(!b.safe);
    LS_EQ_UINT(b.faults, 0);
}

LS_CASE(stay_safe_survives_a_power_cycle)
{
    ls_safe_state_t st;
    ls_safe_state_reset(&st);
    boot(&st, LS_SAFE_RESET_POWERON, 0);
    for (uint32_t i = 0; i < LS_SAFE_FAULT_LIMIT; i++)
        boot(&st, LS_SAFE_RESET_PANIC, 0);

    ls_safe_request_stay(&st);
    for (int i = 0; i < 3; i++) {
        ls_safe_boot_t b = boot(&st, LS_SAFE_RESET_POWERON, 0);
        LS_CHECK(b.safe);
        LS_EQ_INT(b.entry, LS_SAFE_ENTRY_FORCED);
    }
}

LS_CASE(failed_safe_display_attempt_latches_serial_only_fallback)
{
    ls_safe_state_t st;
    ls_safe_state_reset(&st);
    ls_safe_request_force(&st, true);
    ls_safe_boot_t b = boot(&st, LS_SAFE_RESET_SW, 0);
    LS_CHECK(b.safe);

    LS_CHECK(ls_safe_display_should_attempt(&st));
    ls_safe_display_begin(&st);
    LS_CHECK(!ls_safe_display_should_attempt(&st));
    LS_CHECK(ls_safe_state_valid(&st));

    /* Models either CONFIG_BSP_ERROR_CHECK aborting or a blocked LCD call
       eventually being reset by a watchdog. Safe-mode resets do not count as
       normal startup faults, and the retained latch prevents another attempt. */
    b = boot(&st, LS_SAFE_RESET_TASK_WDT, 0);
    LS_CHECK(b.safe);
    LS_CHECK(!ls_safe_display_should_attempt(&st));

    /* A returned failure and STAY SAFE also retain serial-only mode. */
    ls_safe_display_complete(&st, false);
    ls_safe_request_stay(&st);
    LS_CHECK(!ls_safe_display_should_attempt(&st));
}

LS_CASE(successful_safe_display_and_explicit_normal_retry_reenable_one_attempt)
{
    ls_safe_state_t st;
    ls_safe_state_reset(&st);
    ls_safe_request_force(&st, true);
    boot(&st, LS_SAFE_RESET_SW, 0);

    ls_safe_display_begin(&st);
    ls_safe_display_complete(&st, true);
    LS_CHECK(ls_safe_display_should_attempt(&st));

    ls_safe_display_begin(&st);             /* next attempt aborts */
    boot(&st, LS_SAFE_RESET_PANIC, 0);
    LS_CHECK(!ls_safe_display_should_attempt(&st));

    /* Only an explicit TRY NORMAL BOOT clears the latch. The normal path is
       still a full boot, and one failed retry returns to safe mode with one
       fresh (and again bounded-to-one) display opportunity. */
    ls_safe_request_retry(&st);
    LS_CHECK(ls_safe_display_should_attempt(&st));
    ls_safe_boot_t b = boot(&st, LS_SAFE_RESET_SW, 0);
    LS_CHECK(!b.safe);
    ls_safe_boot_plan_t p;
    ls_safe_boot_plan(b.safe, &p);
    LS_CHECK(p.display);
    LS_CHECK(p.full_console);

    b = boot(&st, LS_SAFE_RESET_PANIC, 0);
    LS_CHECK(b.safe);
    LS_EQ_INT(b.entry, LS_SAFE_ENTRY_RETRY_FAILED);
    LS_CHECK(ls_safe_display_should_attempt(&st));
}

/* ------------------------------------------------------ startup stage ---- */

LS_CASE(the_stage_the_last_boot_died_in_is_carried_across)
{
    ls_safe_state_t st;
    ls_safe_state_reset(&st);
    boot(&st, LS_SAFE_RESET_POWERON, 0);

    /* A boot that gets as far as the codec and dies there. */
    ls_safe_note_stage(&st, LS_SAFE_STAGE_C6);
    ls_safe_note_stage(&st, LS_SAFE_STAGE_NVS);
    ls_safe_note_stage(&st, LS_SAFE_STAGE_CODEC);

    ls_safe_boot_t b = boot(&st, LS_SAFE_RESET_PANIC, 0);
    LS_EQ_INT(b.prev_stage, LS_SAFE_STAGE_CODEC);

    char buf[LS_SAFE_REPORT_MAX];
    ls_safe_format(buf, sizeof(buf), &b, "fw", LS_SAFE_DUMP_NONE);
    LS_CHECK(strstr(buf, "audio codec") != NULL);

    /* And the boot that is now running starts over at entry. */
    LS_EQ_INT(st.stage, LS_SAFE_STAGE_ENTRY);

    for (int i = 0; i < LS_SAFE_STAGE__COUNT; i++)
        LS_CHECK(ls_safe_stage_name((ls_safe_stage_t)i) != NULL);
}

LS_CASE(an_out_of_range_stage_is_refused)
{
    ls_safe_state_t st;
    ls_safe_state_reset(&st);
    ls_safe_note_stage(&st, LS_SAFE_STAGE_SHELL);
    ls_safe_note_stage(&st, (ls_safe_stage_t)(LS_SAFE_STAGE__COUNT + 7));
    LS_EQ_INT(st.stage, LS_SAFE_STAGE_SHELL);
    LS_CHECK(ls_safe_state_valid(&st));
}

LS_CASE(the_last_app_is_carried_across_and_bounded)
{
    ls_safe_state_t st;
    ls_safe_state_reset(&st);
    boot(&st, LS_SAFE_RESET_POWERON, 0);

    ls_safe_note_app(&st, "ACARS");
    ls_safe_boot_t b = boot(&st, LS_SAFE_RESET_PANIC, 0);
    LS_EQ_STR(b.app, "ACARS");

    char buf[LS_SAFE_REPORT_MAX];
    ls_safe_format(buf, sizeof(buf), &b, "fw", LS_SAFE_DUMP_NONE);
    LS_CHECK(strstr(buf, "last app: ACARS") != NULL);

    ls_safe_note_app(&st, "0123456789012345678901234567890123456789");
    LS_EQ_INT((int)strlen(st.app), LS_SAFE_APP_MAX - 1);
    LS_CHECK(ls_safe_state_valid(&st));

    ls_safe_note_app(&st, NULL);
    LS_EQ_STR(st.app, "");
}

/* --------------------------------------------------------- boot plan ----- */

/* Every field of the plan, so a field added later cannot quietly default to
   "start it" in safe mode. */
typedef struct {
    const char *name;
    size_t      off;
    int         allowed_in_safe;
} plan_field_t;

#define PF(f, allowed) { #f, offsetof(ls_safe_boot_plan_t, f), allowed }

static const plan_field_t PLAN_FIELDS[] = {
    PF(display, 1),
    PF(touch, 1),
    PF(console, 1),
    PF(full_console, 0),
    PF(c6, 0),
    PF(ble, 0),
    PF(nvs, 0),
    PF(storage_autorepair, 0),
    PF(storage_mount, 0),
    PF(codec, 0),
    PF(boot_splash, 0),
    PF(boot_sound, 0),
    PF(backend, 0),
    PF(apps, 0),
    PF(restore_last_app, 0),
    PF(coredump_parse, 0),
};

LS_CASE(safe_mode_starts_nothing_heavy)
{
    ls_safe_boot_plan_t p;
    memset(&p, 0xFF, sizeof(p));
    ls_safe_boot_plan(true, &p);

    /* The field table must cover the struct, or a new field escapes the
       check by not being listed. */
    LS_EQ_UINT(sizeof(PLAN_FIELDS) / sizeof(PLAN_FIELDS[0]),
               sizeof(ls_safe_boot_plan_t) / sizeof(bool));

    for (unsigned i = 0; i < sizeof(PLAN_FIELDS) / sizeof(PLAN_FIELDS[0]); i++) {
        const bool v = *(const bool *)((const char *)&p + PLAN_FIELDS[i].off);
        if (!PLAN_FIELDS[i].allowed_in_safe)
            LS_CHECK_MSG(!v, "safe mode would start '%s'", PLAN_FIELDS[i].name);
        else
            LS_CHECK_MSG(v, "safe mode would not start '%s'", PLAN_FIELDS[i].name);
    }
}

LS_CASE(a_normal_boot_still_starts_everything)
{
    ls_safe_boot_plan_t p;
    memset(&p, 0xFF, sizeof(p));
    ls_safe_boot_plan(false, &p);

    for (unsigned i = 0; i < sizeof(PLAN_FIELDS) / sizeof(PLAN_FIELDS[0]); i++) {
        const bool v = *(const bool *)((const char *)&p + PLAN_FIELDS[i].off);
        /* coredump_parse is the one thing neither mode does automatically -
           the parser faulted on a checksum-valid image and turned a
           panic into a boot loop that needed a flash erase to escape. */
        if (PLAN_FIELDS[i].off == offsetof(ls_safe_boot_plan_t, coredump_parse))
            LS_CHECK(!v);
        else
            LS_CHECK_MSG(v, "normal boot would not start '%s'",
                         PLAN_FIELDS[i].name);
    }
}

/* ------------------------------------------------- sequencing contract --- */

/* A stand-in for app_main: it consults the plan in the order the firmware
   does, and records what it started. This is what proves the ORDER as well
   as the set - the decision has to happen before the C6 probe, not after it,
   which is exactly where the existing guard was too late. */
typedef struct {
    int  order[24];
    int  n;
    int  decided_at;
} sequence_t;

enum { STEP_DECIDE = 1, STEP_C6, STEP_NVS, STEP_STORAGE, STEP_CODEC,
       STEP_DISPLAY, STEP_BACKEND, STEP_APPS, STEP_BLE, STEP_SOUND,
       STEP_CONSOLE };

static void record(sequence_t *s, int step)
{
    if (s->n < (int)(sizeof(s->order) / sizeof(s->order[0])))
        s->order[s->n++] = step;
}

static int ran(const sequence_t *s, int step)
{
    for (int i = 0; i < s->n; i++) if (s->order[i] == step) return 1;
    return 0;
}

static int position(const sequence_t *s, int step)
{
    for (int i = 0; i < s->n; i++) if (s->order[i] == step) return i;
    return -1;
}

static void fake_app_main(ls_safe_state_t *st, ls_safe_reset_t reset,
                          sequence_t *seq)
{
    ls_safe_boot_t b;
    ls_safe_decide(st, reset, &b);
    record(seq, STEP_DECIDE);
    seq->decided_at = 0;

    ls_safe_boot_plan_t p;
    ls_safe_boot_plan(b.safe, &p);

    /* the GUI safe path starts its independent serial task before it
       enters the assert-enabled LCD BSP. Keep normal boot's established order
       unchanged: its full console still starts after display/backend/apps. */
    if (b.safe) {
        if (p.console) record(seq, STEP_CONSOLE);
        if (p.display) record(seq, STEP_DISPLAY);
        return;
    }

    if (p.c6)            record(seq, STEP_C6);
    if (p.nvs)           record(seq, STEP_NVS);
    if (p.storage_mount) record(seq, STEP_STORAGE);
    if (p.codec)         record(seq, STEP_CODEC);
    if (p.display)       record(seq, STEP_DISPLAY);
    if (p.backend)       record(seq, STEP_BACKEND);
    if (p.apps)          record(seq, STEP_APPS);
    if (p.ble)           record(seq, STEP_BLE);
    if (p.boot_sound)    record(seq, STEP_SOUND);
    if (p.console)       record(seq, STEP_CONSOLE);
}

LS_CASE(recovery_is_decided_before_anything_can_fault)
{
    ls_safe_state_t st;
    ls_safe_state_reset(&st);
    boot(&st, LS_SAFE_RESET_POWERON, 0);
    for (uint32_t i = 0; i < LS_SAFE_FAULT_LIMIT - 1; i++)
        boot(&st, LS_SAFE_RESET_PANIC, 0);

    sequence_t seq = {{0}, 0, -1};
    fake_app_main(&st, LS_SAFE_RESET_PANIC, &seq);

    LS_EQ_INT(seq.order[0], STEP_DECIDE);
    LS_EQ_INT(seq.decided_at, 0);

    LS_CHECK(!ran(&seq, STEP_C6));
    LS_CHECK(!ran(&seq, STEP_NVS));
    LS_CHECK(!ran(&seq, STEP_STORAGE));
    LS_CHECK(!ran(&seq, STEP_CODEC));
    LS_CHECK(!ran(&seq, STEP_BACKEND));
    LS_CHECK(!ran(&seq, STEP_APPS));
    LS_CHECK(!ran(&seq, STEP_BLE));
    LS_CHECK(!ran(&seq, STEP_SOUND));

    LS_CHECK(ran(&seq, STEP_DISPLAY));
    LS_CHECK(ran(&seq, STEP_CONSOLE));
    LS_CHECK(position(&seq, STEP_CONSOLE) < position(&seq, STEP_DISPLAY));
}

LS_CASE(a_healthy_board_still_boots_the_whole_radio)
{
    ls_safe_state_t st;
    ls_safe_state_reset(&st);

    sequence_t seq = {{0}, 0, -1};
    fake_app_main(&st, LS_SAFE_RESET_POWERON, &seq);

    LS_EQ_INT(seq.order[0], STEP_DECIDE);
    LS_CHECK(ran(&seq, STEP_C6));
    LS_CHECK(ran(&seq, STEP_NVS));
    LS_CHECK(ran(&seq, STEP_STORAGE));
    LS_CHECK(ran(&seq, STEP_CODEC));
    LS_CHECK(ran(&seq, STEP_DISPLAY));
    LS_CHECK(ran(&seq, STEP_BACKEND));
    LS_CHECK(ran(&seq, STEP_APPS));
    LS_CHECK(ran(&seq, STEP_BLE));
    LS_CHECK(ran(&seq, STEP_SOUND));
    LS_CHECK(ran(&seq, STEP_CONSOLE));
    LS_CHECK(position(&seq, STEP_DISPLAY) < position(&seq, STEP_CONSOLE));
}

/* ------------------------------------------------------------ report ----- */

LS_CASE(report_never_claims_a_decoded_backtrace)
{
    ls_safe_state_t st;
    ls_safe_state_reset(&st);
    boot(&st, LS_SAFE_RESET_POWERON, 0);
    ls_safe_boot_t b = {0};
    for (uint32_t i = 0; i < LS_SAFE_FAULT_LIMIT; i++)
        b = boot(&st, LS_SAFE_RESET_PANIC, 0);

    char buf[LS_SAFE_REPORT_MAX];
    size_t n = ls_safe_format(buf, sizeof(buf), &b,
                              "LakeShark v0.1-dirty", LS_SAFE_DUMP_PRESENT);
    LS_CHECK(n > 0 && n < sizeof(buf));
    LS_EQ_INT((int)strlen(buf), (int)n);

    LS_CHECK(strstr(buf, "*** SAFE MODE ***") != NULL);
    LS_CHECK(strstr(buf, "LakeShark v0.1-dirty") != NULL);
    LS_CHECK(strstr(buf, "panic") != NULL);
    LS_CHECK(strstr(buf, "no backtrace decoded") != NULL);
    /* Availability, not content: nothing has parsed the stored image. */
    LS_CHECK(strstr(buf, "read it with 'crash'") != NULL);
}

LS_CASE(report_says_when_the_dump_state_is_not_known)
{
    ls_safe_boot_t b = {0};
    char buf[LS_SAFE_REPORT_MAX];

    ls_safe_format(buf, sizeof(buf), &b, NULL, LS_SAFE_DUMP_UNKNOWN);
    LS_CHECK(strstr(buf, "coredump: not checked") != NULL);

    ls_safe_format(buf, sizeof(buf), &b, NULL, LS_SAFE_DUMP_NONE);
    LS_CHECK(strstr(buf, "coredump: none stored") != NULL);
}

LS_CASE(report_truncates_instead_of_overrunning)
{
    ls_safe_state_t st;
    ls_safe_state_reset(&st);
    ls_safe_note_app(&st, "REC");
    boot(&st, LS_SAFE_RESET_POWERON, 0);
    ls_safe_boot_t b = boot(&st, LS_SAFE_RESET_BROWNOUT, 0);

    for (size_t cap = 1; cap < 200; cap++) {
        char guard[256];
        memset(guard, 0x7E, sizeof(guard));
        size_t n = ls_safe_format(guard, cap, &b, "identity string", 1);
        LS_CHECK_MSG(n < cap, "wrote %u into a %u byte buffer",
                     (unsigned)n, (unsigned)cap);
        LS_EQ_INT((int)strlen(guard), (int)n);
        LS_CHECK_MSG(guard[cap] == 0x7E, "wrote past cap=%u", (unsigned)cap);
    }

    LS_EQ_UINT(ls_safe_format(NULL, 64, &b, NULL, 0), 0);
    char one[4] = "zzz";
    LS_EQ_UINT(ls_safe_format(one, 0, &b, NULL, 0), 0);
    LS_EQ_STR(one, "zzz");
}
