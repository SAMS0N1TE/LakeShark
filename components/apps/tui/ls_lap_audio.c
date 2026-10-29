/* API 2.0 audio for native apps; see ls_lap_radio.h and lakeshark_app.h.
 *
 * Hearing: audio_out hands every block of live radio audio (FM, P25 voice)
 * to ls_lap_audio_tap before volume and mute, on the receiver's own task.
 * The tap copies it into a one second ring the app reads; a second receiver
 * task arriving at the same moment drops its block rather than wait.
 *
 * Playing: lap_audio_write copies into a half second ring; the lap_io worker
 * feeds audio_out's cue path (the one tones use: it never stops or discards
 * speech) a little ahead of the player, at most 200 ms queued. While the
 * board is muted, or live radio has the speaker, the worker takes the app's
 * samples at playing speed and drops them, so an app neither stalls nor
 * talks over a transmission.
 *
 * Both rings are PSRAM, allocated the first time an app uses them and kept,
 * so a receiver task can never touch freed memory. */
#include "ls_lap_radio.h"
#include "ls_lap_api.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#if defined(ESP_PLATFORM) && !defined(LS_LAP_HOST_TEST)
#include "audio/audio_out.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#define BIG_ALLOC(n) heap_caps_calloc(1, (n), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define DEVICE 1
#else
#define BIG_ALLOC(n) calloc(1, (n))
#define DEVICE 0
#endif

#define HEAR_N 16384u           /* ~1 s at 16 kHz */
#define PLAY_N 8192u            /* ~1/2 s */
#define AHEAD_MS 200u
_Static_assert((HEAR_N & (HEAR_N - 1)) == 0 && (PLAY_N & (PLAY_N - 1)) == 0, "rings wrap by mask");

typedef struct {
    atomic_uint head, tail;
    int16_t pcm[];
} ring_t;
static ring_t *_Atomic hear;
static ring_t *_Atomic play;
static atomic_bool listening, flush_play;
static atomic_uint flush_to;             /* the play queue's head when its app closed */
static atomic_flag tap_busy = ATOMIC_FLAG_INIT;
static atomic_uint heard_dropped, play_dropped;
static int64_t paced_us;                 /* worker: when it last let samples go */
#if !DEVICE
static ls_lap_hub_host_t host;
ls_lap_hub_host_t *ls_lap_hub_host(void) { return &host; }
#endif

static ring_t *ring(ring_t *_Atomic *slot, unsigned n) {
    ring_t *r = atomic_load_explicit(slot, memory_order_acquire);
    if (!r) {
        r = BIG_ALLOC(sizeof(ring_t) + n * sizeof(int16_t));
        if (r) atomic_store_explicit(slot, r, memory_order_release);
    }
    return r;
}

/* ---- hearing ------------------------------------------------------------ */

void ls_lap_audio_tap(const int16_t *pcm, int n) {
    if (!atomic_load_explicit(&listening, memory_order_acquire) || !pcm || n <= 0) return;
    ring_t *r = atomic_load_explicit(&hear, memory_order_acquire);
    if (!r) return;
    if (atomic_flag_test_and_set_explicit(&tap_busy, memory_order_acquire)) {
        atomic_fetch_add(&heard_dropped, (unsigned)n);
        return;
    }
    const unsigned h = atomic_load_explicit(&r->head, memory_order_relaxed);
    const unsigned room = HEAR_N - (h - atomic_load_explicit(&r->tail, memory_order_acquire));
    unsigned k = (unsigned)n < room ? (unsigned)n : room;
    if (k < (unsigned)n) atomic_fetch_add(&heard_dropped, (unsigned)n - k);
    for (unsigned i = 0; i < k; i++) r->pcm[(h + i) & (HEAR_N - 1)] = pcm[i];
    atomic_store_explicit(&r->head, h + k, memory_order_release);
    atomic_flag_clear_explicit(&tap_busy, memory_order_release);
}
int lap_audio_listen(bool on) {
    if (!ls_lap_api_inside()) return LAP_AUDIO_EINVAL;
    if (!(ls_lap_granted() & LAP_CAP_RADIO_RX)) return LAP_AUDIO_EACCES;
    if (on && !ring(&hear, HEAR_N)) return LAP_AUDIO_EINVAL;
#if DEVICE
    if (on) audio_out_set_tap(ls_lap_audio_tap);   /* once is enough; it stays */
#endif
    ring_t *r = atomic_load(&hear);
    if (!r) return 0;
    if (on && !atomic_load(&listening)) atomic_store(&r->tail, atomic_load(&r->head));   /* start from now */
    atomic_store_explicit(&listening, on, memory_order_release);
    return 0;
}
int lap_audio_read(int16_t *pcm, int max) {
    if (!ls_lap_api_inside() || max < 0) return LAP_AUDIO_EINVAL;
    if (!(ls_lap_granted() & LAP_CAP_RADIO_RX)) return LAP_AUDIO_EACCES;
    if (max && !ls_lap_api_owns(pcm, (size_t)max * sizeof(int16_t))) return LAP_AUDIO_EINVAL;
    ring_t *r = atomic_load_explicit(&hear, memory_order_acquire);
    if (!r || !atomic_load(&listening)) return 0;
    const unsigned t = atomic_load_explicit(&r->tail, memory_order_relaxed);
    unsigned k = atomic_load_explicit(&r->head, memory_order_acquire) - t;
    if (k > (unsigned)max) k = (unsigned)max;
    for (unsigned i = 0; i < k; i++) pcm[i] = r->pcm[(t + i) & (HEAR_N - 1)];
    atomic_store_explicit(&r->tail, t + k, memory_order_release);
    return (int)k;
}

