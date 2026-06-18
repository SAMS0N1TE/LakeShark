
#ifndef PLANE_AUDIO_H
#define PLANE_AUDIO_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "audio_events.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PLANE_UNKNOWN = 0,
    PLANE_COMMERCIAL,
    PLANE_GA,
    PLANE_MILITARY,
} plane_category_t;

plane_category_t plane_classify(uint32_t icao, const char *callsign);

const char *plane_category_label(plane_category_t cat);

void plane_phrase_new_contact(char *out, size_t out_sz,
                              uint32_t icao, const char *callsign,
                              plane_category_t cat, bool crc_shaky);

void plane_phrase_lost_contact(char *out, size_t out_sz,
                               uint32_t icao, const char *callsign);

void plane_phrase_position(char *out, size_t out_sz,
                           uint32_t icao, const char *callsign);

#ifdef __cplusplus
}
#endif

#endif
