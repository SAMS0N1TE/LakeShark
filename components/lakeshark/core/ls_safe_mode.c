/* The pure half of early safe mode: validation of the retained
   record, reset classification, the failed-start accounting, the healthy
   interval, the boot plan and the report. No SDK calls - see ls_safe_mode.h
   for why the state is caller-owned. */

#include "ls_safe_mode.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------ record ---- */

/* CRC-32 (reflected, 0xEDB88320) computed a nibble at a time from a 16-entry
   table. A byte-wise 256-entry table is not worth 1 KB of .rodata for a
   96-byte record, and a bitwise loop over the same record runs at boot on a
   400 MHz core in well under a microsecond either way. */
static uint32_t crc32(const uint8_t *p, size_t n)
{
    static const uint32_t tab[16] = {
        0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu,
        0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
        0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu,
        0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu
    };
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        c = (c >> 4) ^ tab[c & 0x0Fu];
        c = (c >> 4) ^ tab[c & 0x0Fu];
    }
    return ~c;
}

uint32_t ls_safe_state_crc(const ls_safe_state_t *st)
{
    if (!st) return 0;
    return crc32((const uint8_t *)st, offsetof(ls_safe_state_t, crc));
}

bool ls_safe_state_valid(const ls_safe_state_t *st)
{
    if (!st) return false;
    if (st->magic   != LS_SAFE_MAGIC)          return false;
    if (st->version != LS_SAFE_VERSION)        return false;
    if (st->size    != sizeof(ls_safe_state_t)) return false;
    if (st->crc     != ls_safe_state_crc(st))  return false;

    if (st->stage      >= LS_SAFE_STAGE__COUNT) return false;
    if (st->prev_stage >= LS_SAFE_STAGE__COUNT) return false;
    if (st->safe_display_failed > 1)             return false;
    /* The app name must be a NUL-terminated string inside its field or the
       report would run off the end of the record. */
    if (st->app[LS_SAFE_APP_MAX - 1] != '\0')   return false;
    return true;
}

void ls_safe_state_seal(ls_safe_state_t *st)
{
    if (!st) return;
    st->magic   = LS_SAFE_MAGIC;
    st->version = LS_SAFE_VERSION;
    st->size    = (uint16_t)sizeof(ls_safe_state_t);
    st->app[LS_SAFE_APP_MAX - 1] = '\0';
    st->crc     = ls_safe_state_crc(st);
}

void ls_safe_state_reset(ls_safe_state_t *st)
{
    if (!st) return;
    memset(st, 0, sizeof(*st));
    ls_safe_state_seal(st);
}

/* ------------------------------------------------------ classification -- */

ls_safe_class_t ls_safe_classify(ls_safe_reset_t reset)
{
    switch (reset) {
    case LS_SAFE_RESET_POWERON:
    case LS_SAFE_RESET_EXT:
    case LS_SAFE_RESET_SW:
    case LS_SAFE_RESET_DEEPSLEEP:
    case LS_SAFE_RESET_SDIO:
    case LS_SAFE_RESET_USB:
    case LS_SAFE_RESET_JTAG:
    case LS_SAFE_RESET_EFUSE:
        return LS_SAFE_CLASS_CLEAN;

    case LS_SAFE_RESET_PANIC:
    case LS_SAFE_RESET_INT_WDT:
    case LS_SAFE_RESET_TASK_WDT:
    case LS_SAFE_RESET_WDT:
    case LS_SAFE_RESET_CPU_LOCKUP:
        return LS_SAFE_CLASS_FAULT;

    case LS_SAFE_RESET_BROWNOUT:
    case LS_SAFE_RESET_PWR_GLITCH:
        return LS_SAFE_CLASS_POWER;

    default:
        return LS_SAFE_CLASS_UNKNOWN;
    }
}

