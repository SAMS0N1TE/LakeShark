/* US classic Z-Wave scan, promiscuous HomeIDs, FIFO FCS checked here. */
#include "exp_lorafam.h"
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static EXT_RAM_BSS_ATTR lf_view_t s_view;
static EXT_RAM_BSS_ATTR uint8_t s_frame[255];
static bool start(char *why, size_t n)
{
    const esp_err_t err = lr20xx_exp_begin(LR20XX_ENGINE_ZWAVE, 0, 0, 0);
    if (err != ESP_OK) { lf_start_fail(why, n, "LR2021 Z-Wave", err); return false; }
    portENTER_CRITICAL(&s_lock); memset(&s_view, 0, sizeof(s_view)); portEXIT_CRITICAL(&s_lock); return true;
}
static void stop(void) { (void)lr20xx_exp_end(); }
static void poll(void)
{
    lr20xx_engine_packet_t p;
    const int n = lr20xx_exp_poll(s_frame, sizeof(s_frame), &p);
    if (!n) return;
    lorafam_zwave_t z = {0};
    const bool parsed = n > 0 && lorafam_zwave(s_frame, n, p.rate, &z);
    portENTER_CRITICAL(&s_lock);
    if (n < 0) s_view.errors++;
    else {
        s_view.frames++; s_view.packet = p; s_view.zwave = z; s_view.mac_ok = parsed;
        if (!parsed || !z.checksum_ok) s_view.bad++;
        else lf_node_add(&s_view, z.homeid, z.source, 0, z.type, p.rssi_dbm);
    }
    portEXIT_CRITICAL(&s_lock);
}
static int lines(char (*out)[LS_EXP_LINE], int max)
{
    lf_view_t v; portENTER_CRITICAL(&s_lock); v = s_view; portEXIT_CRITICAL(&s_lock);
    int n = 0;
#define LINE(...) do { if (n < max) snprintf(out[n++], LS_EXP_LINE, __VA_ARGS__); } while (0)
    LINE("US scan R1 908.420 R2 908.400 R3 916.000");
    LINE("9.6 / 40 / 100 kbps; HomeID filter OFF");
    LINE("RX %lu bad FCS/header %lu bus %lu", (unsigned long)v.frames, (unsigned long)v.bad, (unsigned long)v.errors);
    if (v.frames) {
        LINE("RATE R%u len %u RSSI %.1f dBm", v.packet.rate, v.packet.length, (double)v.packet.rssi_dbm);
        if (v.mac_ok) {
            LINE("HOME %08lX SRC %u DST %s%u", (unsigned long)v.zwave.homeid, v.zwave.source, v.zwave.dest_known ? "" : "?", v.zwave.dest);
            LINE("FC %04X type %u length %u FCS %s", v.zwave.control, v.zwave.type, v.zwave.length, v.zwave.checksum_ok ? "OK" : "BAD");
        } else LINE("Unparsed MAC (beam/short/unknown)");
    } else LINE("Waiting for a nearby Z-Wave network");
    LINE("HOME / NODE / COUNT / TYPES / RSSI");
    for (unsigned i=0; i<LF_NODES && n<max; i++) if (v.nodes[i].count)
        LINE("%08lX %3lu %5lu %04X %.1f", (unsigned long)v.nodes[i].network, (unsigned long)v.nodes[i].id,
             (unsigned long)v.nodes[i].count, v.nodes[i].types, (double)v.nodes[i].rssi);
#undef LINE
    return n;
}
const ls_experiment_t exp_zwave = {
    .id="zwave", .name="Z-WAVE SNIFFER", .sub="US R1/R2/R3 channel scan, header statistics", .maturity=LS_EXP_TRYING, .lr2021_only=true,
    .needs="An active US Z-Wave network nearby", .start=start, .stop=stop, .poll=poll, .lines=lines,
};
