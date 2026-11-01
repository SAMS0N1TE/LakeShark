/* Regression for the Nano O2 build's existing terminal sink overflow.
 * Include the private sink to exercise chunks larger than its buffer. */
#include "../../components/apps/tui/ls_vt.c"
#include <assert.h>
static char captured[256];
static size_t captured_len;
static void capture(void *ctx, const char *bytes, size_t n)
{
    (void)ctx;
    assert(captured_len + n <= sizeof(captured));
    memcpy(captured + captured_len, bytes, n);
    captured_len += n;
}
int main(void)
{
    struct {
        unsigned before;
        char bytes[8];
        unsigned after;
    } storage = {.before = 0x12345678, .after = 0x76543210};
    sink k = {.buf = storage.bytes, .cap = sizeof(storage.bytes), .out = capture};
    put(&k, "abc", 3);
    put(&k, "01234567890123456789", 20);
    put(&k, "xy", 2);
    flush(&k);
    assert(storage.before == 0x12345678 && storage.after == 0x76543210);
    assert(captured_len == 25 && k.total == 25);
    assert(!memcmp(captured, "abc01234567890123456789xy", 25));
    captured_len = 0;
    size_t n = ls_vt_leave(capture, NULL);
    assert(n == captured_len && n > 0);
    puts("terminal sink oversized chunks passed");
    return 0;
}
