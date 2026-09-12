#include "ls_test.h"
#include <stdint.h>
#include <unistd.h>
static char fixture_path[96];

static size_t injected_read_limit;
static int injected_read_error;
static size_t test_read(void *p, size_t size, size_t count, FILE *f)
{
    if (injected_read_error && ftell(f) >= (long)injected_read_limit) return 0;
    if (injected_read_error && count > injected_read_limit - (size_t)ftell(f))
        count = injected_read_limit - (size_t)ftell(f);
    return fread(p, size, count, f);
}
static int test_error(FILE *f)
{
    return (injected_read_error && ftell(f) >= (long)injected_read_limit) || ferror(f);
}
#define fread test_read
#define ferror test_error
#include "ls_wifi_file_stream.h"
#undef fread
#undef ferror

typedef struct {
    uint64_t bytes;
    unsigned finished;
    uint64_t fail_at;
    int fail_finish;
} sink_t;

static int send_bytes(void *context, const char *data, size_t size)
{
    sink_t *sink = context;
    if (!size) {
        if (sink->fail_finish) return -1;
        sink->finished++;
        return 0;
    }
    if (sink->fail_at && sink->bytes >= sink->fail_at) return -1;
    for (size_t i = 0; i < size; i++)
        LS_EQ_INT((unsigned char)data[i], (unsigned char)((sink->bytes + i) * 29 + 7));
    sink->bytes += size;
    return 0;
}

static FILE *fixture(size_t bytes)
{
    injected_read_error = 0;
    snprintf(fixture_path, sizeof(fixture_path), "wifi_stream_%ld.tmp", (long)getpid());
    FILE *f = fopen(fixture_path, "w+b");
    LS_CHECK(f != NULL);
    for (size_t i = 0; i < bytes; i++) fputc((unsigned char)(i * 29 + 7), f);
    rewind(f);
    return f;
}

LS_CASE(screenshot_larger_than_reported_stall_transfers_every_byte)
{
    const size_t size = 1152054;
    FILE *f = fixture(size);
    char buffer[16384];
    sink_t sink = {0};
    uint64_t sent;
    LS_EQ_INT(ls_wifi_file_stream(f, buffer, sizeof(buffer), size, send_bytes, &sink, &sent), LS_WIFI_FILE_OK);
    LS_EQ_UINT(sent, size);
    LS_EQ_UINT(sink.bytes, size);
    LS_EQ_UINT(sink.finished, 1);
    fclose(f);
    remove(fixture_path);
}

LS_CASE(sd_read_error_at_110kb_does_not_certify_truncated_download)
{
    FILE *f = fixture(1152054);
    char buffer[2048];
    sink_t sink = {0};
    uint64_t sent;
    injected_read_limit = 110 * 1024 + 23;
    injected_read_error = 1;
    LS_EQ_INT(ls_wifi_file_stream(f, buffer, sizeof(buffer), 1152054, send_bytes, &sink, &sent), LS_WIFI_FILE_READ_ERROR);
    LS_EQ_UINT(sink.finished, 0);
    LS_EQ_UINT(sent, 110 * 1024);
    fclose(f);
    remove(fixture_path);
    injected_read_error = 0;
}

LS_CASE(file_shrinking_after_stat_is_incomplete)
{
    FILE *f = fixture(112640);
    char buffer[16384];
    sink_t sink = {0};
    uint64_t sent;
    LS_EQ_INT(ls_wifi_file_stream(f, buffer, sizeof(buffer), 1152054, send_bytes, &sink, &sent), LS_WIFI_FILE_SHORT);
    LS_EQ_UINT(sent, 112640);
    LS_EQ_UINT(sink.finished, 0);
    fclose(f);
    remove(fixture_path);
}

LS_CASE(socket_failure_is_not_retried_or_terminated)
{
    FILE *f = fixture(300000);
    char buffer[16384];
    sink_t sink = {.fail_at = 114688};
    uint64_t sent;
    LS_EQ_INT(ls_wifi_file_stream(f, buffer, sizeof(buffer), 300000, send_bytes, &sink, &sent), LS_WIFI_FILE_SEND_ERROR);
    LS_EQ_UINT(sent, 114688);
    LS_EQ_UINT(sink.finished, 0);
    LS_EQ_INT(ftell(f), 131072);
    fclose(f);
    remove(fixture_path);
}

LS_CASE(empty_file_and_final_chunk_failure)
{
    FILE *f = fixture(0);
    char buffer[2048];
    sink_t sink = {0};
    uint64_t sent;
    LS_EQ_INT(ls_wifi_file_stream(f, buffer, sizeof(buffer), 0, send_bytes, &sink, &sent), LS_WIFI_FILE_OK);
    LS_EQ_UINT(sink.finished, 1);
    sink.fail_finish = 1;
    LS_EQ_INT(ls_wifi_file_stream(f, buffer, sizeof(buffer), 0, send_bytes, &sink, &sent), LS_WIFI_FILE_SEND_ERROR);
    fclose(f);
    remove(fixture_path);
}

