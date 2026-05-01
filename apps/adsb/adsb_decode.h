/*
 * adsb_decode.h - ADS-B sample-to-message pipeline.
 *
 * The on_sample entry takes raw IQ from the radio stream, computes
 * magnitudes, runs Mode-S detection, and dispatches each decoded
 * message into the aircraft table + audio events + event bus.
 */
#ifndef ADSB_DECODE_H
#define ADSB_DECODE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void adsb_decode_init(void);

void adsb_on_sample(uint8_t *iq, int len);

void adsb_periodic_age(int64_t now_us);

/* Inject a fully-populated synthetic aircraft into the table for
 * end-to-end testing of the event pipeline (rendering, audio events,
 * JSONL stream). Called from the ADS-B TUI key handler ('t' for test)
 * so a developer can verify the host-side consumer is receiving and
 * parsing events without needing a live aircraft, HackRF transmitter,
 * or even a connected RTL-SDR. Each call rotates the synthetic ICAO
 * and a few field values so the consumer can see distinct events. */
void adsb_inject_fake_aircraft(void);

#ifdef __cplusplus
}
#endif

#endif
