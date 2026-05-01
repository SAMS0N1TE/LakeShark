/*
 * plane_audio.h - event-to-utterance mapping for the ADS-B receiver.
 *
 * Given an ICAO address and (optional) callsign, classify the aircraft
 * into a broad category and produce a short phrase SAM can pronounce.
 */
#ifndef PLANE_AUDIO_H
#define PLANE_AUDIO_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "audio_events.h"   /* canonical home of audio_mode_t / AUD_MODE_* */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PLANE_UNKNOWN = 0,
    PLANE_COMMERCIAL,   /* IATA/ICAO airline callsign e.g. UAL123, RYR4721   */
    PLANE_GA,           /* general-aviation tail number  e.g. N12345, G-ABCD */
    PLANE_MILITARY,     /* mil ICAO range or known mil callsign prefix       */
} plane_category_t;

/* Classify an aircraft. Pass callsign = NULL or "" if unknown; ICAO range
 * alone can still identify military. */
plane_category_t plane_classify(uint32_t icao, const char *callsign);

/* Short human-readable label for the category ("MIL", "COM", "GA", "UNK"). */
const char *plane_category_label(plane_category_t cat);

/* Build a SAM-friendly new-contact utterance into 'out'. */
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