const char *ls_safe_reset_name(ls_safe_reset_t reset)
{
    switch (reset) {
    case LS_SAFE_RESET_POWERON:    return "power-on";
    case LS_SAFE_RESET_EXT:        return "external reset";
    case LS_SAFE_RESET_SW:         return "software restart";
    case LS_SAFE_RESET_PANIC:      return "panic";
    case LS_SAFE_RESET_INT_WDT:    return "interrupt watchdog";
    case LS_SAFE_RESET_TASK_WDT:   return "task watchdog";
    case LS_SAFE_RESET_WDT:        return "watchdog";
    case LS_SAFE_RESET_DEEPSLEEP:  return "deep sleep wake";
    case LS_SAFE_RESET_BROWNOUT:   return "brownout";
    case LS_SAFE_RESET_SDIO:       return "SDIO reset";
    case LS_SAFE_RESET_USB:        return "USB reset";
    case LS_SAFE_RESET_JTAG:       return "JTAG reset";
    case LS_SAFE_RESET_EFUSE:      return "eFuse error";
    case LS_SAFE_RESET_PWR_GLITCH: return "power glitch";
    case LS_SAFE_RESET_CPU_LOCKUP: return "CPU lockup";
    default:                       return "unknown";
    }
}

const char *ls_safe_class_name(ls_safe_class_t cls)
{
    switch (cls) {
    case LS_SAFE_CLASS_CLEAN: return "deliberate";
    case LS_SAFE_CLASS_FAULT: return "firmware fault";
    case LS_SAFE_CLASS_POWER: return "power";
    default:                  return "unattributed";
    }
}

const char *ls_safe_stage_name(ls_safe_stage_t stage)
{
    switch (stage) {
    case LS_SAFE_STAGE_ENTRY:   return "entry";
    case LS_SAFE_STAGE_C6:      return "C6 co-processor";
    case LS_SAFE_STAGE_NVS:     return "NVS";
    case LS_SAFE_STAGE_STORAGE: return "SPIFFS/SD";
    case LS_SAFE_STAGE_CODEC:   return "audio codec";
    case LS_SAFE_STAGE_DISPLAY: return "display";
    case LS_SAFE_STAGE_SHELL:   return "shell";
    case LS_SAFE_STAGE_BACKEND: return "radio backend";
    case LS_SAFE_STAGE_APPS:    return "app start";
    case LS_SAFE_STAGE_RUNNING: return "running";
    default:                    return "none";
    }
}

const char *ls_safe_entry_name(ls_safe_entry_t entry)
{
    switch (entry) {
    case LS_SAFE_ENTRY_REPEATED_FAULT: return "repeated startup faults";
    case LS_SAFE_ENTRY_RETRY_FAILED:   return "the normal-boot retry faulted";
    case LS_SAFE_ENTRY_FORCED:         return "requested by the operator";
    default:                           return "normal boot";
    }
}

/* ------------------------------------------------------------ decide ---- */

void ls_safe_decide(ls_safe_state_t *st, ls_safe_reset_t reset,
                    ls_safe_boot_t *out)
{
    ls_safe_boot_t local;
    if (!out) out = &local;
    memset(out, 0, sizeof(*out));
    if (!st) return;

    const bool corrupt = !ls_safe_state_valid(st);
    if (corrupt) ls_safe_state_reset(st);

    const ls_safe_class_t cls = ls_safe_classify(reset);

    const uint8_t reached    = st->stage;
    const bool    prev_safe  = st->in_safe != 0;
    const bool    was_armed  = st->armed != 0;
    const bool    was_retry  = st->retry != 0;

    uint32_t faults = st->faults;
    uint32_t power  = st->power_events;

    /* Only a start that was in flight and did not survive counts. A reflash,
       the reset button, `esp_restart()` from the USB-dongle recovery path or
       a clean power cycle all land here too, and counting those was what made
       the guard cry wolf. */
    bool counted_fault = false;
    if (cls == LS_SAFE_CLASS_POWER) {
        power++;
    } else if (was_armed &&
               (cls == LS_SAFE_CLASS_FAULT || cls == LS_SAFE_CLASS_UNKNOWN)) {
        faults++;
        counted_fault = true;
    }

    ls_safe_entry_t entry = LS_SAFE_ENTRY_NONE;
    if (faults >= LS_SAFE_FAULT_LIMIT) {
        entry = (was_retry && counted_fault) ? LS_SAFE_ENTRY_RETRY_FAILED
                                             : LS_SAFE_ENTRY_REPEATED_FAULT;
    }

    if (st->forced) entry = LS_SAFE_ENTRY_FORCED;

    const bool safe = (entry != LS_SAFE_ENTRY_NONE);

    out->safe           = safe;
    out->entry          = entry;
    out->reset          = reset;
    out->reset_class    = cls;
    out->faults         = faults;
    out->power_events   = power;
    out->prev_stage     = reached;
    out->marker_corrupt = corrupt;
    out->prev_was_safe  = prev_safe;
    memcpy(out->app, st->app, LS_SAFE_APP_MAX);
    out->app[LS_SAFE_APP_MAX - 1] = '\0';

    st->faults       = faults;
    st->power_events = power;
    st->prev_stage   = reached;
    st->stage        = LS_SAFE_STAGE_ENTRY;
    st->in_safe      = safe ? 1 : 0;
    /* Safe mode starts nothing that can fault, so there is nothing to arm.
       Arming it would also let a brownout in safe mode look like a failed
       start. */
    st->armed        = safe ? 0 : 1;
    /* The retry flag lives until the boot it protects either reaches healthy
       or faults. Clearing it here would lose exactly the fact that makes
       LS_SAFE_ENTRY_RETRY_FAILED reportable. */
    st->retry        = safe ? 0 : (was_retry ? 1 : 0);
    ls_safe_state_seal(st);
}

