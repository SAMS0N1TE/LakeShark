#ifndef LS_WIFI_FILE_STREAM_H
#define LS_WIFI_FILE_STREAM_H

#include <stdio.h>
#include <stdint.h>

typedef int (*ls_wifi_file_send_fn)(void *context, const char *data, size_t size);
typedef enum {
    LS_WIFI_FILE_OK,
    LS_WIFI_FILE_READ_ERROR,
    LS_WIFI_FILE_SHORT,
    LS_WIFI_FILE_SEND_ERROR
} ls_wifi_file_result_t;

/* follow-up: fread returning zero is also an SD error. Sending the
 * terminating chunk in that case falsely certifies a truncated file as a
 * complete HTTP response. Read exactly the size observed on the open file,
 * and only terminate a response after all those bytes have been sent.
 * The caller owns the buffer and file; the sender uses HTTP's zero-length
 * final chunk convention and returns zero on success. */
static inline ls_wifi_file_result_t ls_wifi_file_stream(
    FILE *file, char *buffer, size_t capacity, uint64_t expected,
    ls_wifi_file_send_fn send, void *context, uint64_t *sent)
{
    *sent = 0;
    if (!capacity) return LS_WIFI_FILE_READ_ERROR;
    while (*sent < expected) {
        uint64_t remaining = expected - *sent;
        size_t want = remaining < capacity ? (size_t)remaining : capacity;
        size_t got = fread(buffer, 1, want, file);
        if (ferror(file)) return LS_WIFI_FILE_READ_ERROR;
        if (!got) return LS_WIFI_FILE_SHORT;
        if (send(context, buffer, got) != 0) return LS_WIFI_FILE_SEND_ERROR;
        *sent += got;
    }
    return send(context, NULL, 0) == 0 ? LS_WIFI_FILE_OK : LS_WIFI_FILE_SEND_ERROR;
}

#endif
