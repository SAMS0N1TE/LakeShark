/* LS_TEST_SOURCES: ${FW}/components/lakeshark/core/ls_crash.c */
/* LS_TEST_INCLUDE: ${FW}/components/lakeshark/core */
/**/

#include "ls_test.h"
#include "ls_crash.h"

#include <string.h>

/* Programmable source. Presence and summary reads are separate because boot
   may inspect the former but must never invoke the latter. */
static ls_crash_summary_t s_stub;
static ls_crash_read_result_t s_stub_read_result = LS_CRASH_READ_OK;
static int                s_present_calls;
static int                s_read_calls;
static int                s_erase_calls;
static bool               s_erase_ok = true;

static bool contains(const char *hay, const char *needle)
{
    return strstr(hay, needle) != NULL;
}

static bool stub_present(void)
{
    s_present_calls++;
    return s_stub.present;
}

static ls_crash_read_result_t stub_read(ls_crash_summary_t *out)
{
    s_read_calls++;
    *out = s_stub;
    return s_stub_read_result;
}

static bool stub_erase(void)
{
    s_erase_calls++;
    if (s_erase_ok) s_stub.present = false;
    return s_erase_ok;
}

static const ls_crash_source_t STUB_SRC = {
    .present = stub_present,
    .read  = stub_read,
    .erase = stub_erase,
};

static void reset(void)
{
    memset(&s_stub, 0, sizeof(s_stub));
    s_stub_read_result = LS_CRASH_READ_OK;
    s_present_calls = 0;
    s_read_calls   = 0;
    s_erase_calls  = 0;
    s_erase_ok     = true;
}

LS_CASE(initialization_and_presence_never_read_the_summary)
{
    /* This callback stands in for esp_core_dump_get_summary(), which
       took a load-access fault on a checksum-valid LCD4.3 dump. A stored dump
       must remain visible at boot without entering that callback. */
    reset();
    s_stub.present = true;

    ls_crash_init(&STUB_SRC, NULL);
    LS_EQ_INT(s_present_calls, 1);
    LS_EQ_INT(s_read_calls, 0);
    LS_CHECK(ls_crash_present());
    LS_EQ_INT(s_read_calls, 0);

    char notice[192];
    LS_CHECK(ls_crash_boot_notice(notice, sizeof(notice)) > 0);
    LS_EQ_INT(s_read_calls, 0);
}

LS_CASE(quarantined_diagnostic_read_is_not_a_fabricated_summary)
{
    reset();
    s_stub.present = true;
    s_stub_read_result = LS_CRASH_READ_QUARANTINED;
    ls_crash_init(&STUB_SRC, NULL);

    char buf[256];
    ls_crash_format(buf, sizeof(buf));
    LS_EQ_INT(s_read_calls, 1);
    LS_CHECK(contains(buf, "quarantined"));
    LS_CHECK(contains(buf, "summary not read"));
    LS_CHECK(ls_crash_present());
}

LS_CASE(empty_source_says_no_crash_recorded)
{
    /* The console command has one entry point - ls_crash_format - and the
       "no dump" case is formatted here, not by the caller. That way a
       future refactor can not accidentally return an empty buffer to the
       user and call it a report. */
    reset();
    ls_crash_init(&STUB_SRC, "2026-09-06 05:57:33");

    char buf[256];
    size_t n = ls_crash_format(buf, sizeof(buf));
    LS_CHECK(n > 0);
    LS_CHECK(contains(buf, "no crash recorded"));
    LS_CHECK(!ls_crash_present());
}

LS_CASE(populated_summary_prints_the_forensic_fields)
{
    /* Every field on the summary is here so a laptop later can resolve
       the addresses against the ELF. Task name, PC, RA, cause, fault
       address, ELF sha and the reason string all have to show up - the
       "reboot happened at these addresses" story only works if the
       reader gets them. */
    reset();
    s_stub.present     = true;
    strcpy(s_stub.task, "p25_rx");
    s_stub.pc          = 0x40800abc;
    s_stub.ra          = 0x40800def;
    s_stub.fault_code  = 0x00000007;   /* mcause: store access fault */
    s_stub.fault_addr  = 0xdeadbeef;
    s_stub.core_dump_version = 0x00010500;
    strcpy(s_stub.reason, "Store access fault");
    strcpy(s_stub.elf_sha, "aa53b1c9de204f76");

    ls_crash_init(&STUB_SRC, "2026-09-06 05:57:33");
    LS_CHECK(ls_crash_present());

    char buf[768];
    size_t n = ls_crash_format(buf, sizeof(buf));
    LS_CHECK(n > 0);
    LS_CHECK(contains(buf, "p25_rx"));
    LS_CHECK(contains(buf, "0x40800abc"));
    LS_CHECK(contains(buf, "0x40800def"));
    LS_CHECK(contains(buf, "0x00000007"));
    LS_CHECK(contains(buf, "0xdeadbeef"));
    LS_CHECK(contains(buf, "aa53b1c9de204f76"));
    LS_CHECK(contains(buf, "Store access fault"));
    /* Running-firmware id is context, not a claim about the crashed
       image - the elf-sha in the report is what identifies the ELF. */
    LS_CHECK(contains(buf, "2026-09-06 05:57:33"));
}

