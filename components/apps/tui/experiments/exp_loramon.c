/* Four LoRa detectors, one frequency/BW group. Header guesses only. */
#include "exp_lorafam.h"
#include <stdlib.h>
extern const ls_experiment_t exp_loramon;
static volatile unsigned s_preset;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static EXT_RAM_BSS_ATTR lf_view_t s_view;
static EXT_RAM_BSS_ATTR uint8_t s_frame[255];
static const char *const PRESETS[] = {"Mesh US", "LongFast US", "LongFast high SF"};
static bool start(char *why, size_t n)
{
    const unsigned preset = s_preset;
    const esp_err_t err = lr20xx_exp_begin(LR20XX_ENGINE_LORAMON, preset, 0, 0);
    if (err != ESP_OK) { lf_start_fail(why, n, "LR2021 receive setup", err); return false; }
    portENTER_CRITICAL(&s_lock); memset(&s_view, 0, sizeof(s_view)); s_view.preset = preset; portEXIT_CRITICAL(&s_lock);
    return true;
}
static void stop(void) { (void)lr20xx_exp_end(); }
static void poll(void)
{
    lr20xx_engine_packet_t p;
    const int n = lr20xx_exp_poll(s_frame, sizeof(s_frame), &p);
    if (!n) return;
    lorafam_mesh_t m = {0};
    const bool mt = n > 0 && s_view.preset && lorafam_meshtastic(s_frame, n, &m);
    const bool mc = n > 0 && !s_view.preset && lorafam_meshcore(s_frame, n, &m);
    portENTER_CRITICAL(&s_lock);
    if (n < 0) s_view.errors++;
    else {
        s_view.frames++; s_view.packet = p; s_view.mesh = m; s_view.protocol = mt ? 1 : mc ? 2 : 0;
        if (m.source_known) lf_node_add(&s_view, mt ? 1 : 2, m.source, m.source_hash ? 2 : mt ? 0 : 1, m.type, p.rssi_dbm);
    }
    portEXIT_CRITICAL(&s_lock);
}
static int lines(char (*out)[LS_EXP_LINE], int max)
{
    lf_view_t v; portENTER_CRITICAL(&s_lock); v = s_view; portEXIT_CRITICAL(&s_lock);
    unsigned preset = v.frames ? v.preset : s_preset;
    int n = 0;
#define LINE(...) do { if (n < max) snprintf(out[n++], LS_EXP_LINE, __VA_ARGS__); } while (0)
    LINE("%s %.3f MHz", PRESETS[preset], preset ? 906.875 : 910.525);
    const unsigned first = preset == 0 ? 7 : preset == 1 ? 8 : 9;
    LINE("BW %.1f kHz SF%u..%u sync %02X", preset ? 250.0 : 62.5, first, first+3, preset ? 0x2B : 0x12);
    LINE("RX %lu  errors %lu", (unsigned long)v.frames, (unsigned long)v.errors);
    if (v.frames) {
        unsigned det = 0; while (det < 4 && !(v.packet.detector & (1u << det))) det++;
        static const char *const cr[] = {"?", "4/5", "4/6", "4/7", "4/8", "LI4/5", "LI4/6", "LI4/8", "conv4/6", "conv4/8"};
        LINE("DET mask %X SF%u CR%s len %u", v.packet.detector, det < 4 ? first+det : 0,
             v.packet.cr < sizeof(cr)/sizeof(cr[0]) ? cr[v.packet.cr] : "?", v.packet.length);
        LINE("RSSI %.1f SNR %.2f FEI %+ld Hz", (double)v.packet.rssi_dbm, (double)v.packet.snr_db, (long)v.packet.fei_hz);
        if (v.protocol == 1) {
            LINE("GUESS Meshtastic %08lX>%08lX", (unsigned long)v.mesh.source, (unsigned long)v.mesh.dest);
            LINE("ID %08lX flags %02X channel %02X", (unsigned long)v.mesh.id, v.mesh.flags, v.mesh.channel);
        } else if (v.protocol == 2) LINE("GUESS MeshCore route %u type %u", v.mesh.route, v.mesh.type);
        else LINE("GUESS unknown LoRa header");
    } else LINE("Waiting for LoRa packets");
    LINE("SENDERS (hash/prefix may collide)");
    for (unsigned i = 0; i < LF_NODES && n < max; i++) if (v.nodes[i].count)
        LINE("%s %08lX %lu %.1f dBm", v.nodes[i].kind == 2 ? "hash" : v.nodes[i].kind == 1 ? "key" : "node",
             (unsigned long)v.nodes[i].id, (unsigned long)v.nodes[i].count, (double)v.nodes[i].rssi);
#undef LINE
    return n;
}
static bool configure(int argc, char **argv, char *why, size_t n)
{
    if (argc == 1) {
        if (!strcmp(argv[0], "mesh") || !strcmp(argv[0], "Mesh US")) { s_preset = 0; return true; }
        if (!strcmp(argv[0], "meshtastic") || !strcmp(argv[0], "LongFast US")) { s_preset = 1; return true; }
        if (!strcmp(argv[0], "high")) { s_preset = 2; return true; }
    }
    snprintf(why, n, "preset: mesh, meshtastic, high"); return false;
}
static int get(const ls_opt_t *o) { (void)o; return s_preset; }
static void set(const ls_opt_t *o, int v) { (void)o; if (v >= 0 && v < 3) s_preset = v; if (ls_exp_running() == &exp_loramon) ls_exp_start(&exp_loramon); }
static const ls_opt_t OPTS[] = {{.label="PRESET", .kind=LS_OPT_CYCLE, .names=PRESETS, .n=3, .get=get, .set=set}};
const ls_experiment_t exp_loramon = {
    .id="loramon", .name="LORA MONITOR", .sub="Four SF detectors on one US mesh frequency", .maturity=LS_EXP_TRYING, .lr2021_only=true,
    .needs="MeshCore or Meshtastic traffic in range", .start=start, .stop=stop, .poll=poll, .lines=lines,
    .configure=configure, .opts=OPTS, .n_opts=1,
};
