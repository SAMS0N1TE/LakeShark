/* MORSE: a Morse (CW / on-off keyed carrier) decoder written as an app.
 *
 * The SDK 2.0 example of a decoder of your own. It opens a receive session,
 * reads every I/Q block with lap_rx_read, and does the whole decode itself:
 *   1. mixes the chosen tone to 0 Hz with a rotating phasor,
 *   2. integrates 1 ms at a time (a boxcar filter about 1 kHz wide) into an
 *      envelope,
 *   3. keys on/off against a threshold between a slow noise floor and a peak
 *      tracker, with hysteresis,
 *   4. times the marks and gaps against an adaptive dot length, and
 *   5. looks the dots and dashes up in the ITU table.
 * It also shows the other half of the audio API: TONE plays the decoded
 * keying as a 700 Hz sidetone through lap_audio_write.
 *
 * Reference: ITU-R M.1677-1 (International Morse code) for the alphabet and
 * the 1:3:7 element, letter and word spacing. The envelope and timing are
 * written for this example.
 *
 * Taps: the strip under the header is the spectrum; tap a peak to listen to
 * it. Keys along the bottom: RADIO r, << , and >> . to step, TONE s, CLEAR c. */
#define LAP_TARGET_API_20     /* uses nothing from 2.1: keeps running on 2.0 firmware */
#include "lakeshark_app.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define A(fg) LAP_ATTR((fg), LAP_BLACK)
#define HIST 512                     /* timeline columns, 20 ms each */
#define TEXT 480
#define READ_PAIRS 2048

LAP_ICON_PX("....................",
            "....................",
            "...........bb.......",
            "..........b..b......",
            "..........b..b......",
            "...........bb.......",
            "............e.......",
            "............e.......",
            "..8888888888888888..",
            "..8..............8..",
            "..8888888888888888..",
            "....................",
            "..aa.aaaa.aa.aa.aaaa",
            "....................",
            "..ee.ee.eeee.ee.ee..",
            "....................",
            "....................",
            "....................");

static struct {
    lap_radio_info_t radios[LAP_RADIOS_MAX];
    int nradios, radio, h;
    lap_rx_config_t cfg;
    lap_rx_status_t st;
    char why[48];
    uint64_t center;
    int32_t offset;                  /* the tone, Hz from the centre */
    bool offset_chosen, tone;
    int16_t *iq;                     /* READ_PAIRS pairs */
    float *spec, *hold;              /* spectrum strip */
    int bins;
    uint32_t spec_seq, lines;
    /* mixer and integrator */
    float pr, pi, wr, wi;            /* phasor and its step */
    float ar, ai;
    uint32_t acc_n, per_ms, renorm;
    /* envelope and keying */
    float floor_e, peak_e, env;
    bool key;
    uint32_t run_ms;                 /* length of the current mark or gap */
    float dot_ms;
    char sym[8];
    int nsym;
    bool letter_done, word_done;
    /* what it decoded */
    char text[TEXT + 1];
    int ntext;
    char last;
    /* the timeline: per 20 ms, the peak envelope (0..7) and whether keyed */
    uint8_t lvl[HIST];
    bool on[HIST];
    int head, sub;
    float sub_peak;
    bool sub_on;
    /* the sidetone */
    int16_t pcm[1024];
    int npcm;
    float tone_ph;
    /* layout */
    int strip_y, strip_h, keys_y;
} s;

/* ---- the alphabet ------------------------------------------------------ */

static const struct { const char *code; char c; } MORSE[] = {
    {".-", 'A'}, {"-...", 'B'}, {"-.-.", 'C'}, {"-..", 'D'}, {".", 'E'}, {"..-.", 'F'}, {"--.", 'G'},
    {"....", 'H'}, {"..", 'I'}, {".---", 'J'}, {"-.-", 'K'}, {".-..", 'L'}, {"--", 'M'}, {"-.", 'N'},
    {"---", 'O'}, {".--.", 'P'}, {"--.-", 'Q'}, {".-.", 'R'}, {"...", 'S'}, {"-", 'T'}, {"..-", 'U'},
    {"...-", 'V'}, {".--", 'W'}, {"-..-", 'X'}, {"-.--", 'Y'}, {"--..", 'Z'},
    {"-----", '0'}, {".----", '1'}, {"..---", '2'}, {"...--", '3'}, {"....-", '4'},
    {".....", '5'}, {"-....", '6'}, {"--...", '7'}, {"---..", '8'}, {"----.", '9'},
    {".-.-.-", '.'}, {"--..--", ','}, {"..--..", '?'}, {"-..-.", '/'}, {"-...-", '='}, {".-.-.", '+'},
};

