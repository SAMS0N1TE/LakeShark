/* RDS / RBDS data layer. See rds.h; references IEC 62106 annex B (check
   word, offsets, burst correction) and NRSC-4-B (RBDS PTY table, call-sign
   PI codes). */
#include "rds.h"

#include <string.h>

/* g(x) = x^10 + x^8 + x^7 + x^5 + x^4 + x^3 + 1 */
#define RDS_POLY 0x5B9u

static const uint16_t OFFSETS[5] = { 0x0FC, 0x198, 0x168, 0x1B4, 0x350 };

uint16_t rds_check(uint16_t data)
{
    uint32_t reg = (uint32_t)data << 10;
    for (int i = 25; i >= 10; --i)
        if (reg & (1u << i)) reg ^= RDS_POLY << (i - 10);
    return (uint16_t)(reg & 0x3FFu);
}

uint16_t rds_offset(int blk) { return blk >= 0 && blk < 5 ? OFFSETS[blk] : 0; }

uint32_t rds_block(uint16_t data, int blk)
{
    return ((uint32_t)data << 10) | (uint32_t)(rds_check(data) ^ rds_offset(blk));
}

/* The offset a 26-bit block carries, -1 for none. */
static int block_type(uint32_t reg)
{
    const uint16_t syn = (uint16_t)((reg & 0x3FFu) ^ rds_check((uint16_t)(reg >> 10)));
    for (int i = 0; i < 5; ++i) if (syn == OFFSETS[i]) return i;
    return -1;
}

static bool block_is(uint32_t reg, int want)
{
    const int t = block_type(reg);
    return t == want || (want == RDS_BLK_C && t == RDS_BLK_C2);
}

/* A received block against the offset expected there: as is, else with one
   bit or a burst of two flipped. Annex B's decoder corrects bursts up to
   five; two keeps the false-correction rate low on a weak signal. */
static int check_block(uint32_t reg, int want, uint16_t *data, int *fixed)
{
    *fixed = 0;
    if (block_is(reg, want)) { *data = (uint16_t)(reg >> 10); return block_type(reg); }
    for (int burst = 1; burst <= 2; ++burst) {
        const uint32_t pattern = burst == 1 ? 1u : 3u;
        for (int i = 0; i + burst <= 26; ++i) {
            const uint32_t tryreg = reg ^ (pattern << i);
            if (block_is(tryreg, want)) {
                *fixed = 1;
                *data = (uint16_t)(tryreg >> 10);
                return block_type(tryreg);
            }
        }
    }
    return -1;
}

static void card_clear(rds_info_t *in)
{
    const uint32_t seq = in->seq;
    memset(in, 0, sizeof(*in));
    in->seq = seq + 1;
}

void rds_reset(rds_t *r)
{
    rds_group_fn fn = r->on_group;
    void *user = r->user;
    const uint32_t seq = r->info.seq;
    memset(r, 0, sizeof(*r));
    r->info.seq = seq + 1;
    r->cand_blk = -1;
    r->rt_ab = -1;
    r->rt_end = -1;
    memset(r->ps_work, ' ', sizeof(r->ps_work));
    memset(r->rt_work, ' ', sizeof(r->rt_work));
    r->on_group = fn;
    r->user = user;
}

void rds_init(rds_t *r, rds_group_fn on_group, void *user)
{
    memset(r, 0, sizeof(*r));
    r->on_group = on_group;
    r->user = user;
    rds_reset(r);
}

/* E.1 character table to the TUI's ASCII: the accented Latin rows fold to
   their base letter, anything else unprintable to '?'. */
static char rds_char(uint8_t c)
{
    static const char fold[33] = "aaeeiioouuNCSs!Iaaeeiioouunc g?i";
    if (c >= 0x20 && c < 0x7F) return (char)c;
    if (c >= 0x80 && c < 0xA0) return fold[c - 0x80];
    return '?';
}

