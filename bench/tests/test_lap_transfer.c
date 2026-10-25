#include "core/ls_base64.h"
#include "ls_lap_transfer.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
typedef struct {
    unsigned char incoming[200], installed[200];
    size_t size, installed_size;
    unsigned aborts;
    bool fail_begin, fail_write, fail_finish, exists;
} fs;
static bool begin(void *ctx, const char *name) {
    fs *s = ctx;
    assert(!strcmp(name, "probe.lap"));
    s->size = 0;
    s->exists = !s->fail_begin;
    return s->exists;
}
static bool write_chunk(void *ctx, const uint8_t *bytes, size_t n) {
    fs *s = ctx;
    if (s->fail_write)
        return false;
    assert(n <= sizeof(s->incoming) - s->size);
    memcpy(s->incoming + s->size, bytes, n);
    s->size += n;
    return true;
}
static bool finish(void *ctx, const char *name, size_t n, const uint8_t hash[32], char *why,
                   size_t cap) {
    fs *s = ctx;
    (void)name;
    assert(n == s->size);
    for (int i = 0; i < 32; i++)
        assert(hash[i] == 0);
    if (s->fail_finish) {
        snprintf(why, cap, "SHA/signature/manifest refused");
        return false;
    }
    memcpy(s->installed, s->incoming, n);
    s->installed_size = n;
    s->exists = false;
    return true;
}
static void abort_upload(void *ctx) {
    fs *s = ctx;
    s->exists = false;
    s->size = 0;
    s->aborts++;
}
static const char *sha = "0000000000000000000000000000000000000000000000000000000000000000";
static const char *payload = "QUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQ"
                             "UFBQUFBQUFBQUFBQUFBQUFBQUFB";
static char reply[160];
static void put(ls_lap_transfer_t *s, unsigned ms) {
    const char *a[] = {"app", "put", "probe.lap", "72", sha};
    assert(ls_lap_transfer_command(s, 5, a, ms, reply, sizeof(reply)));
}
static void data(ls_lap_transfer_t *s, const char *seq, const char *b64, unsigned ms) {
    const char *a[] = {"app", "data", seq, b64};
    assert(ls_lap_transfer_command(s, 4, a, ms, reply, sizeof(reply)));
}
static void end(ls_lap_transfer_t *s, unsigned ms) {
    const char *a[] = {"app", "end"};
    assert(ls_lap_transfer_command(s, 2, a, ms, reply, sizeof(reply)));
}
int main(void) {
    fs disk = {0};
    ls_lap_transfer_t s;
    ls_lap_transfer_init(&s,
                         (ls_lap_transfer_io_t){begin, write_chunk, finish, abort_upload, &disk});
    unsigned checks = 0;
    put(&s, 0);
    assert(s.active);
    data(&s, "0", payload, 1);
    assert(s.received == 72);
    end(&s, 2);
    assert(!s.active && !disk.exists && disk.installed_size == 72 && !memcmp(reply, "APP OK", 6));
    checks++;
    for (unsigned which = 0; which < 12; which++) {
        put(&s, 100);
        assert(s.active);
        unsigned previous = disk.aborts;
        switch (which) {
        case 0:
            data(&s, "1", payload, 101);
            break;
        case 1:
            data(&s, "-1", payload, 101);
            break;
        case 2:
            data(&s, "4294967296", payload, 101);
            break;
        case 3:
            data(&s, "0", "@@==", 101);
            break;
        case 4:
            data(&s, "0", "QQ=Q", 101);
            break;
        case 5:
            data(&s, "0", "QR==", 101);
            break;
        case 6:
            end(&s, 101);
            break;
        case 7:
            data(&s, "0", payload, 101);
            data(&s, "1", "QQ==", 102);
            break;
        case 8:
            data(&s, "0", payload, 30100);
            break;
        case 9:
            disk.fail_write = true;
            data(&s, "0", payload, 101);
            disk.fail_write = false;
            break;
        case 10:
            disk.fail_finish = true;
            data(&s, "0", payload, 101);
            end(&s, 102);
            disk.fail_finish = false;
            break;
        case 11:
            data(&s, "0", payload, 101);
            data(&s, "0", payload, 102);
            break;
        }
        assert(!s.active && !disk.exists && disk.aborts > previous && !memcmp(reply, "APP ERR", 7));
        assert(disk.installed_size == 72 && disk.installed[0] == 'A');
        checks++;
    }
    const char *bad_names[] = {"../probe.lap", ".probe.lap", "x/probe.lap", "probe.LAP",
                               "probe.lap/"};
    for (unsigned i = 0; i < 5; i++) {
        const char *a[] = {"app", "put", bad_names[i], "72", sha};
        assert(ls_lap_transfer_command(&s, 5, a, 0, reply, sizeof(reply)));
        assert(!s.active);
        checks++;
    }
    const char *sizes[] = {"0", "51", "524289", "4294967296", "-1", "72x"};
    for (unsigned i = 0; i < 6; i++) {
        const char *a[] = {"app", "put", "probe.lap", sizes[i], sha};
        assert(ls_lap_transfer_command(&s, 5, a, 0, reply, sizeof(reply)));
        assert(!s.active);
        checks++;
    }
    disk.fail_begin = true;
    put(&s, 0);
    assert(!s.active && !disk.exists);
    disk.fail_begin = false;
    checks++;
    put(&s, UINT32_MAX - 1000);
    assert(!ls_lap_transfer_expire(&s, 500));
    assert(ls_lap_transfer_expire(&s, 29000));
    assert(!disk.exists);
    checks++;
    put(&s, 1);
    assert(!ls_lap_transfer_expire(&s, 30000));
    assert(ls_lap_transfer_expire(&s, 30001));
    checks++;
    uint8_t out[3];
    size_t n;
    assert(ls_base64_decode("TWFu", out, 3, &n) && n == 3 && !memcmp(out, "Man", 3));
    checks++;
    assert(!ls_base64_decode("TWFu", out, 2, &n));
    assert(!ls_base64_decode("QQ=", out, 3, &n));
    checks++;
    assert(strlen(payload) + strlen("app data 4294967295 ") + 2 <= 128);
    checks++;
    printf("test_lap_transfer: %u cases passed\n", checks);
    return 0;
}