static void put_char(char c)
{
    if (c == ' ' && (!s.ntext || s.text[s.ntext - 1] == ' ')) return;
    if (s.ntext >= TEXT) {
        memmove(s.text, s.text + TEXT / 2, TEXT / 2);
        s.ntext -= TEXT / 2;
    }
    s.text[s.ntext++] = c;
    s.text[s.ntext] = 0;
    if (c != ' ') s.last = c;
}

static void end_letter(void)
{
    if (!s.nsym) return;
    s.sym[s.nsym] = 0;
    char c = '*';                    /* heard, but not a letter */
    for (unsigned i = 0; i < sizeof(MORSE) / sizeof(MORSE[0]); i++)
        if (!strcmp(MORSE[i].code, s.sym)) { c = MORSE[i].c; break; }
    put_char(c);
    s.nsym = 0;
}

/* ---- one millisecond of envelope ----------------------------------------- */

static void sidetone(bool on)
{
    if (!s.tone) return;
    for (int i = 0; i < 16 && s.npcm < (int)(sizeof(s.pcm) / sizeof(s.pcm[0])); i++) {
        s.tone_ph += 6.2831853f * 700.0f / 16000.0f;
        if (s.tone_ph > 6.2831853f) s.tone_ph -= 6.2831853f;
        s.pcm[s.npcm++] = on ? (int16_t)(5000.0f * sinf(s.tone_ph)) : 0;
    }
}

static void envelope_ms(float e)
{
    s.env = e;
    /* Floor: follows quiet quickly, rises slowly. Peak: jumps up, sinks slowly. */
    if (e < s.floor_e || s.floor_e <= 0) s.floor_e = e; else s.floor_e += (e - s.floor_e) * 0.0005f;
    if (e > s.peak_e) s.peak_e = e; else s.peak_e -= (s.peak_e - s.floor_e) * 0.0005f;
    const float thr = s.floor_e + (s.peak_e - s.floor_e) * (s.key ? 0.40f : 0.55f);
    const bool snr_ok = s.peak_e > s.floor_e * 3.0f;
    const bool key = snr_ok && e > thr;
    if (key != s.key) {
        if (s.key) {                 /* a mark ended */
            if (s.run_ms >= 12) {
                const bool dash = s.run_ms > 2.0f * s.dot_ms;
                if (s.nsym < 7) s.sym[s.nsym++] = dash ? '-' : '.';
                const float unit = dash ? s.run_ms / 3.0f : (float)s.run_ms;
                s.dot_ms = s.dot_ms * 0.8f + unit * 0.2f;
                if (s.dot_ms < 20) s.dot_ms = 20;
                if (s.dot_ms > 300) s.dot_ms = 300;
            }
            s.letter_done = s.word_done = false;
        }
        s.key = key;
        s.run_ms = 0;
    }
    s.run_ms++;
    if (!s.key) {
        if (!s.letter_done && s.run_ms > 2.5f * s.dot_ms) { end_letter(); s.letter_done = true; }
        if (!s.word_done && s.run_ms > 6.0f * s.dot_ms && s.ntext) { put_char(' '); s.word_done = true; }
    }
    sidetone(s.key);
    /* the timeline, 20 ms a column */
    if (e > s.sub_peak) s.sub_peak = e;
    s.sub_on |= s.key;
    if (++s.sub >= 20) {
        const float span = s.peak_e - s.floor_e;
        float t = span > 0 ? (s.sub_peak - s.floor_e) / span : 0;
        if (t < 0) t = 0;
        if (t > 1) t = 1;
        s.head = (s.head + 1) % HIST;
        s.lvl[s.head] = (uint8_t)(t * 255.0f + 0.5f);
        s.on[s.head] = s.sub_on;
        s.sub = 0; s.sub_peak = 0; s.sub_on = false;
    }
}