static void publish_rt(rds_t *r)
{
    rds_info_t *in = &r->info;
    const int len = r->rt_len_b ? 32 : RDS_RT_LEN;
    int end = r->rt_end >= 0 && r->rt_end < len ? r->rt_end : len;
    char text[RDS_RT_LEN + 1];
    memcpy(text, r->rt_work, (size_t)end);
    text[end] = 0;
    while (end > 0 && text[end - 1] == ' ') text[--end] = 0;
    /* Complete when every cell up to the end has arrived. */
    const int need = r->rt_end >= 0 ? r->rt_end : len;
    const uint64_t want = need >= 64 ? ~0ull : ((1ull << need) - 1ull);
    const bool complete = need > 0 && (r->rt_mask & want) == want;
    if (strcmp(text, in->rt) || complete != in->rt_complete) {
        memcpy(in->rt, text, (size_t)end + 1);
        in->rt_complete = complete;
        in->seq++;
    }
}

static void rt_put(rds_t *r, int pos, uint8_t c)
{
    const int len = r->rt_len_b ? 32 : RDS_RT_LEN;
    if (pos < 0 || pos >= len) return;
    if (c == 0x0D) {
        if (r->rt_end < 0 || pos < r->rt_end) r->rt_end = (int8_t)pos;
        /* Everything after the terminator counts as received. */
        for (int i = pos; i < len; ++i) r->rt_mask |= 1ull << i;
        return;
    }
    r->rt_work[pos] = rds_char(c);
    r->rt_mask |= 1ull << pos;
}

static void af_add(rds_info_t *in, uint8_t code)
{
    if (code < 1 || code > 204) return;
    for (int i = 0; i < in->af_n; ++i) if (in->af[i] == code) return;
    if (in->af_n >= RDS_AF_MAX) return;
    in->af[in->af_n++] = code;
    in->seq++;
}

/* Modified Julian Day to Unix days: MJD 40587 is 1970-01-01. */
#define RDS_MJD_UNIX 40587

