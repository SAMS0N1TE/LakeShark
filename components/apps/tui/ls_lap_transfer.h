/* Hardware-free Web Serial transaction state machine; filesystem/hash policy is injected. */
#ifndef LS_LAP_TRANSFER_H
#define LS_LAP_TRANSFER_H
#include "ls_lap_manifest.h"
#define LS_LAP_CHUNK_BYTES 72u /* 96 base64 chars + command + 10 seq digits <128 */
typedef struct {
    bool (*begin)(void *ctx, const char *name);
    bool (*write)(void *ctx, const uint8_t *bytes, size_t n);
    bool (*finish)(void *ctx, const char *name, size_t size, const uint8_t sha256[32], char *why,
                   size_t cap);
    void (*abort)(void *ctx);
    void *ctx;
} ls_lap_transfer_io_t;
typedef struct {
    ls_lap_transfer_io_t io;
    bool active;
    uint32_t seq, last_ms;
    size_t size, received;
    uint8_t sha256[32];
    char name[41];
} ls_lap_transfer_t;
void ls_lap_transfer_init(ls_lap_transfer_t *s, ls_lap_transfer_io_t io);
/* True means command belonged to put/data/end (including errors). Every reply
 * is one bounded APP OK / APP ERR line; no payload is ever echoed. */
bool ls_lap_transfer_command(ls_lap_transfer_t *s, int argc, const char *const *argv,
                             uint32_t now_ms, char *reply, size_t cap);
bool ls_lap_transfer_expire(ls_lap_transfer_t *s, uint32_t now_ms);
#endif
