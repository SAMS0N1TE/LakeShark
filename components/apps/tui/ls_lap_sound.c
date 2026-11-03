#include "ls_lap_context.h"
/* Opt-in mixer/capture and worker-owned bounded WAV streams. */
#include "ls_lap_net_audio.h"
#include "ls_lap_radio.h"
#if LAP_DEVICE
#include "audio/audio_out.h"
#include "board/ls_audio_hw.h"
#endif
#define MIX_N 2048u
#define MIC_N 4096u
#define WAV_MAX 960000u
static atomic_flag lock = ATOMIC_FLAG_INIT;
#if !LAP_DEVICE
#include <stdlib.h>
static bool sim_muted, sim_live;
static int sim_mic_level = 1200;
static FILE *sim_input, *sim_output;
static ls_lap_sound_host_t host_audio;
ls_lap_sound_host_t *ls_lap_sound_host(void) { return &host_audio; }
void ls_lap_sound_script(const char *c) {
    if (!av_take(&lock))
        return;
    if (!strncmp(c, "micpcm=", 7))
        sim_mic_level = atoi(c + 7);
    else if (!strncmp(c, "audiomute=", 10))
        sim_muted = atoi(c + 10) != 0;
    else if (!strncmp(c, "audiolive=", 10))
        sim_live = atoi(c + 10) != 0;
    else if (!strncmp(c, "micwav=", 7)) {
        if (sim_input)
            fclose(sim_input);
        sim_input = fopen(c + 7, "rb");
        if (sim_input)
            fseek(sim_input, 44, SEEK_SET);
    } else if (!strncmp(c, "mixpcm=", 7)) {
        if (sim_output) {
            fflush(sim_output);
            fclose(sim_output);
        }
        sim_output = fopen(c + 7, "wb");
    }
    av_give(&lock);
}
#endif
static atomic_bool mic_indicators[LS_LAP_CONTEXTS];
#define mic_indicator mic_indicators[ls_lap_context_index()]
static LS_LAP_POOL unsigned p4_epoch_slots[LS_LAP_CONTEXTS], serial;
#define p4_epoch (p4_epoch_slots[ls_lap_context_index()])

static LS_LAP_POOL atomic_int p4_canceled_slots[LS_LAP_CONTEXTS][2], p4_mic_cancel_slots[LS_LAP_CONTEXTS], p4_record_stop_slots[LS_LAP_CONTEXTS], p4_published_slots[LS_LAP_CONTEXTS][2], p4_mic_published_slots[LS_LAP_CONTEXTS];
#define p4_canceled (p4_canceled_slots[ls_lap_context_index()])
#define p4_mic_cancel (p4_mic_cancel_slots[ls_lap_context_index()])
#define p4_mic_published (p4_mic_published_slots[ls_lap_context_index()])
#define p4_published (p4_published_slots[ls_lap_context_index()])
#define p4_record_stop (p4_record_stop_slots[ls_lap_context_index()])

static LS_LAP_POOL uint32_t p4_paced_slots[LS_LAP_CONTEXTS];
#define p4_paced (p4_paced_slots[ls_lap_context_index()])

typedef struct {
    int16_t pcm[MIX_N];
    unsigned head, tail;
    uint32_t consumed, dropped;
    int volume, duck;
    bool enabled;
} mix_channel;
static EXT_RAM_BSS_ATTR mix_channel p4_mix_slots[LS_LAP_CONTEXTS][3];
#define p4_mix (p4_mix_slots[ls_lap_context_index()])
static uint32_t mixed_at;
static bool mixed_clock;
static unsigned active_mixers(void) {
    unsigned n=0; for(unsigned i=0;i<LS_LAP_CONTEXTS;i++) n+=atomic_load(&ls_lap_av_actives[i]); return n;
}

