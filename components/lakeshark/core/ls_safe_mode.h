#ifndef LS_SAFE_MODE_H
#define LS_SAFE_MODE_H

/* Early safe mode: decide whether this boot is allowed to start the radio at all, BEFORE anything that can fault does. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LS_SAFE_MAGIC        UINT32_C(0x4C534D31)   /* "LSM1" */
#define LS_SAFE_VERSION      UINT16_C(1)
#define LS_SAFE_APP_MAX      24

#define LS_SAFE_FAULT_LIMIT  UINT32_C(3)

/* How long a normal boot must survive before it is called healthy. This is
   deliberately well past the point where the display, the backend, the radio
   session and the first app have all run: marking healthy at the end of
   app_main would clear the counter for exactly the crashes that happen a few
   seconds later, which is most of them. */
#define LS_SAFE_HEALTHY_MS   UINT32_C(20000)

/* Safe mode never waits forever for LVGL. A failed/timed-out attempt is
   retained before entering the BSP, so an abort or watchdog reset makes the
   next safe boot serial-only instead of repeating the same panel path. */
#define LS_SAFE_DISPLAY_LOCK_MS UINT32_C(2000)

#define LS_SAFE_REPORT_MAX   512

/* Reset causes. The values match esp_reset_reason_t so the mapping in
   ls_safe_mode_esp.c is a readable switch and this header stays free of the
   SDK - but nothing here depends on the numbering. */
typedef enum {
    LS_SAFE_RESET_UNKNOWN    = 0,
    LS_SAFE_RESET_POWERON    = 1,
    LS_SAFE_RESET_EXT        = 2,
    LS_SAFE_RESET_SW         = 3,
    LS_SAFE_RESET_PANIC      = 4,
    LS_SAFE_RESET_INT_WDT    = 5,
    LS_SAFE_RESET_TASK_WDT   = 6,
    LS_SAFE_RESET_WDT        = 7,
    LS_SAFE_RESET_DEEPSLEEP  = 8,
    LS_SAFE_RESET_BROWNOUT   = 9,
    LS_SAFE_RESET_SDIO       = 10,
    LS_SAFE_RESET_USB        = 11,
    LS_SAFE_RESET_JTAG       = 12,
    LS_SAFE_RESET_EFUSE      = 13,
    LS_SAFE_RESET_PWR_GLITCH = 14,
    LS_SAFE_RESET_CPU_LOCKUP = 15,
    LS_SAFE_RESET__COUNT
} ls_safe_reset_t;

typedef enum {
    /* Somebody or something asked for this reset. Not evidence of a defect. */
    LS_SAFE_CLASS_CLEAN = 0,
    /* The firmware died: panic, watchdog, lockup. */
    LS_SAFE_CLASS_FAULT,

    LS_SAFE_CLASS_POWER,
    /* The chip will not say. Counted as a failed start when one was in
       flight, because escaping a reboot loop matters more than attribution -
       but the report prints the reason as unknown, never as a panic. */
    LS_SAFE_CLASS_UNKNOWN
} ls_safe_class_t;

/* How far the boot sequence got. Recorded as each stage begins, so a boot
   that never returns leaves behind the name of the thing it was starting. */
typedef enum {
    LS_SAFE_STAGE_NONE = 0,
    LS_SAFE_STAGE_ENTRY,
    LS_SAFE_STAGE_C6,
    LS_SAFE_STAGE_NVS,
    LS_SAFE_STAGE_STORAGE,
    LS_SAFE_STAGE_CODEC,
    LS_SAFE_STAGE_DISPLAY,
    LS_SAFE_STAGE_SHELL,
    LS_SAFE_STAGE_BACKEND,
    LS_SAFE_STAGE_APPS,
    LS_SAFE_STAGE_RUNNING,
    LS_SAFE_STAGE__COUNT
} ls_safe_stage_t;

typedef enum {
    LS_SAFE_ENTRY_NONE = 0,        /* normal boot */
    LS_SAFE_ENTRY_REPEATED_FAULT,  /* the limit of failed starts was reached */
    LS_SAFE_ENTRY_RETRY_FAILED,    /* a deliberate "try normal boot" faulted */
    LS_SAFE_ENTRY_FORCED
} ls_safe_entry_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    uint32_t faults;      /* consecutive failed normal starts */
    uint32_t power_events;/* consecutive brownout / glitch resets */
    uint8_t  armed;       /* a normal start is in flight and not yet healthy */
    uint8_t  forced;      /* boot safe until explicitly told otherwise */
    uint8_t  retry;       /* this normal boot is a deliberate retry */
    uint8_t  in_safe;     /* this boot is running in safe mode */
    uint8_t  stage;       /* stage this boot reached */
    uint8_t  prev_stage;  /* stage the previous boot reached */
    uint8_t  safe_display_failed; /* prior safe-display attempt did not finish */
    uint8_t  reserved;
    char     app[LS_SAFE_APP_MAX];  /* last app the shell opened, "" if none */
    uint32_t crc;
} ls_safe_state_t;

/* What ls_safe_decide() concluded. A snapshot: the state struct is rewritten
   for the boot that is now running, this is what it was. */
typedef struct {
    bool            safe;
    ls_safe_entry_t entry;
    ls_safe_reset_t reset;
    ls_safe_class_t reset_class;
    uint32_t        faults;        /* after this boot's accounting */
    uint32_t        power_events;
    uint8_t         prev_stage;
    bool            marker_corrupt;/* the retained record failed validation */
    bool            prev_was_safe;
    char            app[LS_SAFE_APP_MAX];
} ls_safe_boot_t;

