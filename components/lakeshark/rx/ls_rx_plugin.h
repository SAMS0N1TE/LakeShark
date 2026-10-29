/* RX HEAD plugin contract: one receiver, swappable decoders.

   A decoder is a plugin: it says what it needs (which inputs, which front,
   which band and radio), and the head feeds it blocks of exactly that. The
   head owns the radio, the tuning, the front (FM discriminator, AM envelope,
   audio) and the output paths; a plugin owns only its decoder state.

   Inputs are a bitmask so one stream can carry several at once, computed once
   per block by the front (ls_rx_front.h) whatever number of plugins read them.
   Outputs: decoded items go to the ls_decoded timeline (ls_rx_emit), audio to
   the head's one audio sink (ls_rx_audio), tune requests to the owner
   (ls_rx_request_tune). Per-kind stores (FM page ring, aprs_store, ...) stay
   as they are and are bound by the owner where a plugin keeps them.

   Pure C with no radio, UI or RTOS dependency, so the bench builds it.
   Precedent: ls_experiment_t (apps/tui/ls_experiments.h); the SDK's
   lap_decoder_t keeps the field order of ls_rx_needs_t. */
#ifndef LS_RX_PLUGIN_H
#define LS_RX_PLUGIN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LS_RX_ABI 1

/* What a block carries. Preference order when a plugin takes several. */
enum {
    LS_RX_IQ_U8   = 1u << 0,  /* RTL-style unsigned I/Q                         */
    LS_RX_IQ_S16  = 1u << 1,  /* LR 125k / SDK lap_rx I/Q, channel at 0 Hz     */
    LS_RX_DISC    = 1u << 2,  /* float FM discriminator, 32 kHz                 */
    LS_RX_ENV     = 1u << 3,  /* float AM envelope, 32 kHz                      */
    LS_RX_PCM16   = 1u << 4,  /* int16 audio after de-emphasis, 16 kHz          */
    LS_RX_BITS    = 1u << 5,  /* chip FSK FIFO bytes, MSB first, clock recovered */
    LS_RX_BATCH   = 1u << 6,  /* sync-stripped batches from the chip            */
    LS_RX_PACKET  = 1u << 7,  /* LoRa/FSK packet; rssi/snr in the block         */
    LS_RX_EDGES   = 1u << 8,  /* OOK pulse edges, microseconds                  */
    LS_RX_RSSI    = 1u << 9,  /* dBm series                                     */
};
#define LS_RX_FRONT_INPUTS (LS_RX_DISC | LS_RX_ENV | LS_RX_PCM16)

typedef enum {
    LS_RX_FRONT_NONE = 0,     /* raw: I/Q, bits, packets                        */
    LS_RX_FRONT_NFM,          /* discriminator, then audio                      */
    LS_RX_FRONT_AM,           /* envelope                                       */
    LS_RX_FRONT_WFM,
} ls_rx_front_kind_t;

/* needs.flags */
enum {
    LS_RX_F_AUDIO        = 1u << 0, /* writes the speaker; one per head          */
    LS_RX_F_TUNES        = 1u << 1, /* may ls_rx_request_tune; one per stream    */
    LS_RX_F_EXCLUSIVE    = 1u << 2, /* nothing else on the stream                */
    LS_RX_F_NEEDS_LR2021 = 1u << 3,
    LS_RX_F_OWNS_RADIO   = 1u << 4, /* an app adapter: drives the radio itself   */
    LS_RX_F_TRYING       = 1u << 5, /* shown in the maturity hue                 */
};

typedef enum { LS_RX_IDEA = 0, LS_RX_TRYING, LS_RX_WORKS } ls_rx_maturity_t;

typedef enum {
    LS_RX_RESET_GAP = 0,      /* samples were lost: drop alignment, keep counts */
    LS_RX_RESET_RETUNE,       /* new frequency                                  */
    LS_RX_RESET_RATE,         /* new sample rate or bit rate                    */
    LS_RX_RESET_USER,         /* the operator cleared it: counts too            */
} ls_rx_reset_t;