void rds_decode_group(rds_t *r, const uint16_t blk[4], uint8_t ok)
{
    rds_info_t *in = &r->info;
    in->groups++;
    if (r->on_group) r->on_group(r->user, blk, ok);

    if (ok & 1) {
        if (in->has_pi && in->pi != blk[0]) {
            /* A new station: the old card means nothing now. */
            card_clear(in);
            r->ps_mask = 0;
            r->rt_mask = 0;
            r->rt_end = -1;
            r->rt_ab = -1;
            memset(r->ps_work, ' ', sizeof(r->ps_work));
            memset(r->rt_work, ' ', sizeof(r->rt_work));
        }
        if (!in->has_pi) { in->has_pi = true; in->seq++; }
        in->pi = blk[0];
    }
    in->synced = r->synced;
    if (!(ok & 2)) return;

    const uint16_t b = blk[1];
    const int type = b >> 12;
    const bool ver_b = (b >> 11) & 1;
    const bool tp = (b >> 10) & 1;
    const uint8_t pty = (uint8_t)((b >> 5) & 0x1F);
    if (tp != in->tp || pty != in->pty) { in->tp = tp; in->pty = pty; in->seq++; }
    in->type_seen |= (uint16_t)(1u << type);
    if (ver_b && (ok & 4) && !(ok & 1)) { in->pi = blk[2]; in->has_pi = true; }

    switch (type) {
    case 0: {
        const bool ta = (b >> 4) & 1, ms = (b >> 3) & 1;
        if (ta != in->ta || ms != in->music) { in->ta = ta; in->music = ms; in->seq++; }
        if (!ver_b && (ok & 4)) {
            af_add(in, (uint8_t)(blk[2] >> 8));
            af_add(in, (uint8_t)(blk[2] & 0xFF));
        }
        if (ok & 8) {
            const int seg = b & 3;
            r->ps_work[seg * 2]     = rds_char((uint8_t)(blk[3] >> 8));
            r->ps_work[seg * 2 + 1] = rds_char((uint8_t)(blk[3] & 0xFF));
            /* In order: a segment 0 starts a fresh name. */
            if (seg == 0) r->ps_mask = 1;
            else if (r->ps_mask == (1u << seg) - 1u) r->ps_mask |= (uint8_t)(1u << seg);
            if (r->ps_mask == 0x0F) {
                if (memcmp(in->ps, r->ps_work, RDS_PS_LEN)) {
                    memcpy(in->ps, r->ps_work, RDS_PS_LEN);
                    in->ps[RDS_PS_LEN] = 0;
                    in->seq++;
                }
                r->ps_mask = 0;
            }
        }
        break;
    }
    case 2: {
        const int seg = b & 0x0F;
        const int ab = (b >> 4) & 1;
        if (r->rt_ab >= 0 && ab != r->rt_ab) {
            memset(r->rt_work, ' ', sizeof(r->rt_work));
            r->rt_mask = 0;
            r->rt_end = -1;
        }
        r->rt_ab = (int8_t)ab;
        r->rt_len_b = ver_b;
        if (!ver_b) {
            if (ok & 4) { rt_put(r, seg * 4, (uint8_t)(blk[2] >> 8)); rt_put(r, seg * 4 + 1, (uint8_t)blk[2]); }
            if (ok & 8) { rt_put(r, seg * 4 + 2, (uint8_t)(blk[3] >> 8)); rt_put(r, seg * 4 + 3, (uint8_t)blk[3]); }
        } else if (ok & 8) {
            rt_put(r, seg * 2, (uint8_t)(blk[3] >> 8));
            rt_put(r, seg * 2 + 1, (uint8_t)blk[3]);
        }
        publish_rt(r);
        break;
    }
    case 4:
        if (!ver_b && (ok & 12) == 12) {
            const uint32_t mjd = ((uint32_t)(b & 3) << 15) | (blk[2] >> 1);
            const unsigned hour = ((blk[2] & 1u) << 4) | (blk[3] >> 12);
            const unsigned minute = (blk[3] >> 6) & 0x3F;
            const int off = (blk[3] & 0x1F) * ((blk[3] & 0x20) ? -1 : 1);
            if (hour < 24 && minute < 60 && mjd > RDS_MJD_UNIX && off >= -24 && off <= 24) {
                in->ct_unix = ((int64_t)mjd - RDS_MJD_UNIX) * 86400 + hour * 3600 + minute * 60;
                in->ct_offset_half = (int8_t)off;
                in->ct_valid = true;
                in->ct_seq++;
                in->seq++;
            }
        }
        break;
    default:
        break;
    }
}

static void lose_sync(rds_t *r)
{
    r->synced = false;
    r->cand_blk = -1;
    r->bad_run = 0;
    r->hist_bits = 0;
    r->bad_hist = 0;
    if (r->info.synced) { r->info.synced = false; r->info.seq++; }
}

static void block_done(rds_t *r, int which, uint16_t data, bool good)
{
    if (which == RDS_BLK_A) r->ok = 0;
    if (good) { r->blk[which] = data; r->ok |= (uint8_t)(1u << which); }
    if (which == RDS_BLK_D) {
        if (r->ok) rds_decode_group(r, r->blk, r->ok);
        r->ok = 0;
    }
}