bool ls_safe_healthy_due(uint32_t uptime_ms)
{
    return uptime_ms >= LS_SAFE_HEALTHY_MS;
}

void ls_safe_mark_healthy(ls_safe_state_t *st)
{
    if (!st) return;
    if (!ls_safe_state_valid(st)) ls_safe_state_reset(st);
    st->armed        = 0;
    st->retry        = 0;
    st->faults       = 0;
    st->power_events = 0;

    ls_safe_state_seal(st);
}

void ls_safe_request_retry(ls_safe_state_t *st)
{
    if (!st) return;
    if (!ls_safe_state_valid(st)) ls_safe_state_reset(st);
    st->forced = 0;
    st->retry  = 1;
    st->armed  = 0;
    st->faults = LS_SAFE_FAULT_LIMIT - 1;
    /* this is the only automatic re-enable after a failed safe-mode
       panel attempt. A normal retry remains explicit, and if it faults the
       following safe boot gets one fresh display attempt before falling back
       to retained serial-only mode again. */
    st->safe_display_failed = 0;
    ls_safe_state_seal(st);
}

void ls_safe_request_stay(ls_safe_state_t *st)
{
    ls_safe_request_force(st, true);
}

void ls_safe_request_force(ls_safe_state_t *st, bool on)
{
    if (!st) return;
    if (!ls_safe_state_valid(st)) ls_safe_state_reset(st);
    st->forced = on ? 1 : 0;
    if (!on) {
        /* Leaving safe mode on purpose is also a statement that the counter
           is stale; otherwise the next boot walks straight back in. */
        st->faults       = 0;
        st->power_events = 0;
        st->retry        = 0;
    }
    ls_safe_state_seal(st);
}

void ls_safe_note_stage(ls_safe_state_t *st, ls_safe_stage_t stage)
{
    if (!st) return;
    if (stage >= LS_SAFE_STAGE__COUNT) return;
    if (!ls_safe_state_valid(st)) ls_safe_state_reset(st);
    st->stage = (uint8_t)stage;
    ls_safe_state_seal(st);
}

void ls_safe_note_app(ls_safe_state_t *st, const char *name)
{
    if (!st) return;
    if (!ls_safe_state_valid(st)) ls_safe_state_reset(st);
    if (!name) name = "";
    strncpy(st->app, name, LS_SAFE_APP_MAX - 1);
    st->app[LS_SAFE_APP_MAX - 1] = '\0';
    ls_safe_state_seal(st);
}

/* ------------------------------------------------ safe-mode display ---- */

bool ls_safe_display_should_attempt(const ls_safe_state_t *st)
{
    return ls_safe_state_valid(st) && st->safe_display_failed == 0;
}

void ls_safe_display_begin(ls_safe_state_t *st)
{
    if (!st) return;
    if (!ls_safe_state_valid(st)) ls_safe_state_reset(st);
    /* seal the failure latch before entering the configured Waveshare
       BSP. CONFIG_BSP_ERROR_CHECK turns its nominal NULL returns into aborts,
       and lower LCD calls can block; after either kind of reset the next safe
       boot must skip the panel path and leave the recovery CLI usable. */
    st->safe_display_failed = 1;
    ls_safe_state_seal(st);
}