static EXT_RAM_BSS_ATTR struct {
    int id;
    bool acquired;
    lap_operation_t status;
    int16_t pcm[MIC_N];
    unsigned head, tail;
    uint32_t total;
} p4_mic_slots[LS_LAP_CONTEXTS];
#define p4_mic (p4_mic_slots[ls_lap_context_index()])

static EXT_RAM_BSS_ATTR struct stream {
    int id, channel;
    unsigned epoch;
    bool started;
    char path[128];
    FILE *file;
    uint32_t bytes, limit, remaining;
    lap_operation_t status;
} p4_streams_slots[LS_LAP_CONTEXTS][2];
#define p4_streams (p4_streams_slots[ls_lap_context_index()])

static int new_id(void) {
    serial = serial % 0xfffffff + 1;
    return serial;
}
bool ls_lap_mic_active(void) { for(unsigned i=0;i<LS_LAP_CONTEXTS;i++) if(atomic_load(&mic_indicators[i])) return true; return false; }
static int access(uint32_t hw) { return ls_lap_av_access(hw, false); }
static void reset_mix(void) {
    memset(p4_mix, 0, sizeof(p4_mix));
    for (int i = 0; i < 3; i++) {
        p4_mix[i].volume = 100;
        p4_mix[i].duck = 30;
    }
    p4_paced = ls_lap_av_now();
    if(!mixed_clock || (active_mixers()==1 && atomic_load(&ls_lap_av_active))) { mixed_at=p4_paced; mixed_clock=true; }
}
int lap_mixer_config(int channel, int volume, int duck) {
    int e = access(LAP_HW_SPEAKER);
    if (e)
        return e;
    if (channel < 0 || channel >= 3 || volume < 0 || volume > 100 || duck < 0 || duck > 100)
        return LAP_IO_EINVAL;
    if (!av_take(&lock))
        return LAP_IO_EBUSY;
    if (p4_epoch != atomic_load(&ls_lap_av_epoch)) {
        av_give(&lock);
        return LAP_IO_EAGAIN;
    }
    p4_mix[channel].enabled = true;
    p4_mix[channel].volume = volume;
    p4_mix[channel].duck = duck;
    av_give(&lock);
    ls_lap_av_wake();
    return 0;
}
int lap_mixer_write(int channel, const int16_t *pcm, int n) {
    int e = access(LAP_HW_SPEAKER);
    if (e)
        return e;
    if (channel < 0 || channel >= 3 || n < 0 || n > (int)MIX_N ||
        (n && !ls_lap_api_readable(pcm, n * sizeof(*pcm))))
        return LAP_IO_EINVAL;
    if (!av_take(&lock))
        return LAP_IO_EBUSY;
    if (p4_epoch != atomic_load(&ls_lap_av_epoch) || !p4_mix[channel].enabled) {
        av_give(&lock);
        return LAP_IO_EAGAIN;
    }
    unsigned room = MIX_N - (p4_mix[channel].head - p4_mix[channel].tail);
    if (n > (int)room)
        n = room;
    for (int i = 0; i < n; i++)
        p4_mix[channel].pcm[p4_mix[channel].head++ % MIX_N] = pcm[i];
    av_give(&lock);
    ls_lap_av_wake();
    return n;
}
void lap_mixer_clear(int channel) {
    if (!ls_lap_api_inside() || channel < 0 || channel >= 3 || !av_take(&lock))
        return;
    p4_mix[channel].tail = p4_mix[channel].head;
    av_give(&lock);
}
int lap_mixer_status(int channel, lap_operation_t *out) {
    int e = access(LAP_HW_SPEAKER);
    if (e)
        return e;
    if (channel < 0 || channel >= 3 || !ls_lap_api_readable(out, sizeof(*out)))
        return LAP_IO_EINVAL;
    if (!av_take(&lock))
        return LAP_IO_EBUSY;
    *out = av_status(p4_mix[channel].head == p4_mix[channel].tail ? LAP_OPERATION_DONE : LAP_OPERATION_READY, 0);
    out->transferred = p4_mix[channel].consumed;
    out->dropped = p4_mix[channel].dropped;
    av_give(&lock);
    return 0;
}
int lap_mic_open(void) {
    if (!(ls_lap_granted() & LAP_CAP_MIC))
        return LAP_IO_EACCES;
    int e = access(LAP_HW_MIC);
    if (e)
        return e;
    if (!av_take(&lock))
        return LAP_IO_EBUSY;
    int h = LAP_IO_EBUSY;
    if (!p4_mic.id && p4_epoch == atomic_load(&ls_lap_av_epoch) && ls_lap_resource_claim(LS_LAP_LEASE_MIC,true)) {
        atomic_store(&p4_mic_published, 0);
        memset(&p4_mic, 0, sizeof(p4_mic));
        p4_mic.id = h = new_id();
        atomic_store(&p4_mic_published, h);
        p4_mic.status = av_status(LAP_OPERATION_PENDING, 0);
        atomic_store(&mic_indicator, true);
    }
    av_give(&lock);
    ls_lap_av_wake();
    return h;
}
int lap_mic_read(int h, int16_t *pcm, int max, lap_operation_t *status) {
    int e = access(LAP_HW_MIC);
    if (e)
        return e;
    if (h <= 0)
        return LAP_IO_ECLOSED;
    if (max < 0 || max > (int)MIC_N || (max && !ls_lap_api_readable(pcm, max * sizeof(*pcm))) ||
        !ls_lap_api_readable(status, sizeof(*status)))
        return LAP_IO_EINVAL;
    if (!av_take(&lock))
        return LAP_IO_EBUSY;
    int n = LAP_IO_ECLOSED;
    if (p4_mic.id == h && p4_epoch == atomic_load(&ls_lap_av_epoch) && atomic_load(&p4_mic_cancel) != h) {
        *status = p4_mic.status;
        unsigned k = p4_mic.head - p4_mic.tail;
        if (k > (unsigned)max)
            k = max;
        for (unsigned i = 0; i < k; i++)
            pcm[i] = p4_mic.pcm[p4_mic.tail++ % MIC_N];
        n = k ? (int)k : p4_mic.status.error ? p4_mic.status.error : LAP_IO_EAGAIN;
    }
    av_give(&lock);
    return n;
}
void lap_mic_close(int h) {
    if (ls_lap_api_inside() && h > 0)
        av_close(&p4_mic_cancel, &p4_mic_published, h);
    ls_lap_av_wake();
}

