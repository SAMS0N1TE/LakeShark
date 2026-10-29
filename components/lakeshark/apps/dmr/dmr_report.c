/* The `dmr` report, as text, so it can be tested on the bench rather than
 * only read off a console. */
#include "dmr_watch.h"

#include <stdarg.h>
#include <stdio.h>

dmr_verdict_t dmr_watch_verdict(const dmr_watch_t *w)
{
    if (!w || !w->bursts) return DMR_VERDICT_NOTHING;
    /* Only the link control is evidence. A sync match inside its 5-bit
       threshold happens on noise, the Slot Type's Golay accepts about a
       third of random words within its correction radius, and BPTC corrects
       its way to a clean block often enough to have done so on a P25
       channel here. The RS parity behind a full LC is 24 bits checked
       against a data-type mask, which noise does not pass. An RS-corrected
       LC passes about one random word in 5500 and an embedded LC rests on
       a 5-bit checksum behind its Hamming rows, so one of those is not
       enough - two are. CSBK and data header CRCs are 16 bits: reported,
       not counted. */
    if (w->lc_ok) return DMR_VERDICT_DMR;
    if (w->lc_fixed + w->emb_lc_ok >= 2) return DMR_VERDICT_DMR;
    return DMR_VERDICT_NOT_DMR;
}

typedef struct { char *buf; size_t len, at; } out_t;

static void put(out_t *o, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    const size_t room = o->at < o->len ? o->len - o->at : 0;
    const int n = vsnprintf(room ? o->buf + o->at : NULL, room, fmt, ap);
    va_end(ap);
    if (n > 0) o->at += (size_t)n;
}

static const char *class_name(dmr_sync_class_t c)
{
    switch (c) {
    case DMR_SYNC_BS_VOICE: return "BS voice";
    case DMR_SYNC_BS_DATA:  return "BS data";
    case DMR_SYNC_MS_VOICE: return "MS voice";
    case DMR_SYNC_MS_DATA:  return "MS data";
    default:                return "none";
    }
}

static const char *end_name(uint8_t r)
{
    switch (r) {
    case DMR_END_TERMINATOR: return "terminator";
    case DMR_END_TIMEOUT:    return "timeout";
    case DMR_END_REPLACED:   return "replaced";
    default:                 return "-";
    }
}

static void call_line(out_t *o, unsigned slot, const dmr_call_t *c, int64_t now)
{
    if (!c->have_lc) {
        put(o, "slot %u: no call\n", slot);
        return;
    }
    const bool live = c->active && now >= c->last_us &&
                      now - c->last_us <= DMR_CALL_TIMEOUT_US;
    int64_t end = live ? now : c->end_us ? c->end_us : c->last_us;
    const unsigned long ms = end > c->start_us ? (unsigned long)((end - c->start_us) / 1000) : 0;
    put(o, "slot %u: %s %s %lu -> %s %lu cc=%u %lu.%lus%s%s%s",
        slot, live ? "LIVE" : "ended", c->group ? "group" : "private",
        (unsigned long)c->lc.source, c->group ? "TG" : "ID",
        (unsigned long)c->lc.destination, (unsigned)c->colour_code,
        ms / 1000, (ms % 1000) / 100,
        c->encrypted ? " encrypted" : "",
        c->late_entry ? " late-entry" : "",
        c->corrected ? " rs-fixed" : "");
    if (!live) put(o, " end=%s", end_name(c->end_reason));
    put(o, "\n");
    if (c->alias.text[0])
        put(o, "        alias \"%s\"%s\n", c->alias.text, c->alias.complete ? "" : " (partial)");
    if (c->have_gps)
        put(o, "        gps %.5f %.5f err=%u\n", (double)c->gps.latitude,
            (double)c->gps.longitude, (unsigned)c->gps.position_error);
}