/* Mix the tone down and integrate 1 ms at a time. */
static void process(const int16_t *iq, int n)
{
    for (int i = 0; i < n; i++) {
        const float x = iq[2 * i], y = iq[2 * i + 1];
        s.ar += x * s.pr - y * s.pi;
        s.ai += x * s.pi + y * s.pr;
        const float r = s.pr * s.wr - s.pi * s.wi;
        s.pi = s.pr * s.wi + s.pi * s.wr;
        s.pr = r;
        if (++s.renorm >= 1024) {    /* keep the phasor on the unit circle */
            const float m = 1.0f / sqrtf(s.pr * s.pr + s.pi * s.pi);
            s.pr *= m; s.pi *= m; s.renorm = 0;
        }
        if (++s.acc_n >= s.per_ms) {
            envelope_ms(sqrtf(s.ar * s.ar + s.ai * s.ai) / (float)s.acc_n);
            s.ar = s.ai = 0;
            s.acc_n = 0;
        }
    }
}

static void set_offset(int32_t hz)
{
    const uint32_t rate = s.st.rate_hz ? s.st.rate_hz : s.cfg.rate_hz;
    s.offset = hz;
    const float w = -6.2831853f * (float)hz / (float)(rate ? rate : 48000);
    s.wr = cosf(w); s.wi = sinf(w);
    s.pr = 1; s.pi = 0;
    s.per_ms = rate / 1000 ? rate / 1000 : 1;
    s.ar = s.ai = 0; s.acc_n = 0;
    s.floor_e = s.peak_e = 0;
}

/* ---- the radio ------------------------------------------------------- */

static void close_rx(void)
{
    if (s.h > 0) lap_rx_close(s.h);
    s.h = 0;
}

static void open_rx(void)
{
    close_rx();
    if (s.radio < 0 || s.radio >= s.nradios) return;
    const lap_radio_info_t *r = &s.radios[s.radio];
    if (!r->present || r->busy || !(r->features & LAP_RXF_IQ)) {
        snprintf(s.why, sizeof(s.why), "%s: %s", r->name, r->why[0] ? r->why : "no I/Q");
        return;
    }
    if (s.center < r->min_hz || s.center > r->max_hz) s.center = (r->min_hz + r->max_hz) / 2;
    /* The lowest rate the radio has: the least work for the decoder. */
    s.cfg = (lap_rx_config_t){ s.center, r->min_rate, LAP_GAIN_AUTO, 0 };
    s.h = lap_rx_open(s.radio, &s.cfg);
    if (s.h < 0) snprintf(s.why, sizeof(s.why), "%s would not open (%d)", r->name, s.h);
    else s.why[0] = 0;
    s.offset_chosen = false;
    s.lines = 0;
    for (int i = 0; s.hold && i < 512; i++) s.hold[i] = -200;
    set_offset(0);
}

static bool usable(const lap_radio_info_t *r) { return r->present && !r->busy && (r->features & LAP_RXF_IQ); }

static void next_radio(void)
{
    s.nradios = lap_radio_list(s.radios, LAP_RADIOS_MAX);
    if (s.nradios <= 0) return;
    for (int k = 1; k <= s.nradios; k++) {
        const int i = (s.radio + k) % s.nradios;
        if (usable(&s.radios[i]) || k == s.nradios) { s.radio = i; break; }
    }
    open_rx();
}

static void step(int dir)
{
    if (s.h <= 0) return;
    const lap_radio_info_t *r = &s.radios[s.radio];
    const uint32_t rate = s.st.rate_hz ? s.st.rate_hz : s.cfg.rate_hz;
    int64_t hz = (int64_t)s.center + dir * (int64_t)rate / 2;
    if (hz < (int64_t)r->min_hz || hz > (int64_t)r->max_hz) return;
    if (lap_rx_tune(s.h, (uint64_t)hz) == 0) {
        s.center = (uint64_t)hz;
        s.offset_chosen = false;
        s.lines = 0;
        for (int i = 0; i < 512; i++) s.hold[i] = -200;
    }
}

