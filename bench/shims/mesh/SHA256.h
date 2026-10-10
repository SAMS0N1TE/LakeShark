#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
/* Deterministic packet identity fixture, not a cryptographic implementation. */
class SHA256 {
    uint32_t hash = 2166136261u;
public:
    void reset() { hash = 2166136261u; }
    void update(const void *ptr, size_t len) {
        const uint8_t *p = static_cast<const uint8_t *>(ptr);
        for (size_t i = 0; i < len; i++) hash = (hash ^ p[i]) * 16777619u;
    }
    void finalize(void *ptr, size_t len) {
        uint8_t *p = static_cast<uint8_t *>(ptr);
        for (size_t i = 0; i < len; i++) p[i] = (uint8_t)(hash >> ((i % 4) * 8));
    }
};