void ls_safe_display_complete(ls_safe_state_t *st, bool painted)
{
    if (!st || !painted) return;
    if (!ls_safe_state_valid(st)) ls_safe_state_reset(st);
    st->safe_display_failed = 0;
    ls_safe_state_seal(st);
}

/* ------------------------------------------------------------- plan ----- */

void ls_safe_boot_plan(bool safe, ls_safe_boot_plan_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));

    /* Never automatic, in either mode. a checksum-valid stored image
       is not necessarily a parseable one, and the parser faulted on one -
       panic, write a dump, reboot, parse it, panic. Reading a summary stays
       an explicit `crash`. */
    out->coredump_parse = false;

    if (safe) {
        out->display = true;
        out->touch   = true;
        out->console = true;
        return;
    }

    out->display            = true;
    out->touch              = true;
    out->console            = true;
    out->full_console       = true;
    out->c6                 = true;
    out->ble                = true;
    out->nvs                = true;
    out->storage_autorepair = true;
    out->storage_mount      = true;
    out->codec              = true;
    out->boot_splash        = true;
    out->boot_sound         = true;
    out->backend            = true;
    out->apps               = true;
    out->restore_last_app   = true;
}

/* ------------------------------------------------------------ report ---- */

static size_t append(char *buf, size_t buflen, size_t used, const char *fmt, ...)
{
    if (buflen == 0) return 0;
    if (used >= buflen - 1) { buf[buflen - 1] = '\0'; return used; }

    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + used, buflen - used, fmt, ap);
    va_end(ap);
    if (n < 0) { buf[used] = '\0'; return used; }

    size_t added = (size_t)n;
    if (added >= buflen - used) added = buflen - used - 1;
    return used + added;
}

size_t ls_safe_format(char *buf, size_t buflen, const ls_safe_boot_t *boot,
                      const char *identity, int dump_state)
{
    if (!buf || buflen == 0) return 0;
    buf[0] = '\0';
    if (!boot) return 0;

    size_t n = 0;
    n = append(buf, buflen, n, "%s\n",
               boot->safe ? "*** SAFE MODE ***" : "normal boot");
    n = append(buf, buflen, n, "why: %s\n", ls_safe_entry_name(boot->entry));
    if (identity && identity[0])
        n = append(buf, buflen, n, "fw: %s\n", identity);

    n = append(buf, buflen, n, "reset: %s (%s)\n",
               ls_safe_reset_name(boot->reset),
               ls_safe_class_name(boot->reset_class));
    n = append(buf, buflen, n, "last boot reached: %s\n",
               ls_safe_stage_name((ls_safe_stage_t)boot->prev_stage));
    n = append(buf, buflen, n, "failed starts: %lu of %lu\n",
               (unsigned long)boot->faults, (unsigned long)LS_SAFE_FAULT_LIMIT);
    if (boot->power_events)
        n = append(buf, buflen, n,
                   "power resets: %lu  (supply, not counted as a firmware fault)\n",
                   (unsigned long)boot->power_events);
    if (boot->app[0])
        n = append(buf, buflen, n, "last app: %s\n", boot->app);
    if (boot->prev_was_safe)
        n = append(buf, buflen, n, "previous boot was also safe mode\n");
    if (boot->marker_corrupt)
        n = append(buf, buflen, n,
                   "note: retained record was unreadable and has been reset\n");

    if (dump_state == LS_SAFE_DUMP_PRESENT)
        n = append(buf, buflen, n,
                   "coredump: one is stored - read it with 'crash'\n");
    else if (dump_state == LS_SAFE_DUMP_NONE)
        n = append(buf, buflen, n, "coredump: none stored\n");
    else
        n = append(buf, buflen, n, "coredump: not checked\n");

    /* Say plainly that nothing above is a decoded backtrace. The reset reason
       and the stage are observations; the cause is not known here and must
       never be printed as though it were. */
    n = append(buf, buflen, n,
               "no backtrace decoded - the above is the recorded reset reason "
               "and startup stage, not a cause\n");
    return n;
}