/* Read everything waiting (within a frame's budget) and decode it. */
static void pump(void)
{
    lap_iq_meta_t meta;
    for (int budget = 0; budget < 40; budget++) {
        const int n = lap_rx_read(s.h, s.iq, READ_PAIRS, &meta);
        if (n <= 0) break;
        if (meta.flags & (LAP_IQ_GAP | LAP_IQ_RETUNED)) { s.ar = s.ai = 0; s.acc_n = 0; }
        if (meta.rate_hz && meta.rate_hz / 1000 != s.per_ms) set_offset(s.offset);
        process(s.iq, n);
    }
    if (s.npcm) {
        const int took = lap_audio_write(s.pcm, s.npcm);
        if (took > 0 && took < s.npcm) memmove(s.pcm, s.pcm + took, (size_t)(s.npcm - took) * 2);
        s.npcm = took > 0 ? s.npcm - took : 0;
    }
}

/* The strongest steady signal off the centre, from the held spectrum. */
static void auto_offset(void)
{
    const uint32_t rate = s.st.rate_hz ? s.st.rate_hz : s.cfg.rate_hz;
    int best = -1;
    for (int i = 0; i < s.bins; i++) {
        if (i > s.bins / 2 - s.bins / 40 && i < s.bins / 2 + s.bins / 40) continue;   /* not the centre spur */
        if (best < 0 || s.hold[i] > s.hold[best]) best = i;
    }
    if (best >= 0) set_offset((int32_t)(((int64_t)(2 * best + 1) * rate) / (2 * s.bins) - rate / 2));
}

/* ---- the screen ------------------------------------------------------- */

static const uint8_t BIT[2][4] = { { 0x01, 0x02, 0x04, 0x40 }, { 0x08, 0x10, 0x20, 0x80 } };

static void draw_strip(int x, int w)
{
    float lo = 0, hi = -200;
    for (int i = 0; i < s.bins; i++) { if (i == 0 || s.spec[i] < lo) lo = s.spec[i]; if (s.spec[i] > hi) hi = s.spec[i]; }
    if (hi - lo < 20) hi = lo + 20;
    const int dots = s.strip_h * 4;
    const uint32_t rate = s.st.rate_hz ? s.st.rate_hz : s.cfg.rate_hz;
    const int mark = rate ? (int)(((int64_t)s.offset + rate / 2) * (w * 2) / rate) / 2 : -1;
    for (int cx = 0; cx < w; cx++)
        for (int cy = 0; cy < s.strip_h; cy++) {
            uint8_t b = 0;
            for (int k = 0; k < 2; k++) {
                const int i = (cx * 2 + k) * s.bins / (w * 2);
                const int top = s.lines ? dots - 1 - (int)((s.spec[i] - lo) / (hi - lo) * (float)(dots - 1)) : dots;
                for (int r = 0; r < 4; r++) if (cy * 4 + r >= top) b |= BIT[k][r];
            }
            const uint8_t col = cx == mark ? LAP_YELLOW | LAP_BRIGHT : cy == s.strip_h - 1 ? LAP_BLUE | LAP_BRIGHT : LAP_CYAN;
            lap_put_cell(x + cx, s.strip_y + cy, b ? LAP_BRAILLE(b) : ' ', A(col));
        }
    if (mark >= 0 && mark < w) lap_put_cell(x + mark, s.strip_y - 1, 'v', A(LAP_YELLOW | LAP_BRIGHT));
}

