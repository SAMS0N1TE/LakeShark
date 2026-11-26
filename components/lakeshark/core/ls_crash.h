#ifndef LS_CRASH_H
#define LS_CRASH_H

/* read the coredump summary back on the device it crashed on.
   partitions_16m.csv / partitions_32m.csv both carry a coredump slot and
   sdkconfig.defaults enables ESP_COREDUMP_ENABLE_TO_FLASH, so every board
   in bench/configs.json has been *writing* a dump on panic since -
   but nothing in the firmware ever read one back. In the field a crash was
   "it rebooted" until a laptop with esptool and the matching ELF got
   involved. The SDK call lives behind ls_crash_source_t so the bench can
   drive the formatter without a chip. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LS_CRASH_MAX_BT       40   /* a stack scan passes stale frames first */
#define LS_CRASH_ELF_HEX_LEN  16   /* first N hex chars of app_elf_sha256[] */
#define LS_CRASH_REASON_MAX   128
#define LS_CRASH_TASK_MAX     16

typedef struct {
    bool     present;                     /* false => no dump is stored */
    char     task[LS_CRASH_TASK_MAX];     /* task that faulted (may be empty) */
    uint32_t pc;                          /* faulting PC / mepc */
    uint32_t ra;                          /* return address, RISC-V (0 if n/a) */
    uint32_t fault_code;                  /* mcause on RV, exccause on Xtensa */
    uint32_t fault_addr;                  /* mtval / excvaddr */
    uint32_t core_dump_version;
    char     reason[LS_CRASH_REASON_MAX]; /* panic reason, empty if none */
    char     elf_sha[LS_CRASH_ELF_HEX_LEN + 1]; /* short hex of app_elf_sha256 */
    uint32_t bt[LS_CRASH_MAX_BT];         /* backtrace, zero-padded */
    uint8_t  bt_depth;                    /* valid entries in bt[] */
    bool     bt_corrupted;                /* backtrace could not be fully unwound */
    uint32_t partition_address, partition_size;
    uint32_t declared_size; /* untrusted; never used as a read length */
    bool header_bounds_ok; /* size only, NOT ELF/checksum validation */
    bool bt_scanned;       /* bt[] is a stack scan (ls_crash_parse_elf), not an unwind */
    const char *parse_why; /* why a stored dump was not parsed; NULL if it was */
} ls_crash_summary_t;

typedef enum {
    LS_CRASH_READ_OK = 0,
    LS_CRASH_READ_NOT_FOUND,
    LS_CRASH_READ_UNAVAILABLE,
    LS_CRASH_READ_QUARANTINED,
    LS_CRASH_READ_METADATA,
} ls_crash_read_result_t;

/* Inspect only fixed 20-byte envelope; unknown versions stay raw metadata.
   Caller supplies partition_address/size. No pointer chasing or writes. */
ls_crash_read_result_t ls_crash_inspect_header(const uint8_t *header,
                                             size_t len, ls_crash_summary_t *out);

/* A bounded read of the stored image: false on any error or out of range. */
typedef bool (*ls_crash_read_at_fn)(void *ctx, uint32_t offset, void *buf, uint32_t len);
/* Addresses [lo, hi) that can hold code in the crashed image. */
typedef struct { uint32_t lo, hi; } ls_crash_range_t;

/* Read the crash itself out of an IDF ELF core dump with a CRC32 checksum
   (version 0x....0102): the faulting task, its pc/ra, the panic reason, the
   crashed image's ELF sha, and the code addresses found on the crashed task's
   stack (bt[], bt_scanned) - enough to decode offline with addr2line and the
   matching ELF. Never the IDF parser (it faulted once and overwrote the dump):
   every read is bounded by the header's size, which must fit the partition,
   and the checksum is checked over the whole image before anything is
   believed. Only addresses leave the dump - no memory contents, so nothing a
   task held (keys, Wi-Fi credentials) can reach a report.
   *out must hold ls_crash_inspect_header's metadata. Returns LS_CRASH_READ_OK
   with the summary filled, or LS_CRASH_READ_METADATA with parse_why set and
   the metadata kept. Allocates two small buffers for the duration. */
ls_crash_read_result_t ls_crash_parse_elf(ls_crash_read_at_fn read_at, void *ctx,
                                          const ls_crash_range_t *code, size_t n_code,
                                          ls_crash_summary_t *out);

/* Source of coredump data. The device wires these to esp_core_dump_*; the
   bench supplies its own so the formatter is tested, not the SDK. */
typedef struct {
    /* Presence-only probe. This must not parse the stored image: init, boot
       notices and GUI state are allowed to call it automatically. Return
       true for an invalid or otherwise unreadable stored image too; only a
       known-empty slot is false. */
    bool (*present)(void);
    /* Bounded diagnostic read. Never invoke an unsafe platform parser.
       *out is valid for READ_OK and READ_METADATA. */
    ls_crash_read_result_t (*read)(ls_crash_summary_t *out);
    /* Erase the stored dump. Return true on success, and true when there
       was nothing to erase. */
    bool (*erase)(void);
} ls_crash_source_t;

/* Install the source and the current firmware identifier. Only the source's
   non-parsing present() probe is called; initialization never reads a summary.
   running_id is
   what the report prints as "running now" for context (crashed image is
   identified by its own elf_sha). May be NULL. Safe to call before any
   other ls_crash_* function; a NULL source is treated as "no dump". */
void ls_crash_init(const ls_crash_source_t *src, const char *running_id);

/* Format safe diagnostic data into buf. Device sources report raw metadata,
   not parsed ELF summaries. Always NUL-terminates.
   Returns the number of bytes written (not counting the terminator).

   The empty case ("no crash recorded") is formatted here too, so the
   console command has one entry point and never has to know the state. */
size_t ls_crash_format(char *buf, size_t buflen);

/* Erase the stored dump. Returns true on success (or if there was none). */
bool ls_crash_erase(void);

/* Whether a dump is currently stored. Cached: refreshes on init and on
   erase, not on every call - this is fine for the boot notice, and the
   `crash` command re-reads through the source anyway. */
bool ls_crash_present(void);

/* One-line boot notice, suitable for a single log line or printf. Writes at
   most buflen bytes (always NUL-terminated) and returns the length written,
   or 0 if there is nothing to say. */
size_t ls_crash_boot_notice(char *buf, size_t buflen);

/* Device-only helper (defined in ls_crash_esp.c). Wires the SDK-backed
   source, tags the running firmware, and emits the boot notice. Call from
   every app_main path once nvs_flash_init has settled. */
void ls_crash_boot_setup(void);

#ifdef __cplusplus
}
#endif
#endif
