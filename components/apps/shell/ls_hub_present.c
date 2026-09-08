#include "shell/ls_hub_present.h"

#include <stdio.h>
#include <string.h>

#include "fm_state.h"
#include "rec_state.h"

static void text_copy(char *dst, size_t cap, const char *src)
{
    if (!dst || cap == 0) return;
    snprintf(dst, cap, "%s", src ? src : "");
}

static void set_identity(ls_hub_presentation_t *out, const char *name)
{
    text_copy(out->mode, sizeof(out->mode), name);
    text_copy(out->target_app, sizeof(out->target_app), name);
}

static const char *rec_phase_name(int phase)
{
    switch (phase) {
    case REC_ARMED:     return "ARMED";
    case REC_CAPTURING: return "CAPTURING";
    case REC_DONE:      return "DONE";
    default:            return "IDLE";
    }
}

/*LS-697  app_current() exposes the backend owner, which is not always the
  operator-facing app: ACARS deliberately runs inside FM, while REC used to
  fall through to an IDLE/zero-frequency summary. Keep that translation here
  so every hub consumer sees the same label and navigation target. */
void ls_hub_present(const ls_hub_observation_t *in,
                    ls_hub_presentation_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    text_copy(out->mode, sizeof(out->mode), "--");
    text_copy(out->detail, sizeof(out->detail), "IDLE");
    if (!in) return;

    const char *backend = in->backend_mode ? in->backend_mode : "--";
    out->freq_hz     = in->freq_hz;
    out->signal_pct  = in->signal_pct;
    out->active      = in->active;
    out->iq_bytes_sec = in->iq_bytes_sec;

    if (strcmp(backend, "P25") == 0) {
        set_identity(out, "P25");
        if (in->p25_nac)
            snprintf(out->detail, sizeof(out->detail), "NAC %03X  TG %d",
                     in->p25_nac, in->p25_tg);
        else if (in->p25_has_sync)
            snprintf(out->detail, sizeof(out->detail), "SYNC  %s",
                     in->p25_ftype ? in->p25_ftype : "");
        else
            text_copy(out->detail, sizeof(out->detail), "NO SYNC");
    } else if (strcmp(backend, "FM") == 0) {
        if (in->fm_submode == FM_MODE_ACARS) {
            set_identity(out, "ACARS");
            /* FM.squelch_open is not updated in ACARS mode. Presenting it as
               OPEN/QUIET would turn stale analog state into ACARS activity. */
            out->active = false;
            text_copy(out->detail, sizeof(out->detail),
                      "MSK 2400  AIRCRAFT TEXT");
        } else {
            set_identity(out, "FM");
            snprintf(out->detail, sizeof(out->detail), "%s  SQL %d.%d",
                     out->active ? "OPEN" : "QUIET",
                     in->fm_squelch_tenths / 10,
                     in->fm_squelch_tenths % 10);
        }
    } else if (strcmp(backend, "ADS-B") == 0) {
        set_identity(out, "ADS-B");
        out->contacts = in->adsb_tracked;
        snprintf(out->detail, sizeof(out->detail), "%d TRACKED  %d/s",
                 in->adsb_tracked, in->adsb_msgs_sec);
    } else if (strcmp(backend, "REC") == 0) {
        set_identity(out, "REC");
        if (in->rec_mag_now < 0) out->signal_pct = 0;
        else if (in->rec_mag_now >= 256) out->signal_pct = 100;
        else out->signal_pct = (in->rec_mag_now * 100 + 128) / 256;
        out->active = (in->rec_mag_thresh > 0 &&
                       in->rec_mag_now >= in->rec_mag_thresh) ||
                      in->rec_phase == REC_ARMED ||
                      in->rec_phase == REC_CAPTURING;
        snprintf(out->detail, sizeof(out->detail), "%s  %d EDGES",
                 rec_phase_name(in->rec_phase), in->rec_edges);
    } else {
        out->freq_hz = 0;
        out->signal_pct = 0;
        out->active = false;
        out->iq_bytes_sec = 0;
    }

    if (out->signal_pct < 0) out->signal_pct = 0;
    if (out->signal_pct > 100) out->signal_pct = 100;

    /* A stopped or missing receiver cannot truthfully carry live signal
       state. Preserve identity, navigation, and the requested frequency so
       Home does not rewrite or conceal the station the operator selected. */
    if (in->parked) {
        out->signal_pct = 0;
        out->active = false;
        out->iq_bytes_sec = 0;
        text_copy(out->detail, sizeof(out->detail), "RADIO PARKED");
    }
    if (!in->receiver_ready) {
        out->signal_pct = 0;
        out->active = false;
        out->iq_bytes_sec = 0;
        text_copy(out->detail, sizeof(out->detail), "RECEIVER UNAVAILABLE");
    }
}