static int stream_open(int index, int channel, const char *name, uint32_t limit, int capture) {
    int e = access(LAP_HW_SD | (index ? LAP_HW_MIC : LAP_HW_SPEAKER));
    if (e)
        return e;
    char copied[49], path[128];
    if (!ls_lap_av_string(copied, sizeof(copied), name) || !ls_lap_api_data_path(path, sizeof(path), copied))
        return LAP_IO_EINVAL;
    if (!av_take(&lock))
        return LAP_IO_EBUSY;
    if (index == 1 && (capture != p4_mic.id || atomic_load(&p4_mic_cancel) == p4_mic.id || p4_mic.status.error ||
                       !(ls_lap_granted() & LAP_CAP_MIC))) {
        av_give(&lock);
        return LAP_IO_ECLOSED;
    }
    if (p4_streams[1 - index].id && p4_streams[1 - index].status.state != LAP_OPERATION_DONE &&
        !strcmp(p4_streams[1 - index].path, path)) {
        av_give(&lock);
        return LAP_IO_EBUSY;
    }
    int h = LAP_IO_EBUSY;
    if (!p4_streams[index].id && p4_epoch == atomic_load(&ls_lap_av_epoch)) {
        struct stream *s = &p4_streams[index];
        memset(s, 0, sizeof(*s));
        s->id = h = new_id() * 2 + index;
        atomic_store(&p4_published[index], h);
        s->epoch = p4_epoch;
        s->channel = channel;
        s->limit = limit;
        s->status = av_status(LAP_OPERATION_PENDING, 0);
        strcpy(s->path, path);
    }
    av_give(&lock);
    ls_lap_av_wake();
    return h;
}
int lap_sample_play(int channel, const char *name) {
    if (channel < 0 || channel >= 3)
        return LAP_IO_EINVAL;
    return stream_open(0, channel, name, WAV_MAX, 0);
}
int lap_audio_record_start(int h, const char *name, uint32_t max_ms) {
    if (!h || !max_ms || max_ms > 30000)
        return LAP_IO_EINVAL;
    return stream_open(1, 0, name, max_ms * 32, h);
}
int lap_audio_record_stop(int h) {
    int e = access(LAP_HW_SD);
    if (e)
        return e;
    if (!av_take(&lock))
        return LAP_IO_EBUSY;
    bool valid = p4_streams[1].id == h && p4_streams[1].epoch == atomic_load(&ls_lap_av_epoch) &&
                 atomic_load(&p4_canceled[1]) != h;
    if (valid)
        atomic_store(&p4_record_stop, h);
    av_give(&lock);
    ls_lap_av_wake();
    return valid ? 0 : LAP_IO_ECLOSED;
}

