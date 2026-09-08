#include "ui/ls_receiver_status.h"

#include <stdio.h>
#include <string.h>

static void format_mhz(char *out, size_t cap, uint64_t hz)
{
    snprintf(out, cap, "%llu.%04llu",
             (unsigned long long)(hz / 1000000u),
             (unsigned long long)((hz / 100u) % 10000u));
}

static void format_gain(char *out, size_t cap, int tenths)
{
    if (tenths == 0) snprintf(out, cap, "AGC");
    else snprintf(out, cap, "%d.%d dB", tenths / 10,
                  tenths < 0 ? -(tenths % 10) : tenths % 10);
}

void ls_receiver_present(const ls_iq_control_status_t *status,
                         ls_receiver_presentation_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!status) {
        snprintf(out->connection, sizeof(out->connection), "NO RECEIVER");
        snprintf(out->frequency, sizeof(out->frequency), "REQ -- / ACT --");
        snprintf(out->gain, sizeof(out->gain), "REQ -- / ACT --");
        return;
    }

    out->available = status->receiver_streaming;
    snprintf(out->connection, sizeof(out->connection), "%s",
             status->receiver_streaming ? "STREAMING" : "RECONNECTING");

    char requested[20], effective[20];
    format_mhz(requested, sizeof(requested), status->requested_center_hz);
    if (status->effective_center_known)
        format_mhz(effective, sizeof(effective), status->effective_center_hz);
    else
        snprintf(effective, sizeof(effective), "--");

    bool tune_differs = !status->effective_center_known ||
        status->requested_center_hz != status->effective_center_hz;
    if (status->tune_state == LS_IQ_RESULT_FAILED) {
        snprintf(out->frequency, sizeof(out->frequency),
                 "REQ %s / ACT %s  %s", requested, effective,
                 ls_radio_err_name(status->tune_error));
        out->tune_attention = true;
    } else if (status->tune_state == LS_IQ_RESULT_PENDING) {
        snprintf(out->frequency, sizeof(out->frequency),
                 "REQ %s / ACT %s  PENDING", requested, effective);
        out->tune_attention = true;
    } else if (tune_differs) {
        snprintf(out->frequency, sizeof(out->frequency),
                 "REQ %s / ACT %s", requested, effective);
    } else {
        snprintf(out->frequency, sizeof(out->frequency), "%s MHz", effective);
    }

    char requested_gain[20], effective_gain[20];
    format_gain(requested_gain, sizeof(requested_gain),
                status->requested_gain_tenths_db);
    if (status->effective_gain_known)
        format_gain(effective_gain, sizeof(effective_gain),
                    status->effective_gain_tenths_db);
    else
        snprintf(effective_gain, sizeof(effective_gain), "--");

    bool gain_differs = !status->effective_gain_known ||
        status->requested_gain_tenths_db != status->effective_gain_tenths_db;
    if (status->gain_state == LS_IQ_RESULT_FAILED) {
        snprintf(out->gain, sizeof(out->gain), "REQ %s / ACT %s  %s",
                 requested_gain, effective_gain,
                 ls_radio_err_name(status->gain_error));
        out->gain_attention = true;
    } else if (status->gain_state == LS_IQ_RESULT_PENDING) {
        snprintf(out->gain, sizeof(out->gain), "REQ %s / ACT %s  PENDING",
                 requested_gain, effective_gain);
        out->gain_attention = true;
    } else if (gain_differs) {
        snprintf(out->gain, sizeof(out->gain), "REQ %s / ACT %s",
                 requested_gain, effective_gain);
    } else {
        snprintf(out->gain, sizeof(out->gain), "%s", effective_gain);
    }
}