typedef struct {
    bool display;            /* panel + LVGL */
    bool touch;              /* the same init; named so the contract is explicit */
    bool console;            /* serial CLI */
    bool full_console;       /* the normal command set, which dereferences backends */
    bool c6;                 /* esp_hosted co-processor probe */
    bool ble;
    bool nvs;                /* nvs_flash_init */
    bool storage_autorepair; /* nvs_flash_erase() on a corrupt partition */
    bool storage_mount;      /* SPIFFS + SD */
    bool codec;              /* audio codec / I2S */
    bool boot_splash;
    bool boot_sound;
    bool backend;            /* radio */
    bool apps;               /* app shell and app registration */
    bool restore_last_app;
    bool coredump_parse;     /* automatic summary parse: never, in either mode */
} ls_safe_boot_plan_t;

/* ---- retained record -------------------------------------------------- */

uint32_t ls_safe_state_crc(const ls_safe_state_t *st);
bool     ls_safe_state_valid(const ls_safe_state_t *st);
void     ls_safe_state_seal(ls_safe_state_t *st);
/* Zero to a defined, sealed, "nothing known" record. */
void     ls_safe_state_reset(ls_safe_state_t *st);

/* ---- classification --------------------------------------------------- */

ls_safe_class_t ls_safe_classify(ls_safe_reset_t reset);
const char *ls_safe_reset_name(ls_safe_reset_t reset);
const char *ls_safe_class_name(ls_safe_class_t cls);
const char *ls_safe_stage_name(ls_safe_stage_t stage);
const char *ls_safe_entry_name(ls_safe_entry_t entry);

/* ---- the decision ----------------------------------------------------- */

void ls_safe_decide(ls_safe_state_t *st, ls_safe_reset_t reset,
                    ls_safe_boot_t *out);

/* True once a normal boot has survived long enough to be trusted. */
bool ls_safe_healthy_due(uint32_t uptime_ms);

/* Clear the failure accounting. Only call when healthy_due() is satisfied. */
void ls_safe_mark_healthy(ls_safe_state_t *st);

/* TRY NORMAL BOOT. Clears the forced flag and leaves the counter one short of
   the limit, so the retry is protected: if it faults, the very next boot is
   safe again and reports LS_SAFE_ENTRY_RETRY_FAILED. */
void ls_safe_request_retry(ls_safe_state_t *st);
/* STAY SAFE. */
void ls_safe_request_stay(ls_safe_state_t *st);
/* Manual entry/exit from the console. */
void ls_safe_request_force(ls_safe_state_t *st, bool on);

void ls_safe_note_stage(ls_safe_state_t *st, ls_safe_stage_t stage);
void ls_safe_note_app(ls_safe_state_t *st, const char *name);

/* Safe-display one-shot policy. begin() latches failure before calling the
   BSP; complete(true) clears it only after the recovery screen is painted.
   An explicit normal-boot retry clears the latch and permits one later safe
   display attempt if that retry faults. */
bool ls_safe_display_should_attempt(const ls_safe_state_t *st);
void ls_safe_display_begin(ls_safe_state_t *st);
void ls_safe_display_complete(ls_safe_state_t *st, bool painted);

/* ---- what may start --------------------------------------------------- */

void ls_safe_boot_plan(bool safe, ls_safe_boot_plan_t *out);

/* ---- report ----------------------------------------------------------- */

/* dump_state: -1 not safely known, 0 none stored, 1 one is stored. Nothing
   here parses a dump; a stored image is read only by an explicit `crash`. */
#define LS_SAFE_DUMP_UNKNOWN  (-1)
#define LS_SAFE_DUMP_NONE     (0)
#define LS_SAFE_DUMP_PRESENT  (1)

/* Bounded plain-text report. Always NUL-terminates when buflen > 0; returns
   the number of bytes written, not counting the terminator. identity is the
   firmware/build line and may be NULL. */
size_t ls_safe_format(char *buf, size_t buflen, const ls_safe_boot_t *boot,
                      const char *identity, int dump_state);

/* ---- device-only glue (ls_safe_mode_esp.c) ---------------------------- */

/* Runs ls_safe_decide() against the RTC-retained record and esp_reset_reason().
   Idempotent: later calls return the same snapshot. */
const ls_safe_boot_t *ls_safe_boot_begin(void);
const ls_safe_boot_t *ls_safe_boot_result(void);
bool   ls_safe_active(void);
void   ls_safe_stage(ls_safe_stage_t stage);
void   ls_safe_app(const char *name);
bool   ls_safe_display_attempt(void);
void   ls_safe_display_done(bool painted);
/* Arm the one-shot that marks this boot healthy after LS_SAFE_HEALTHY_MS.
   Only meaningful on a normal boot; a no-op in safe mode. */
void   ls_safe_healthy_arm(void);
/* Record what the presence-only coredump probe found, so the report can say
   whether a dump is waiting without anything having parsed one. */
void   ls_safe_note_dump(int dump_state);
size_t ls_safe_report(char *buf, size_t buflen);
/* Both reboot. retry re-arms the protection; stay comes back to safe mode. */
void   ls_safe_retry_normal_boot(void);
void   ls_safe_stay_safe(void);
void   ls_safe_force(bool on);
bool   ls_safe_forced(void);

#ifdef __cplusplus
}
#endif
#endif
