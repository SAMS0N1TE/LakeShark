#include "radio_choice.h"

#include "settings.h"
#include "radio_endpoint.h"

int ls_rsel_unpack(uint64_t packed, int job)
{
    if (job < 0 || job > 15) return -1;
    const int v = (int)((packed >> (4 * job)) & 0xF);
    return v == 0xF ? -1 : v;
}

uint64_t ls_rsel_pack(uint64_t packed, int job, int radio)
{
    if (job < 0 || job > 15 || radio < -1 || radio > 14) return packed;
    const uint64_t nibble = radio < 0 ? 0xF : (uint64_t)radio;
    return (packed & ~(0xFull << (4 * job))) | (nibble << (4 * job));
}

ls_rsel_radio_t ls_rsel_saved(ls_rsel_job_t job)
{
    if (job < 0 || job >= LS_RSEL_JOBS) return LS_RSEL_NONE;
    const int r = settings_get_radio_choice((int)job);
    return r >= 0 && r < LS_RSEL_RADIOS ? (ls_rsel_radio_t)r : LS_RSEL_NONE;
}

static bool plugged(const char *id)
{
    ls_radio_endpoint_info_t info;
    return ls_radio_endpoint_get(id, &info) == LS_RADIO_OK && info.present;
}

const char *ls_rsel_sdr_endpoint(ls_rsel_job_t job)
{
    const ls_rsel_radio_t r = ls_rsel_saved(job);
    if (r == LS_RSEL_SDR_RTL && plugged(LS_RADIO_ENDPOINT_RTL_USB)) return LS_RADIO_ENDPOINT_RTL_USB;
    if (r == LS_RSEL_SDR_HACKRF && plugged(LS_RADIO_ENDPOINT_HACKRF_USB)) return LS_RADIO_ENDPOINT_HACKRF_USB;
    if (plugged(LS_RADIO_ENDPOINT_RTL_USB)) return LS_RADIO_ENDPOINT_RTL_USB;
    if (plugged(LS_RADIO_ENDPOINT_HACKRF_USB)) return LS_RADIO_ENDPOINT_HACKRF_USB;
    return NULL;
}
