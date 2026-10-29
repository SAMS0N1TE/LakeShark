/* RX HEAD core: the plugins attached to one stream, and what feeds them.

   The owner (FM's receive task today, the RADIO screen's worker later) keeps
   one ls_rx_head_t per stream, attaches the plugins its preset names, and
   hands every block to ls_rx_head_feed. The head:
     - admits a plugin only if the stream can deliver one of its inputs, at
       most one AUDIO and one TUNES plugin, nothing beside an EXCLUSIVE one,
       and while the summed cpu_permille fits the budget;
     - feeds the audio plugin first, then the data plugins, each only the
       inputs it asked for, timing every call (EMA, microseconds a block);
     - when the producer has no spare time, sheds the data plugins before the
       voice (reset GAP, a SHED chip, one timeline line per episode) and only
       then drops the block;
     - routes audio to the owner's sink and tune requests to the owner.

   Attach, detach, reset and retune run on the owner task with the feeding
   task stopped or the owner being that task: the head holds no lock. */
#ifndef LS_RX_HEAD_H
#define LS_RX_HEAD_H

#include "ls_rx_plugin.h"
#include "ls_decoded.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LS_RX_SLOTS       6
#define LS_RX_CPU_BUDGET  900      /* permille of one core for one head */

typedef struct {
    const ls_rx_plugin_t *p;
    void    *st;
    uint32_t ema_us;               /* process() time per block, EMA 1/16 */
    uint32_t blocks, shed;
    bool     shedding;
} ls_rx_slot_t;

/* Audio: write n samples at rate; return true when the sink dropped some. */
typedef bool (*ls_rx_audio_fn)(void *user, const int16_t *pcm, int n, uint32_t rate_hz);
/* Squelch/carrier gate for recorders that follow the voice. */
typedef void (*ls_rx_gate_fn)(void *user, bool open);
typedef bool (*ls_rx_tune_fn)(void *user, uint32_t hz);

typedef struct ls_rx_head {
    ls_rx_stream_t stream;
    ls_rx_slot_t   slot[LS_RX_SLOTS];
    int            n;
    uint16_t       cpu_budget;
    bool           shedding;       /* this episode: data plugins are out */
    uint32_t       shed_episodes, dropped;
    int64_t        shed_logged_us;
    ls_rx_audio_fn audio; ls_rx_gate_fn gate; void *audio_user;
    uint32_t       audio_blocks, audio_drops;
    ls_rx_tune_fn  tune;  void *tune_user;
    uint32_t       emitted;
    void          *front;          /* ls_rx_front_t feeding this head, or NULL */
    const ls_rx_plugin_t *attaching; /* during attach(): the plugin, for its ctx */
} ls_rx_head_t;

void ls_rx_head_init(ls_rx_head_t *h, const ls_rx_stream_t *s);
void ls_rx_head_set_audio(ls_rx_head_t *h, ls_rx_audio_fn fn, ls_rx_gate_fn gate, void *user);
void ls_rx_head_set_tune(ls_rx_head_t *h, ls_rx_tune_fn fn, void *user);

/* Returns the slot index, or -1 with why. */
int   ls_rx_head_attach(ls_rx_head_t *h, const ls_rx_plugin_t *p, char *why, size_t n);
void  ls_rx_head_detach(ls_rx_head_t *h, const ls_rx_plugin_t *p);
void  ls_rx_head_detach_all(ls_rx_head_t *h);
void *ls_rx_head_state(const ls_rx_head_t *h, const ls_rx_plugin_t *p);
/* Union of the inputs the attached plugins take (what the front computes). */
uint32_t ls_rx_head_wants(const ls_rx_head_t *h);
bool  ls_rx_head_has_audio(const ls_rx_head_t *h);

/* Block admission for the producer: false when the whole block must be
   dropped (the owner counts it and resets its front). With spare false and
   both voice and data attached, the first block sheds the data plugins and
   is admitted for the voice. */
bool  ls_rx_head_admit(ls_rx_head_t *h, bool spare);
void  ls_rx_head_feed(ls_rx_head_t *h, const ls_rx_block_t *b);
void  ls_rx_head_reset(ls_rx_head_t *h, ls_rx_reset_t why);
void  ls_rx_head_retune(ls_rx_head_t *h, uint32_t hz);
void  ls_rx_head_tick(ls_rx_head_t *h);
bool  ls_rx_head_status(const ls_rx_head_t *h, int slot, ls_rx_status_t *out);
/* "NFM POCSAG" style chip line, attached plugins in slot order. */
int   ls_rx_head_chips(const ls_rx_head_t *h, char *out, size_t n);

/* For plugins. */
bool  ls_rx_audio(ls_rx_head_t *h, const int16_t *pcm, int n, uint32_t rate_hz);
void  ls_rx_audio_gate(ls_rx_head_t *h, bool open);
bool  ls_rx_request_tune(ls_rx_head_t *h, const ls_rx_plugin_t *p, uint32_t hz);
void  ls_rx_emit(ls_rx_head_t *h, const ls_decoded_t *d);

#ifdef __cplusplus
}
#endif
#endif
