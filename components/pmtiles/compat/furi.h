/* Enough of the Flipper's furi API for ZeroMesh's PMTiles reader.

   The reader is vendored byte-identical from ZeroMesh so it can be re-synced
   with a file copy, which means the environment moves rather than the code.
   Its whole dependency on the Flipper is a logger and a way to get at
   storage; both are one line here.

   This is the same arrangement components/meshcore/compat/ uses for the
   Arduino API, and for the same reason: a shim is reviewable and a fork is
   not. */
#ifndef LS_PMTILES_COMPAT_FURI_H
#define LS_PMTILES_COMPAT_FURI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

#define FURI_LOG_E(tag, ...) ESP_LOGE(tag, __VA_ARGS__)
#define FURI_LOG_W(tag, ...) ESP_LOGW(tag, __VA_ARGS__)
#define FURI_LOG_I(tag, ...) ESP_LOGI(tag, __VA_ARGS__)
#define FURI_LOG_D(tag, ...) ESP_LOGD(tag, __VA_ARGS__)

/* On the Flipper a Storage is a service you take a handle to. Here the
   filesystem is already mounted and stdio reaches it, so the record is a
   token: opening it yields a non-NULL pointer the reader only ever passes
   back to storage_file_alloc. */
#define RECORD_STORAGE "storage"

typedef struct Storage Storage;

static inline Storage *furi_record_open(const char *name)
{
    (void)name;
    /* A fixed non-NULL token. The reader checks it for NULL and otherwise
       treats it as opaque, so there is nothing to allocate. */
    static int token;
    return (Storage *)&token;
}

static inline void furi_record_close(const char *name) { (void)name; }

#endif