static void draw_timeline(int x, int y, int w, int rows)
{
    /* Rows of braille, 4 dots of envelope height each, newest on the right;
       keyed time in green, the gaps between in blue. */
    const int dots = rows * 4;
    for (int cx = 0; cx < w; cx++) {
        int level[2];
        bool on = false;
        for (int k = 0; k < 2; k++) {
            const int i = (s.head - (w * 2 - 1 - (cx * 2 + k)) + HIST * 4) % HIST;
            level[k] = (s.lvl[i] * dots + 127) / 255;
            on |= s.on[i];
        }
        const uint8_t col = on ? LAP_GREEN | LAP_BRIGHT : LAP_BLUE;
        for (int row = 0; row < rows; row++) {
            uint8_t b = 0;
            for (int k = 0; k < 2; k++)
                for (int r = 0; r < 4; r++)
                    if (level[k] >= dots - (row * 4 + r)) b |= BIT[k][r];
            const bool base = row == rows - 1;
            lap_put_cell(x + cx, y + row, b ? LAP_BRAILLE(b) : base ? '.' : ' ', A(b || !base ? col : LAP_BLUE | LAP_BRIGHT));
        }
    }
}

static void meter(int x, int y, int w, float t, uint8_t col)
{
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    const int n = (int)(t * (float)(w - 2) + 0.5f);
    lap_put_cell(x, y, '[', A(LAP_WHITE));
    for (int i = 0; i < w - 2; i++) lap_put_cell(x + 1 + i, y, i < n ? '=' : '-', A(i < n ? col : LAP_BLUE));
    lap_put_cell(x + w - 1, y, ']', A(LAP_WHITE));
}

static void key_box(int x, int w, const char *label, const char *hint, uint8_t col, bool lit)
{
    const int y = s.keys_y;
    if (lit) lap_fill(x, y, w, 3, ' ', LAP_ATTR(LAP_BLACK, col));
    lap_box(x, y, w, 3, NULL, lit ? LAP_ATTR(LAP_BLACK, col) : A(col));
    char t[24];
    snprintf(t, sizeof(t), "%s %s", label, hint);
    const int n = (int)strlen(t) < w - 2 ? (int)strlen(t) : w - 2;
    t[n] = 0;
    lap_put_str(x + (w - n) / 2, y + 1, t, lit ? LAP_ATTR(LAP_BLACK, col) : A(LAP_WHITE | LAP_BRIGHT));
    if (!lit) lap_put_str(x + (w - n) / 2 + (int)strlen(label) + 1, y + 1, hint, A(col | LAP_BRIGHT));
}

