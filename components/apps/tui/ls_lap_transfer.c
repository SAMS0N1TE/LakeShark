#include "ls_lap_transfer.h"
#include "core/ls_base64.h"
#include <stdio.h>
#include <string.h>
void ls_lap_transfer_init(ls_lap_transfer_t *s, ls_lap_transfer_io_t io) {
    memset(s, 0, sizeof(*s));
    s->io = io;
}
static void abort_upload(ls_lap_transfer_t *s) {
    s->active = false;
    if (s->io.abort)
        s->io.abort(s->io.ctx);
}
bool ls_lap_transfer_expire(ls_lap_transfer_t *s, uint32_t ms) {
    if (s->active && (uint32_t)(ms - s->last_ms) >= 30000) {
        abort_upload(s);
        return true;
    }
    return false;
}
static bool number(const char *p, uint32_t *n) {
    if (!p || !*p)
        return false;
    uint32_t v = 0;
    while (*p) {
        if (*p < '0' || *p > '9' || v > (UINT32_MAX - (unsigned)(*p - '0')) / 10)
            return false;
        v = v * 10 + (unsigned)(*p++ - '0');
    }
    *n = v;
    return true;
}
static int hex(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}
static bool name_valid(const char *name) {
    if (!ls_lap_plain_name(name, 40))
        return false;
    size_t n = strlen(name);
    return n >= 5 && !strcmp(name + n - 4, ".lap");
}

bool ls_lap_transfer_command(ls_lap_transfer_t *s, int argc, const char *const *argv, uint32_t ms,
                             char *reply, size_t cap) {
    if (argc < 2)
        return false;
    const char *cmd = argv[1];
    if (strcmp(cmd, "put") && strcmp(cmd, "data") && strcmp(cmd, "end"))
        return false;
    if (ls_lap_transfer_expire(s, ms)) {
        snprintf(reply, cap, "APP ERR timeout upload expired");
        return true;
    }
    const char *code = "args", *why = "invalid upload arguments";
    if (!strcmp(cmd, "put")) {
        if (s->active)
            abort_upload(s);
        uint32_t size;
        if (argc != 5 || !name_valid(argv[2]) || !number(argv[3], &size) || strlen(argv[4]) != 64)
            goto bad;
        if (size < 52 || size > LS_LAP_MAX_BYTES) {
            code = "size";
            why = "app must be 52..524288 bytes";
            goto bad;
        }
        for (unsigned i = 0; i < 32; i++) {
            int a = hex(argv[4][i * 2]), b = hex(argv[4][i * 2 + 1]);
            if (a < 0 || b < 0)
                goto bad;
            s->sha256[i] = (uint8_t)(a * 16 + b);
        }
        s->size = size;
        s->received = 0;
        s->seq = 0;
        s->last_ms = ms;
        snprintf(s->name, sizeof(s->name), "%s", argv[2]);
        if (!s->io.begin(s->io.ctx, s->name)) {
            code = "io";
            why = "cannot create incoming file";
            goto bad;
        }
        s->active = true;
        snprintf(reply, cap, "APP OK put chunk=72 next=0");
        return true;
    }
    if (!s->active) {
        code = "state";
        why = "no upload in progress";
        goto bad;
    }
    if (!strcmp(cmd, "data")) {
        uint32_t seq;
        uint8_t bytes[LS_LAP_CHUNK_BYTES];
        size_t n;
        if (argc != 4 || !number(argv[2], &seq) || seq != s->seq) {
            code = "seq";
            why = "sequence must be contiguous from zero";
            goto bad;
        }
        if (strlen(argv[3]) > 96 || !*argv[3] ||
            !ls_base64_decode(argv[3], bytes, sizeof(bytes), &n)) {
            code = "base64";
            why = "invalid base64 chunk";
            goto bad;
        }
        if (n > s->size - s->received) {
            code = "size";
            why = "upload size overrun";
            goto bad;
        }
        if (!s->io.write(s->io.ctx, bytes, n)) {
            code = "io";
            why = "incoming write failed";
            goto bad;
        }
        s->received += n;
        s->seq++;
        s->last_ms = ms;
        snprintf(reply, cap, "APP OK data next=%lu received=%lu", (unsigned long)s->seq,
                 (unsigned long)s->received);
        return true;
    }
    if (argc != 2)
        goto bad;
    if (s->received != s->size) {
        code = "size";
        why = "upload size mismatch";
        goto bad;
    }
    char detail[96] = "verification failed";
    if (!s->io.finish(s->io.ctx, s->name, s->size, s->sha256, detail, sizeof(detail))) {
        abort_upload(s);
        snprintf(reply, cap, "APP ERR verify %s", detail);
        return true;
    }
    s->active = false;
    snprintf(reply, cap, "APP OK end %s", s->name);
    return true;
bad:
    abort_upload(s);
    snprintf(reply, cap, "APP ERR %s %s", code, why);
    return true;
}