LS_CASE(zero_depth_backtrace_says_so_rather_than_going_silent)
{
    /* RISC-V P4 has no on-device unwind: the SDK ships a raw stackdump
       and the summary comes back with depth==0. Users typing `crash`
       must not see an empty line where the backtrace should be, or the
       report reads as if the unwind quietly succeeded and found nothing. */
    reset();
    s_stub.present = true;
    strcpy(s_stub.task, "main");
    s_stub.pc = 0x40800abc;
    s_stub.bt_depth = 0;
    s_stub.bt_corrupted = false;

    ls_crash_init(&STUB_SRC, NULL);

    char buf[512];
    ls_crash_format(buf, sizeof(buf));
    LS_CHECK(contains(buf, "backtrace:"));
    LS_CHECK(contains(buf, "(none captured)"));
}

LS_CASE(corrupted_backtrace_is_flagged)
{
    /* Xtensa can partially unwind and set corrupted=true. A partial list
       is still useful (top frames usually are), but the corruption flag
       has to travel to the reader or a stale-stack backtrace reads like
       a good one. */
    reset();
    s_stub.present = true;
    strcpy(s_stub.task, "adsb");
    s_stub.pc = 0x40001000;
    s_stub.bt_depth = 3;
    s_stub.bt[0] = 0x40001000;
    s_stub.bt[1] = 0x40002000;
    s_stub.bt[2] = 0x40003000;
    s_stub.bt_corrupted = true;

    ls_crash_init(&STUB_SRC, NULL);

    char buf[512];
    ls_crash_format(buf, sizeof(buf));
    LS_CHECK(contains(buf, "0x40001000"));
    LS_CHECK(contains(buf, "0x40002000"));
    LS_CHECK(contains(buf, "0x40003000"));
    LS_CHECK(contains(buf, "corrupted"));
}

LS_CASE(boot_notice_present_only_when_a_dump_is_stored)
{
    /* Two halves of the same guarantee: the notice must show on the boot
       after a crash, and must fall silent again after `crash clear`.
       Skipping either half leaves the user either uninformed or annoyed
       forever by a stale dump they already read. */
    reset();
    ls_crash_init(&STUB_SRC, NULL);

    char notice[192];
    LS_EQ_INT(ls_crash_boot_notice(notice, sizeof(notice)), 0);
    LS_EQ_STR(notice, "");

    /* Now simulate a crashed boot. */
    reset();
    s_stub.present = true;
    strcpy(s_stub.task, "irq");
    s_stub.pc = 0x40200000;
    ls_crash_init(&STUB_SRC, NULL);
    LS_CHECK(ls_crash_present());

    size_t n = ls_crash_boot_notice(notice, sizeof(notice));
    LS_CHECK(n > 0);
    LS_CHECK(contains(notice, "crash"));
    LS_CHECK(contains(notice, "'crash clear'"));

    /* Erase and confirm the notice goes silent without a re-read. */
    LS_CHECK(ls_crash_erase());
    LS_CHECK(!ls_crash_present());
    LS_EQ_INT(ls_crash_boot_notice(notice, sizeof(notice)), 0);
}

LS_CASE(null_source_is_treated_as_no_dump)
{
    /* Defensive: if a future refactor forgets to install a source, the
       formatter must still print something sensible instead of dereferencing
       a NULL fn pointer. Same as the read-fail case - it's "no dump". */
    ls_crash_init(NULL, NULL);
    LS_CHECK(!ls_crash_present());

    char buf[128];
    ls_crash_format(buf, sizeof(buf));
    LS_CHECK(contains(buf, "no crash recorded"));

    /* Erase against a NULL source is a no-op, not a failure - the caller
       already got what they wanted (no stored dump). */
    LS_CHECK(ls_crash_erase());
}

LS_CASE(format_never_overflows_a_tight_buffer)
{
    /* The device console runs the report on the REPL task's 4KB stack,
       so buffers cannot grow without bound. Point a formatter at a small
       buffer, populate everything, and confirm it truncates cleanly and
       leaves a terminator - the alternative is a torn string that reads
       past the end into whoever's stack this is. */
    reset();
    s_stub.present = true;
    strcpy(s_stub.task, "aaaaaaaaaaaaaaa");
    s_stub.pc = 0x11111111; s_stub.ra = 0x22222222;
    s_stub.fault_code = 0x33333333; s_stub.fault_addr = 0x44444444;
    strcpy(s_stub.reason, "long reason string that will not fit into a tight buffer");
    strcpy(s_stub.elf_sha, "0123456789abcdef");
    s_stub.bt_depth = LS_CRASH_MAX_BT;
    for (unsigned i = 0; i < LS_CRASH_MAX_BT; i++) s_stub.bt[i] = 0x40000000u + i;

    ls_crash_init(&STUB_SRC, "2026-09-06 05:57:33");

    char tight[48];
    memset(tight, 0x7f, sizeof(tight));
    size_t n = ls_crash_format(tight, sizeof(tight));
    LS_CHECK(n < sizeof(tight));
    LS_EQ_INT(tight[sizeof(tight) - 1], '\0');
}
