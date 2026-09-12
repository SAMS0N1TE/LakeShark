

#ifndef LS_PMTILES_COMPAT_STORAGE_H
#define LS_PMTILES_COMPAT_STORAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "furi.h"

typedef struct File File;

/* Open access mode and open-status flags. The reader passes exactly one pair
   and never inspects them, so they only have to exist and be distinct. */
#define FSAM_READ           0x01
#define FSOM_OPEN_EXISTING  0x02

static inline File *storage_file_alloc(Storage *s)
{
    (void)s;
    /* Nothing is allocated until the open, so this is a slot for the FILE*.
       A calloc rather than a malloc: storage_file_close is allowed to run on
       a handle whose open failed, and it checks the pointer. */
    return (File *)calloc(1, sizeof(FILE *));
}

static inline bool storage_file_open(File *f, const char *path,
                                     uint8_t access, uint8_t mode)
{
    (void)access;
    (void)mode;
    if (!f || !path) return false;
    FILE *fp = fopen(path, "rb");
    if (!fp) return false;
    *(FILE **)f = fp;
    return true;
}

static inline bool storage_file_seek(File *f, uint32_t offset, bool from_start)
{
    if (!f || !*(FILE **)f) return false;
    return fseek(*(FILE **)f, (long)offset,
                 from_start ? SEEK_SET : SEEK_CUR) == 0;
}

static inline uint16_t storage_file_read(File *f, void *dst, uint16_t len)
{
    if (!f || !*(FILE **)f || !dst) return 0;
    return (uint16_t)fread(dst, 1, len, *(FILE **)f);
}

static inline void storage_file_close(File *f)
{
    if (f && *(FILE **)f) {
        fclose(*(FILE **)f);
        *(FILE **)f = NULL;
    }
}

static inline void storage_file_free(File *f) { free(f); }

#endif
