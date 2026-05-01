/*
 * stream.h - app-agnostic IQ read loop.
 *
 * Spawns a task that pulls bulk transfers from the RTL-SDR and hands
 * each buffer to the currently-active app via app_current()->on_sample().
 * The app decides what to do with the bytes - demodulate, FFT, FM
 * discriminate, whatever. Switching apps mid-stream is supported; the
 * read loop itself doesn't care which app is consuming.
 */
#ifndef RADIO_STREAM_H
#define RADIO_STREAM_H
#ifdef __cplusplus
extern "C" {
#endif
#define STREAM_PACKET_SIZE   16384
#define STREAM_BUFFER_BYTES  (STREAM_PACKET_SIZE * 2)
void radio_stream_start(void);
#ifdef __cplusplus
}
#endif
#endif