int lap_audio_operation(int h, lap_operation_t *out) {
    int e = access(0);
    if (e)
        return e;
    if (!ls_lap_api_readable(out, sizeof(*out)))
        return LAP_IO_EINVAL;
    if (!av_take(&lock))
        return LAP_IO_EBUSY;
    int r = LAP_IO_ECLOSED;
    for (int i = 0; i < 2; i++)
        if (p4_streams[i].id == h && p4_streams[i].epoch == atomic_load(&ls_lap_av_epoch) &&
            atomic_load(&p4_canceled[i]) != h) {
            *out = p4_streams[i].status;
            r = 0;
            break;
        }
    av_give(&lock);
    return r;
}
void lap_audio_close(int h) {
    if (ls_lap_api_inside() && h > 0)
        av_close(&p4_canceled[(unsigned)h % 2], &p4_published[(unsigned)h % 2], h);
    ls_lap_av_wake();
}

static uint32_t u32(const uint8_t *p) {
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void put32(uint8_t *p, uint32_t v) {
    for (int i = 0; i < 4; i++)
        p[i] = v >> (i * 8);
}
static bool header(FILE *f, uint32_t bytes) {
    uint8_t h[44] = {0};
    memcpy(h, "RIFF", 4);
    put32(h + 4, bytes + 36);
    memcpy(h + 8, "WAVEfmt ", 8);
    put32(h + 16, 16);
    h[20] = 1;
    h[22] = 1;
    put32(h + 24, 16000);
    put32(h + 28, 32000);
    h[32] = 2;
    h[34] = 16;
    memcpy(h + 36, "data", 4);
    put32(h + 40, bytes);
    return !fseek(f, 0, SEEK_SET) && fwrite(h, 1, sizeof(h), f) == sizeof(h);
}
static int wav_open(struct stream *s) {
    s->file = fopen(s->path, "rb");
    if (!s->file)
        return LAP_IO_EIO;
    uint8_t h[16];
    if (fread(h, 1, 12, s->file) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4))
        return LAP_IO_EINVAL;
    uint32_t riff = u32(h + 4);
    if (riff < 4 || riff > WAV_MAX + 4096)
        return LAP_IO_ELIMIT;
    uint32_t left = riff - 4;
    bool format = false;
    while (left >= 8) {
        if (fread(h, 1, 8, s->file) != 8)
            return LAP_IO_EIO;
        left -= 8;
        uint32_t n = u32(h + 4);
        if (n > left || n + (n & 1) > left)
            return LAP_IO_EINVAL;
        if (!memcmp(h, "fmt ", 4)) {
            if (n < 16 || fread(h, 1, 16, s->file) != 16)
                return LAP_IO_EINVAL;
            if (h[0] != 1 || h[1] || h[2] != 1 || h[3] || u32(h + 4) != 16000 || u32(h + 8) != 32000 ||
                h[12] != 2 || h[13] || h[14] != 16 || h[15])
                return LAP_IO_ENOTSUP;
            format = true;
            if (fseek(s->file, n - 16 + (n & 1), SEEK_CUR))
                return LAP_IO_EIO;
        } else if (!memcmp(h, "data", 4)) {
            if (!format || !n || (n & 1))
                return LAP_IO_EINVAL;
            if (n > WAV_MAX)
                return LAP_IO_ELIMIT;
            s->remaining = n;
            return 0;
        } else if (fseek(s->file, n + (n & 1), SEEK_CUR))
            return LAP_IO_EIO;
        left -= n + (n & 1);
    }
    return LAP_IO_EINVAL;
}
static void finish(struct stream *s, bool record, int error) {
    /* Even an immediate stop is a valid empty WAV. */
    if (record && !s->started && !s->file && !error) {
        s->started = true;
        s->file = fopen(s->path, "wb");
        if (!s->file)
            error = LAP_IO_EIO;
    }

    if (s->file) {
        if (record && !header(s->file, s->bytes))
            error = LAP_IO_EIO;
        if (fclose(s->file))
            error = LAP_IO_EIO;
        s->file = NULL;
    }
    s->status.state = LAP_OPERATION_DONE;
    s->status.error = error;
    s->status.transferred = s->bytes;
}
bool ls_lap_sound_service(void) {
    if (!av_take(&lock))
        return true;
    unsigned current = atomic_load(&ls_lap_av_epoch);
    bool changed = p4_epoch != current;
    if (changed) {
#if !LAP_DEVICE
        if (!active_mixers()) {
            if (sim_input) {
                fclose(sim_input);
                sim_input = NULL;
            }
            if (sim_output) {
                fclose(sim_output);
                sim_output = NULL;
            }
        }
#endif
        reset_mix();
        p4_epoch = current;
        if (p4_mic.id)
            av_cancel(&p4_mic_cancel, p4_mic.id);
    }
    for (int i = 0; i < 2; i++)
        if (p4_streams[i].id && (p4_streams[i].epoch != p4_epoch || atomic_load(&p4_canceled[i]) == p4_streams[i].id)) {
            finish(&p4_streams[i], i == 1, 0);
            atomic_store(&p4_published[i], 0);
            memset(&p4_streams[i], 0, sizeof(p4_streams[i]));
        }
    if (p4_mic.id && (changed || atomic_load(&p4_mic_cancel) == p4_mic.id)) {
        if (p4_streams[1].id && p4_streams[1].status.state != LAP_OPERATION_DONE)
            finish(&p4_streams[1], true, 0);
#if LAP_DEVICE
        if (p4_mic.acquired)
            audio_out_capture_release();
#endif
        atomic_store(&p4_mic_published, 0);
        memset(&p4_mic, 0, sizeof(p4_mic));
        ls_lap_resource_claim(LS_LAP_LEASE_MIC,false);
        atomic_store(&mic_indicator, false);
    }
    if (p4_streams[1].id && atomic_load(&p4_record_stop) == p4_streams[1].id && p4_streams[1].status.state != LAP_OPERATION_DONE)
        finish(&p4_streams[1], true, 0);
    bool busy = p4_mic.id != 0;
    if (p4_streams[0].id && p4_streams[0].status.state != LAP_OPERATION_DONE) {
        struct stream *s = &p4_streams[0];
        busy = true;
        if (!s->started) {
            int r = wav_open(s);
            s->started = true;
            if (r)
                finish(s, false, r);
            else {
                p4_mix[s->channel].enabled = true;
                s->status.state = LAP_OPERATION_READY;
            }
        }
        if (s->file) {
            unsigned room = MIX_N - (p4_mix[s->channel].head - p4_mix[s->channel].tail), n = s->remaining / 2;
            if (n > 512)
                n = 512;
            if (n > room)
                n = room;
            static EXT_RAM_BSS_ATTR uint8_t data[1024];
            if (n && fread(data, 2, n, s->file) != n)
                finish(s, false, LAP_IO_EIO);
            else if (n) {
                for (unsigned j = 0; j < n; j++)
                    p4_mix[s->channel].pcm[p4_mix[s->channel].head++ % MIX_N] =
                        (int16_t)(data[2 * j] | (unsigned)data[2 * j + 1] << 8);
                s->remaining -= n * 2;
                s->bytes += n * 2;
                s->status.transferred = s->bytes;
                if (!s->remaining)
                    finish(s, false, 0);
            }
        }
    }
    if (p4_mic.id && !p4_mic.acquired && !p4_mic.status.error) {
#if LAP_DEVICE
        int r = audio_out_capture_acquire();
        if (r || !ls_audio_hw_has_mic()) {
            if (!r)
                audio_out_capture_release();
            p4_mic.status.error = r ? LAP_IO_EBUSY : LAP_IO_ENOTSUP;
            if (p4_streams[1].id && p4_streams[1].status.state != LAP_OPERATION_DONE)
                finish(&p4_streams[1], true, p4_mic.status.error);
            p4_mic.status.state = LAP_OPERATION_DONE;
            atomic_store(&mic_indicator, false);
        } else {
            p4_mic.acquired = true;
            p4_mic.status.state = LAP_OPERATION_READY;
        }
#else
        p4_mic.acquired = true;
        p4_mic.status.state = LAP_OPERATION_READY;
#endif
    }
    uint32_t now = ls_lap_av_now();
    unsigned elapsed = now - p4_paced;
    unsigned n = elapsed > 32 ? 512 : elapsed * 16;
    if (n)
        p4_paced = now;
    if (p4_mic.acquired && !p4_mic.status.error && n) {
        static EXT_RAM_BSS_ATTR int16_t pcm[512];
        unsigned got = n;
#if LAP_DEVICE
        static EXT_RAM_BSS_ATTR int16_t stereo[1024];
        size_t bytes = 0;
        if (ls_audio_hw_read(stereo, n * 4, &bytes) != ESP_OK) {
            p4_mic.status.error = LAP_IO_EIO;
            p4_mic.status.state = LAP_OPERATION_DONE;
            audio_out_capture_release();
            p4_mic.acquired = false;
            atomic_store(&mic_indicator, false);
            if (p4_streams[1].id && p4_streams[1].status.state != LAP_OPERATION_DONE)
                finish(&p4_streams[1], true, LAP_IO_EIO);
            got = 0;
        } else {
            got = bytes / 4;
            for (unsigned j = 0; j < got; j++)
                pcm[j] = stereo[j * 2];
        }
#else
        for (unsigned j = 0; j < got; j++)
            pcm[j] = ((p4_mic.total + j) % 32 < 16) ? sim_mic_level : -sim_mic_level;
        if (sim_input) {
            got = fread(pcm, sizeof(*pcm), n, sim_input);
            if (!got) {
                clearerr(sim_input);
                fseek(sim_input, 44, SEEK_SET);
            }
        }
#endif
        if (p4_streams[1].id && p4_streams[1].status.state != LAP_OPERATION_DONE) {
            struct stream *s = &p4_streams[1];
            if (!s->started) {
                s->started = true;
                s->file = fopen(s->path, "wb");
                if (!s->file || !header(s->file, 0))
                    finish(s, true, LAP_IO_EIO);
                else {
                    s->status.state = LAP_OPERATION_READY;
                    fseek(s->file, 44, SEEK_SET);
                }
            }
            if (s->file) {
                unsigned k = got;
                if (k > (s->limit - s->bytes) / 2)
                    k = (s->limit - s->bytes) / 2;
                static EXT_RAM_BSS_ATTR uint8_t data[1024];
                for (unsigned j = 0; j < k; j++) {
                    data[2 * j] = pcm[j];
                    data[2 * j + 1] = (uint16_t)pcm[j] >> 8;
                }
                unsigned written = fwrite(data, 2, k, s->file);
                s->bytes += written * 2;
                s->status.transferred = s->bytes;
                if (written != k)
                    finish(s, true, LAP_IO_EIO);
                else if (s->bytes == s->limit)
                    finish(s, true, 0);
            }
        }
        for (unsigned j = 0; j < got; j++) {
            if (p4_mic.head - p4_mic.tail == MIC_N)
                p4_mic.status.dropped++;
            else
                p4_mic.pcm[p4_mic.head++ % MIC_N] = pcm[j];
        }
        p4_mic.total += got;
        p4_mic.status.transferred = p4_mic.total;
    }
    static EXT_RAM_BSS_ATTR int16_t output[512];
    /* One hardware-rate mix across every resident, regardless of which slot
       the IO scheduler visits first. Each channel consumes exactly once. */
    unsigned mix_elapsed=now-mixed_at;
    n=mix_elapsed>32?512:mix_elapsed*16;
    if(n) mixed_at=now;
    bool any = false;
#if LAP_DEVICE
    bool dropping = audio_is_muted() || audio_out_live_active() || audio_out_ring_avail() >= 6400 ||
                    !audio_out_app_cues_available();
#else
    bool dropping = sim_muted || sim_live;
#endif
    for (unsigned j = 0; j < n; j++) {
        int32_t sum = 0;
        bool voice=false;
        for(unsigned s=0;s<LS_LAP_CONTEXTS;s++) if(atomic_load(&ls_lap_av_actives[s]))
            voice=voice||p4_mix_slots[s][2].head!=p4_mix_slots[s][2].tail;
        for(unsigned s=0;s<LS_LAP_CONTEXTS;s++) if(atomic_load(&ls_lap_av_actives[s]))
        for (int i = 0; i < 3; i++) {
            mix_channel *m=&p4_mix_slots[s][i];
            if (m->head != m->tail) {
                any = true;
                m->consumed++;
                if (dropping)
                    m->dropped++;
                int volume = m->volume;
                if (i != 2 && voice)
                    volume = volume * m->duck / 100;
                sum += (int32_t)m->pcm[m->tail++ % MIX_N] * volume / 100;
            }
        }
        output[j] = sum > 32767 ? 32767 : sum < -32768 ? -32768 : sum;
    }
#if LAP_DEVICE
    if (any && !dropping) {
        audio_write_cue(output, n);
        audio_out_play_now();
    }
#else
    if (n) {
        host_audio.count = n;
        host_audio.frames += n;
        memcpy(host_audio.pcm, output, n * sizeof(*output));
    }
    if (sim_output && n && any)
        fwrite(output, sizeof(*output), n, sim_output);
#endif
    for(unsigned s=0;s<LS_LAP_CONTEXTS;s++) if(atomic_load(&ls_lap_av_actives[s]))
        for (int i = 0; i < 3; i++) busy=busy||p4_mix_slots[s][i].head!=p4_mix_slots[s][i].tail;
    av_give(&lock);
    return busy || any;
}

#include "ls_lap_usage.h"
ls_lap_usage_t ls_lap_sound_usage(unsigned s) {
    ls_lap_usage_t v={sizeof(p4_mix_slots[s])+sizeof(p4_mic_slots[s])+sizeof(p4_streams_slots[s]),atomic_load(&p4_mic_published_slots[s])>0};
    for(int i=0;i<2;i++) v.handles+=atomic_load(&p4_published_slots[s][i])>0;
    for(int i=0;i<3;i++) v.handles+=p4_mix_slots[s][i].enabled;
    return v;
}