size_t dmr_watch_format(const dmr_watch_t *w, int64_t now_us, char *buf, size_t len)
{
    out_t o = { buf, len, 0 };
    if (buf && len) buf[0] = '\0';
    if (!w) return 0;

    put(&o, "dmr symbols=%lu bursts=%lu slot_type=%lu bptc ok=%lu fail=%lu lc=%lu\n",
        (unsigned long)w->symbols, (unsigned long)w->bursts,
        (unsigned long)w->slot_type_ok, (unsigned long)w->bptc_ok,
        (unsigned long)w->bptc_fail, (unsigned long)w->lc_ok);
    put(&o, "checked: lc rs-fixed=%lu embedded=%lu (fail %lu) csbk=%lu data hdr=%lu "
            "voice B-F=%lu\n",
        (unsigned long)w->lc_fixed, (unsigned long)w->emb_lc_ok,
        (unsigned long)w->emb_lc_fail, (unsigned long)w->csbk_ok,
        (unsigned long)w->data_hdr_ok, (unsigned long)w->voice_bursts);
    put(&o, "last burst: %s errors=%u cc=%u polarity=%s timing=%s",
        class_name(w->last_class), (unsigned)w->last_sync_errors,
        (unsigned)w->colour_code,
        w->polarity < 0 ? "unknown" : w->polarity ? "inverted" : "normal",
        w->from_samples ? "own" : "P25");
    if (w->from_samples) put(&o, " resliced=%lu", (unsigned long)w->refined);
    put(&o, "\n");

    if (w->have_lc)
        put(&o, "last LC: flco=%u fid=%u src=%lu dst=%lu opts=0x%02X\n",
            (unsigned)w->lc.flco, (unsigned)w->lc.fid,
            (unsigned long)w->lc.source, (unsigned long)w->lc.destination,
            (unsigned)w->lc.service_options);
    else
        put(&o, "last LC: none has passed parity\n");

    if (w->have_csbk) {
        put(&o, "last CSBK: %s (o=0x%02X fid=%u) slot=%u",
            dmr_csbk_name(w->csbk.opcode, w->csbk.fid), (unsigned)w->csbk.opcode,
            (unsigned)w->csbk.fid, (unsigned)w->csbk_slot);
        if (w->csbk.has_addresses)
            put(&o, " src=%lu dst=%lu", (unsigned long)w->csbk.source,
                (unsigned long)w->csbk.target);
        put(&o, "\n");
    }
    if (w->have_data_hdr)
        put(&o, "last data header: %s sap=%u %s src=%lu dst=%lu blocks=%u slot=%u\n",
            dmr_dpf_name(w->data_hdr.dpf), (unsigned)w->data_hdr.sap,
            w->data_hdr.group ? "group" : "individual",
            (unsigned long)w->data_hdr.source, (unsigned long)w->data_hdr.destination,
            (unsigned)w->data_hdr.blocks_to_follow, (unsigned)w->data_hdr_slot);

    call_line(&o, 1, &w->call[0], now_us);
    call_line(&o, 2, &w->call[1], now_us);
    if (w->unknown_slot)
        put(&o, "bursts with no slot identity: %lu\n", (unsigned long)w->unknown_slot);

    switch (dmr_watch_verdict(w)) {
    case DMR_VERDICT_DMR:
        if (w->lc_ok)
            put(&o, "verdict: DMR - %lu link control block(s) decoded\n",
                (unsigned long)w->lc_ok);
        else
            put(&o, "verdict: DMR - %lu embedded and %lu RS-corrected link "
                    "control block(s), no exact one yet\n",
                (unsigned long)w->emb_lc_ok, (unsigned long)w->lc_fixed);
        break;
    case DMR_VERDICT_NOT_DMR:
        if (w->lc_fixed + w->emb_lc_ok)
            put(&o, "verdict: not yet DMR - %lu sync hit(s), one %s link control, "
                    "which needs a second or an exact one to count\n",
                (unsigned long)w->bursts, w->emb_lc_ok ? "embedded" : "RS-corrected");
        else
            put(&o, "verdict: no DMR - %lu sync hit(s), %lu through BPTC, none "
                    "carried a valid link control\n",
                (unsigned long)w->bursts, (unsigned long)w->bptc_ok);
        break;
    default:
        put(&o, "verdict: no bursts\n");
        break;
    }
    return o.at;
}