static void draw(void)
{
    const int W = lap_cols(), H = lap_rows();
    lap_clear(A(LAP_WHITE));
    if (!s.iq) {
        s.iq = malloc(READ_PAIRS * 4);
        s.spec = calloc(512, sizeof(float));
        s.hold = calloc(512, sizeof(float));
        for (int i = 0; s.hold && i < 512; i++) s.hold[i] = -200;
        if (!s.iq || !s.spec || !s.hold) { lap_put_str(1, 1, "out of memory", A(LAP_RED)); return; }
    }
    if (!s.nradios || !(lap_frame() % 25)) {
        const int n = lap_radio_list(s.radios, LAP_RADIOS_MAX);
        if (n >= 0) s.nradios = n;
        if (s.nradios && s.radio < 0) next_radio();
    }
    /* Taller graphs when the screen is tall (portrait), the same layout otherwise. */
    const int tl_rows = H >= 48 ? 4 : 2;
    s.strip_y = 3; s.strip_h = H >= 48 ? 5 : 3; s.keys_y = H - 3;
    const int sx = 1, sw = W - 2;
    if (s.h > 0) {
        lap_rx_status(s.h, &s.st);
        if (s.st.state == LAP_RX_RUNNING) {
            pump();
            s.bins = sw * 2 > 512 ? 512 : sw * 2;
            uint32_t seq;
            if (lap_rx_spectrum(s.h, s.spec, s.bins, &seq) == s.bins && seq != s.spec_seq) {
                s.spec_seq = seq;
                s.lines++;
                for (int i = 0; i < s.bins; i++) if (s.spec[i] > s.hold[i]) s.hold[i] = s.spec[i];
                if (!s.offset_chosen && s.lines == 12) auto_offset();
            }
        } else if (s.st.state < 0)
            snprintf(s.why, sizeof(s.why), "the radio stopped (%d): tap RADIO", s.st.state);
    }

    /* Header: a straight key, and what is tuned. */
    lap_put_str(0, 0, " _o_ ", A(LAP_YELLOW | LAP_BRIGHT));
    lap_put_str(0, 1, "[___]", A(LAP_WHITE));
    lap_put_str(6, 0, "MORSE", A(LAP_GREEN | LAP_BRIGHT));
    lap_put_str(12, 0, "-- --- .-. ... .", A(LAP_GREEN));
    const lap_radio_info_t *r = s.radio >= 0 && s.radio < s.nradios ? &s.radios[s.radio] : NULL;
    char t[64];
    snprintf(t, sizeof(t), "%s", r ? r->name : "no radio");
    lap_put_str(W - (int)strlen(t) - 1, 0, t, A(LAP_MAGENTA | LAP_BRIGHT));
    const uint64_t tone_hz = s.center + (int64_t)s.offset;
    snprintf(t, sizeof(t), "%lu.%03lu.%03lu MHz", (unsigned long)(tone_hz / 1000000u),
             (unsigned long)(tone_hz / 1000u % 1000u), (unsigned long)(tone_hz % 1000u));
    lap_put_str(6, 1, t, A(LAP_WHITE | LAP_BRIGHT));
    snprintf(t, sizeof(t), "tone %+ld Hz", (long)s.offset);
    lap_put_str(W - (int)strlen(t) - 1, 1, t, A(LAP_CYAN));

    if (s.h <= 0 || s.st.state < 0) {
        const char *m = s.why[0] ? s.why : "tap RADIO to choose a radio";
        lap_put_str((W - (int)strlen(m)) / 2 > 0 ? (W - (int)strlen(m)) / 2 : 0, H / 2, m, A(LAP_RED | LAP_BRIGHT));
    } else {
        draw_strip(sx, sw);
        lap_hline(sx, s.strip_y + s.strip_h, sw, '-', A(LAP_BLUE));
        /* Lamp, keying and timing. */
        const int y = s.strip_y + s.strip_h + 1;
        const uint8_t lamp = s.key ? LAP_GREEN | LAP_BRIGHT : LAP_BLACK | LAP_BRIGHT;
        lap_put_str(sx, y, s.key ? "(#)" : "( )", A(lamp));
        lap_put_str(sx + 4, y, s.key ? "KEY DOWN" : "key up  ", A(s.key ? LAP_GREEN | LAP_BRIGHT : LAP_BLACK | LAP_BRIGHT));
        s.sym[s.nsym] = 0;
        lap_put_str(sx + 14, y, s.sym, A(LAP_YELLOW | LAP_BRIGHT));
        snprintf(t, sizeof(t), "%2d WPM", s.dot_ms > 0 ? (int)(1200.0f / s.dot_ms + 0.5f) : 0);
        lap_put_str(W - (int)strlen(t) - 1, y, t, A(LAP_CYAN | LAP_BRIGHT));
        draw_timeline(sx, y + 1, sw, tl_rows);
        const float snr = s.floor_e > 0 ? 8.6858896f * logf(s.peak_e / s.floor_e) : 0;   /* 20 log10 */
        const int my = y + 1 + tl_rows;
        lap_put_str(sx, my, "SNR", A(LAP_WHITE));
        meter(sx + 4, my, sw / 2 - 4, snr / 40.0f, snr > 10 ? LAP_GREEN | LAP_BRIGHT : LAP_YELLOW);
        snprintf(t, sizeof(t), "%2d dB", (int)snr);
        lap_put_str(sx + sw / 2 + 1, my, t, A(snr > 10 ? LAP_GREEN : LAP_YELLOW));
        /* The newest letters large, as a headline, when there is room for them. */
        int ty = my + 2;
        if (s.keys_y - ty - 1 >= 12) {
            const int fit = (W - 2) / 4;
            int n = 0, first = s.ntext;
            while (first > 0 && n < fit) { first--; n++; }
            for (int i = 0; i < n; i++) {
                const char c = s.text[first + i];
                const bool newest = first + i == s.ntext - 1;
                if (c != ' ') lap_big_char(1 + i * 4, ty, c, A(newest ? LAP_YELLOW | LAP_BRIGHT : LAP_GREEN));
            }
            ty += 7;
        }
        const int th = s.keys_y - ty - 1;
        const int tw = W - 2 < 120 ? W - 2 : 120;
        if (th > 0 && tw > 4) {
            lap_box(0, ty - 1, W, th + 2 > 3 ? th + 2 : 3, "HEARD", A(LAP_BLUE | LAP_BRIGHT));
            const int lines = th, total = (s.ntext + tw - 1) / tw;
            for (int l = 0; l < lines; l++) {
                const int line = total - lines + l;
                if (line < 0) continue;
                char row[128];
                const int n = s.ntext - line * tw < tw ? s.ntext - line * tw : tw;
                memcpy(row, s.text + line * tw, (size_t)n);
                row[n] = 0;
                lap_put_str(1, ty + l, row, A(line == total - 1 ? LAP_WHITE | LAP_BRIGHT : LAP_GREEN));
            }
            if (!s.ntext) lap_put_str(2, ty, "listening for keying...", A(LAP_BLACK | LAP_BRIGHT));
        }
    }
    const int kw = W / 5;
    key_box(0, kw, "RADIO", "r", LAP_MAGENTA, false);
    key_box(kw, kw, "<<", ",", LAP_CYAN, false);
    key_box(2 * kw, kw, ">>", ".", LAP_CYAN, false);
    key_box(3 * kw, kw, "TONE", "s", LAP_YELLOW, s.tone);
    key_box(4 * kw, W - 4 * kw, "CLEAR", "c", LAP_RED, false);
}

