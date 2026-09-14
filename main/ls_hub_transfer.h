#ifndef LS_HUB_TRANSFER_H
#define LS_HUB_TRANSFER_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static inline bool ls_hub_map_name(const char *name)
{
    if (!name) return false;
    size_t n = strlen(name);
    if (n < 9 || n > 64 || name[0] == '.' || strcmp(name + n - 8, ".pmtiles")) return false;
    for (size_t i = 0; i < n; ++i) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.')) return false;
    }
    return strstr(name, "..") == NULL;
}

static inline uint32_t ls_hub_crc32(uint32_t crc, const void *data, size_t size)
{
    const uint8_t *p = (const uint8_t *)data;
    crc = ~crc;
    while (size--) {
        crc ^= *p++;
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

static inline bool ls_hub_map_header(const uint8_t *header, uint64_t size)
{
    if (size < 127 || memcmp(header, "PMTiles\003", 8) || header[99] != 1 ||
        header[100] > header[101] || header[101] > 30) return false;
    for (int offset = 8; offset <= 56; offset += 16) {
        uint64_t start = 0, length = 0;
        for (int b = 0; b < 8; ++b) {
            start |= (uint64_t)header[offset + b] << (8 * b);
            length |= (uint64_t)header[offset + 8 + b] << (8 * b);
        }
        if (start > size || length > size - start || (length && start < 127)) return false;
    }
    return true;
}
#endif