/* ---- playing ------------------------------------------------------------ */

int lap_audio_write(const int16_t *pcm, int n) {
    if (!ls_lap_api_inside() || n < 0) return LAP_AUDIO_EINVAL;
    if (n && !ls_lap_api_owns(pcm, (size_t)n * sizeof(int16_t))) return LAP_AUDIO_EINVAL;
    ring_t *r = ring(&play, PLAY_N);
    if (!r) return LAP_AUDIO_EINVAL;
    const unsigned h = atomic_load_explicit(&r->head, memory_order_relaxed);
    const unsigned room = PLAY_N - (h - atomic_load_explicit(&r->tail, memory_order_acquire));
    const unsigned k = (unsigned)n < room ? (unsigned)n : room;
    for (unsigned i = 0; i < k; i++) r->pcm[(h + i) & (PLAY_N - 1)] = pcm[i];
    atomic_store_explicit(&r->head, h + k, memory_order_release);
    return (int)k;
}
static bool board_muted(void) {
#if DEVICE
    return audio_is_muted();
#else
    return host.muted;
#endif
}
static bool board_live(void) {
#if DEVICE
    return audio_out_live_active();
#else
    return host.live;
#endif
}
int lap_audio_status(lap_audio_status_t *out) {
    if (!ls_lap_api_inside() || !ls_lap_api_owns(out, sizeof(*out))) return LAP_AUDIO_EINVAL;
    memset(out, 0, sizeof(*out));
    out->listening = atomic_load(&listening);
    out->muted = board_muted();
    out->live = board_live();
#if DEVICE
    out->volume = audio_volume_get();
#else
    out->volume = 30;
#endif
    out->heard_dropped = atomic_load(&heard_dropped);
    out->play_dropped = atomic_load(&play_dropped);
    ring_t *r = atomic_load(&play);
    if (r) out->play_queued = atomic_load(&r->head) - atomic_load(&r->tail);
    return 0;
}

/* Worker: let at most `n` samples go from the app's queue, to the speaker
   or (muted, live radio) nowhere. */
static unsigned drain(ring_t *r, unsigned n, bool drop) {
#if DEVICE
    EXT_RAM_BSS_ATTR static int16_t chunk[256];   /* worker only; PSRAM, as the rings are */
#endif
    const unsigned t0 = atomic_load_explicit(&r->tail, memory_order_relaxed);
    unsigned have = atomic_load_explicit(&r->head, memory_order_acquire) - t0;
    if (n > have) n = have;
    if (drop) {
        atomic_fetch_add(&play_dropped, n);
    } else {
        for (unsigned done = 0; done < n;) {
            unsigned k = n - done < 256u ? n - done : 256u;
#if DEVICE
            for (unsigned i = 0; i < k; i++) chunk[i] = r->pcm[(t0 + done + i) & (PLAY_N - 1)];
            audio_write_cue(chunk, (int)k);
#else
            host.played += (int)k;
#endif
            done += k;
        }
    }
    atomic_store_explicit(&r->tail, t0 + n, memory_order_release);
    return n;
}
bool ls_lap_audio_service(void) {
    ring_t *r = atomic_load_explicit(&play, memory_order_acquire);
    if (!r) return false;
    if (atomic_exchange(&flush_play, false)) {
        /* Drop what the closed app left, not what the next one has written. */
        const unsigned to = atomic_load(&flush_to);
        if ((int)(to - atomic_load(&r->tail)) > 0) atomic_store(&r->tail, to);
    }
    const unsigned waiting = atomic_load(&r->head) - atomic_load(&r->tail);
    if (!waiting) { paced_us = 0; return false; }
    const bool drop = board_muted() || board_live();
#if DEVICE
    const int64_t now = esp_timer_get_time();
    if (drop) {
        /* At playing speed, so the app sees the same pace as when it plays. */
        if (!paced_us) paced_us = now;
        const unsigned n = (unsigned)((now - paced_us) * 16 / 1000);
        if (n) { drain(r, n, true); paced_us = now; }
        return true;
    }
    paced_us = 0;
    const unsigned queued = audio_out_ring_avail() / 2u;
    const unsigned ahead = LAP_AUDIO_RATE * AHEAD_MS / 1000u;
    if (queued < ahead) {
        audio_out_play_now();
        drain(r, ahead - queued, false);
    }
#else
    /* The bench has no player: everything waiting goes at once. */
    drain(r, waiting, drop);
#endif
    return true;
}

/* ---- lifetime ----------------------------------------------------------- */

void ls_lap_audio_begin(void) {
    atomic_store(&listening, false);
    atomic_store(&heard_dropped, 0u);
    atomic_store(&play_dropped, 0u);
}
void ls_lap_audio_end(void) {
    atomic_store(&listening, false);
    ring_t *r = atomic_load(&play);
    if (r) {                             /* the worker empties it */
        atomic_store(&flush_to, atomic_load(&r->head));
        atomic_store(&flush_play, true);
    }
}
