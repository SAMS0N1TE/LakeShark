#ifndef LS_C6_CORE_H
#define LS_C6_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The two slots in c6_firmware/partition-table.bin. OTA cannot change this
 * layout. Only provisioned T-Display-P4 boards with that layout are supported. */
#define LS_C6_SLOT_SIZE 0x1e0000u
/* Match Hosted's host_performs_slave_ota example (fits one transport frame). */
#define LS_C6_CHUNK 1400u
#define LS_C6_DEFAULT_PATH "/sdcard/lakeshark/c6/network_adapter.bin"

typedef struct { char project[32], version[32]; } ls_c6_image_t;
typedef int (*ls_c6_hash_fn)(const uint8_t *, size_t, uint8_t digest[32]);
const char *ls_c6_validate(const uint8_t *data, size_t size, size_t slot,
                          ls_c6_hash_fn hash, ls_c6_image_t *image);
bool ls_c6_version_allowed(const char *candidate, const char *running, bool force);

typedef enum {
    LS_C6_IDLE = -1, /* read-only preflight complete / no install underway */
    LS_C6_VALIDATE = 0, LS_C6_STOP, LS_C6_BEGIN, LS_C6_WRITE, LS_C6_END,
    LS_C6_ACTIVATE, LS_C6_WAIT, LS_C6_DONE, LS_C6_FAILED
} ls_c6_stage_t;
typedef enum { LS_C6_OLD_ACTIVE, LS_C6_BOOT_UNKNOWN, LS_C6_ACTIVATED } ls_c6_boot_t;
typedef struct {
    ls_c6_stage_t stage;
    ls_c6_boot_t boot;
    size_t sent, size;
    int error;
    char old_version[32], new_version[32], message[160];
} ls_c6_result_t;
typedef struct {
    void *ctx;
    int (*power)(void *); /* refuse low battery / failed exposed gauge */
    int (*version)(void *, char out[32]);
    int (*stop)(void *);  /* latch users off until P4 reboot, including on failure */
    int (*begin)(void *);
    int (*write)(void *, const uint8_t *, size_t);
    int (*end)(void *);
    int (*activate)(void *);
    /* Must observe a NEW coprocessor boot, then query version; bounded. */
    int (*wait)(void *, unsigned timeout_ms, char out[32]);
    uint64_t (*millis)(void *);
    void (*report)(void *, const ls_c6_result_t *);
} ls_c6_transport_t;

/* Runs on a worker. data is an immutable, validated snapshot, owned by caller.
 * No retry/abort RPC exists in Hosted 2.12.9. Never activate on a failed end. */
bool ls_c6_run(const uint8_t *data, size_t size, bool force, ls_c6_hash_fn hash,
               const ls_c6_transport_t *io, ls_c6_result_t *result);
#endif