/* Field order is the SDK's lap_decoder_t; append only. */
typedef struct {
    uint32_t inputs;          /* LS_RX_* it can take                            */
    uint8_t  front;           /* ls_rx_front_kind_t                             */
    uint32_t channel_bw_hz;
    uint32_t min_rate_hz;
    int32_t  offset_hz;
    uint32_t freq_lo_hz, freq_hi_hz;  /* 0, 0: any                              */
    uint16_t radios;          /* LS_OPT_RADIO bits; 0 any                       */
    int8_t   rsel_job;        /* an existing ls_rsel_job_t, or -1               */
    uint32_t psram_bytes;     /* what attach allocates, all of it PSRAM         */
    uint32_t internal_bytes;  /* 0 for built-ins                                */
    uint16_t cpu_permille;    /* measured, one core                             */
    uint16_t flags;           /* LS_RX_F_*                                      */
} ls_rx_needs_t;

/* The stream a plugin is attached to. */
typedef struct {
    uint32_t center_hz;
    uint32_t rate_hz;         /* rate of the stream's raw input                  */
    uint32_t inputs;          /* what this stream can deliver                    */
    uint8_t  front;           /* ls_rx_front_kind_t                              */
    uint8_t  radio;           /* ls_rsel_radio_t in use                          */
} ls_rx_stream_t;

/* block.flags */
enum {
    LS_RX_B_INVERTED   = 1u << 0, /* BATCH: the chip's sync matched inverted     */
    LS_RX_B_CONTIGUOUS = 1u << 1, /* BATCH: follows the previous one directly    */
};

typedef struct {
    uint32_t    input;        /* one LS_RX_* bit                                 */
    const void *data;
    uint32_t    n;            /* samples (DISC/ENV/PCM16/IQ pairs) or bytes      */
    uint32_t    rate_hz;      /* samples/s, or bits/s for BITS/BATCH             */
    int64_t     at_us;
    uint8_t     flags;        /* LS_RX_B_*                                       */
    int16_t     rssi_dbm, snr_db;   /* PACKET                                    */
} ls_rx_block_t;

/* A snapshot any task may take: words written by the decoder side only. */
typedef struct {
    bool     synced;
    bool     shed;            /* skipped blocks under load since the last tick */
    uint32_t frames;
    uint32_t decoded;         /* pages, packets, messages                       */
    uint32_t errors;
    int32_t  detail;          /* plugin's own number (baud, mode)               */
    char     chip[12];        /* mode chip text: "POCSAG", "NFM"                */
} ls_rx_status_t;

struct ls_opt_s;              /* apps/tui/ls_options.h                           */
struct ls_rx_panel;           /* apps/tui/rx_panels/ls_rx_panel.h                */
struct ls_rx_head;

typedef struct ls_rx_plugin {
    uint16_t abi;             /* LS_RX_ABI                                      */
    const char *id;           /* console name, lower case: "pocsag"             */
    const char *name;         /* chip and tab: "POCSAG"                         */
    const char *sub;          /* one line: what it hears                        */
    uint8_t maturity;         /* ls_rx_maturity_t                               */
    ls_rx_needs_t needs;
    const struct ls_opt_s *opts; int n_opts;
    const char *doc;          /* required: what it decodes, where, how to test  */

    /* Owner task. attach allocates in PSRAM only and returns the state, or
       NULL with why. detach is fenced: no process() runs during or after. */
    void *(*attach)(struct ls_rx_head *h, const ls_rx_stream_t *s, char *why, size_t n);
    void  (*detach)(void *st);
    void  (*reset)(void *st, ls_rx_reset_t why);
    void  (*retune)(void *st, uint32_t hz);
    /* Bounded and non-blocking: no allocation, flash, NVS or waits. */
    void  (*process)(void *st, const ls_rx_block_t *b);
    void  (*tick)(void *st);                  /* about 10 Hz, owner task        */
    void  (*status)(const void *st, ls_rx_status_t *out);   /* any task         */
    const struct ls_rx_panel *panel;          /* UI task only; may be NULL      */
    const void *ctx;          /* an adapter's own constant (the experiment)     */
} ls_rx_plugin_t;

#ifdef __cplusplus
}
#endif
#endif