void rds_push_bit(rds_t *r, int bit)
{
    r->reg = ((r->reg << 1) | (bit ? 1u : 0u)) & 0x3FFFFFFu;
    r->nbits++;
    if (r->nbits < 26) return;

    if (!r->synced) {
        int t = block_type(r->reg);
        if (t < 0) return;
        const int ti = t == RDS_BLK_C2 ? RDS_BLK_C : t;
        if (r->cand_blk >= 0) {
            const uint32_t d = r->nbits - r->cand_at;
            if (d % 26 == 0 && d / 26 >= 1 && d / 26 <= 6 &&
                (int)((r->cand_blk + d / 26) % 4) == ti) {
                r->synced = true;
                r->next_blk = (uint8_t)((ti + 1) & 3);
                r->bitpos = 0;
                r->bad_run = 0;
                r->ok = 0;
                r->info.synced = true;
                r->info.seq++;
                r->info.blocks_ok++;
                block_done(r, ti, (uint16_t)(r->reg >> 10), true);
                return;
            }
        }
        r->cand_blk = (int8_t)ti;
        r->cand_at = r->nbits;
        return;
    }

    if (++r->bitpos < 26) return;
    r->bitpos = 0;
    const int want = r->next_blk;
    r->next_blk = (uint8_t)((want + 1) & 3);
    uint16_t data = 0;
    int fixed = 0;
    const bool good = check_block(r->reg, want, &data, &fixed) >= 0;
    /* A sixteen-block window of outcomes, and a run counter: either losing
       ten says the clock slipped, so search again. */
    const bool was_bad = (r->hist_bits >> 15) & 1;
    r->hist_bits = (uint16_t)((r->hist_bits << 1) | (good ? 0u : 1u));
    r->bad_hist = (uint8_t)(r->bad_hist + (good ? 0 : 1) - (was_bad ? 1 : 0));
    if (good) {
        r->bad_run = 0;
        if (fixed) r->info.blocks_fixed++;
        else r->info.blocks_ok++;
    } else {
        r->bad_run++;
        r->info.blocks_bad++;
    }
    block_done(r, want, data, good);
    if (r->bad_run >= 10 || r->bad_hist >= 10) lose_sync(r);
}

/* NRSC-4-B table 3 (RBDS) and IEC 62106 annex F (RDS). */
static const char *const PTY_RBDS[32] = {
    "NONE", "NEWS", "INFO", "SPORTS", "TALK", "ROCK", "CLASS ROCK", "ADULT HITS",
    "SOFT ROCK", "TOP 40", "COUNTRY", "OLDIES", "SOFT", "NOSTALGIA", "JAZZ", "CLASSICAL",
    "R&B", "SOFT R&B", "LANGUAGE", "REL MUSIC", "REL TALK", "PERSONALTY", "PUBLIC", "COLLEGE",
    "", "", "", "", "", "WEATHER", "EMERG TEST", "EMERGENCY",
};
static const char *const PTY_RDS[32] = {
    "NONE", "NEWS", "AFFAIRS", "INFO", "SPORT", "EDUCATE", "DRAMA", "CULTURE",
    "SCIENCE", "VARIED", "POP", "ROCK", "EASY", "LIGHT CLAS", "SERIOUS CL", "OTHER MUS",
    "WEATHER", "FINANCE", "CHILDREN", "SOCIAL", "RELIGION", "PHONE IN", "TRAVEL", "LEISURE",
    "JAZZ", "COUNTRY", "NATIONAL", "OLDIES", "FOLK", "DOCUMENTRY", "ALARM TEST", "ALARM",
};

const char *rds_pty_name(uint8_t pty, bool rbds)
{
    if (pty > 31) return "";
    return rbds ? PTY_RBDS[pty] : PTY_RDS[pty];
}

void rds_callsign(uint16_t pi, char *out)
{
    out[0] = 0;
    unsigned n;
    char first;
    if (pi >= 4096 && pi <= 21671) { first = 'K'; n = pi - 4096u; }
    else if (pi >= 21672 && pi <= 39247) { first = 'W'; n = pi - 21672u; }
    else return;
    out[0] = first;
    out[1] = (char)('A' + n / 676);
    out[2] = (char)('A' + (n % 676) / 26);
    out[3] = (char)('A' + n % 26);
    out[4] = 0;
}

uint32_t rds_af_khz(uint8_t code)
{
    return code >= 1 && code <= 204 ? 87500u + 100u * code : 0;
}