static bool key(lap_key_t k, char ch)
{
    if (k == LAP_KEY_LEFT || (k == LAP_KEY_CHAR && ch == ',')) { step(-1); return true; }
    if (k == LAP_KEY_RIGHT || (k == LAP_KEY_CHAR && ch == '.')) { step(1); return true; }
    if (k == LAP_KEY_CHAR && ch == 'r') { next_radio(); return true; }
    if (k == LAP_KEY_CHAR && ch == 's') { s.tone = !s.tone; s.npcm = 0; return true; }
    if (k == LAP_KEY_CHAR && ch == 'c') { s.ntext = 0; s.text[0] = 0; s.last = 0; s.nsym = 0; return true; }
    return false;
}

static bool touch(int col, int row)
{
    const int W = lap_cols();
    if (row >= s.keys_y) {
        const int i = col / (W / 5) < 5 ? col / (W / 5) : 4;
        static const char KEYS[] = { 'r', ',', '.', 's', 'c' };
        return key(LAP_KEY_CHAR, KEYS[i]);
    }
    /* The spectrum strip: listen to what was tapped. */
    if (row >= s.strip_y - 1 && row < s.strip_y + s.strip_h && col >= 1 && col < W - 1 && s.h > 0) {
        const uint32_t rate = s.st.rate_hz ? s.st.rate_hz : s.cfg.rate_hz;
        const int w = W - 2;
        set_offset((int32_t)(((int64_t)(col - 1) * 2 + 1) * rate / (2 * w) - rate / 2));
        s.offset_chosen = true;
        return true;
    }
    return false;
}

static void enter(void)
{
    memset(&s, 0, sizeof(s));
    s.radio = -1;
    s.center = 433900000u;           /* the SIM radio keys its test carrier at +20 kHz */
    s.dot_ms = 60;
}

static void leave(void)
{
    close_rx();
    free(s.iq); free(s.spec); free(s.hold);
    memset(&s, 0, sizeof(s));
}

LAP_APP(.id = "morse", .name = "MORSE", .sub = "CW decoder on I/Q", .version = "1.0",
        .author = "LakeShark", .icon = LAP_ICON_RECORD, .color = LAP_GREEN, .flags = LAP_CAP_RADIO_RX,
        .icon_art = { "   _o_    ", "  [___]   ", "          ", " .- -. .-.", " -- --- . ", "  MORSE   " },
        .enter = enter, .leave = leave, .draw = draw, .key = key, .touch = touch);
