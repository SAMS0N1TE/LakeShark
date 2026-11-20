/* sim_traffic: Meshtastic frames for the simulator, so MESHTASTIC's screens
   can be looked at full instead of empty.

   Prints a LAPSIM_SCRIPT: node info, positions, telemetry and channel text
   from a handful of nodes around Buenos Aires, each a real frame on the
   default channel (LongFast, key AQ==), encrypted as a node would send it.

     cc -O1 -I.. -o sim_traffic sim_traffic.c ../mt_proto.c ../mt_aes.c ../mt_pki.c -lm
     LAPSIM=meshtastic LAPSIM_SCRIPT="$(./sim_traffic)" lssim lap -A 40 -f 400 -o out.bmp

   `./sim_traffic N` stops after N steps (fewer messages), `-q` leaves the
   text out (nodes only), `-s F` spaces the frames F apart (40 ms each;
   the default 6). Frame numbers start at 20 (`-f N`: at N). `-x` adds 1.3's traffic: a
   reply, reactions, a waypoint, neighbour info, a store-and-forward
   heartbeat and range-test packets; `-d HEX` a direct message to that node;
   `-l` lines in Portuguese, French, German, Italian and Dutch after the rest,
   `-c` lines in Russian, Ukrainian and Polish. */
#include "mt_proto.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint32_t num;
    const char *long_name, *short_name;
    double lat, lon;
    int battery;            /* 101 = on power */
    float volts;
    int hops, rssi;         /* tenths of a dBm */
} sim_node_t;

static const sim_node_t NODES[] = {
    { 0xA1B2C3D4u, "Martin Portatil", "MART", -34.5795, -58.4206, 78, 3.98f, 0, -874 },
    { 0x5E11F00Du, "Base Tigre", "TGR1", -34.4260, -58.5796, 101, 4.92f, 1, -1012 },
    { 0x0BADCAFEu, "Sofi T-Echo", "SOFI", -34.9214, -57.9545, 41, 3.71f, 2, -1103 },
    { 0x7777AAAAu, "Repetidor Cerro", "RPT", -34.6400, -58.7900, 101, 5.01f, 3, -1187 },
    { 0x31415926u, "Dave Portable", "DAVE", -34.6037, -58.3816, 92, 4.10f, 0, -655 },
};
#define NN ((int)(sizeof(NODES) / sizeof(NODES[0])))

typedef struct { int node; const char *text; } sim_text_t;
static const sim_text_t TEXTS[] = {
    { 0, "Hola! alguien me escucha? estoy probando el nodo nuevo" },
    { 1, "Te escucho re bien desde Tigre" },
    { 4, "Copy that, hearing everyone fine from downtown" },
    { 2, "Buenas! Mañana a las 10 nos juntamos en la plaza, ¿vienen?" },
    { 0, "Dale, nos vemos ahí. Llevo la antena nueva" },
    { 3, "El repetidor del cerro vuelve a funcionar" },
    { 2, "Que buena onda! Gracias che" },
};
#define NT ((int)(sizeof(TEXTS) / sizeof(TEXTS[0])))
/* `-l`: visitors on the mesh, in the translator's other languages */
static const sim_text_t TEXTS_L[] = {
    { 4, "Hallo zusammen, hört mich jemand? Ich bin auf dem Berg mit dem Funkgerät" },
    { 1, "Oi pessoal, tudo bem? Te ouço alto e claro daqui" },
    { 2, "Salut à tous, je suis sur la colline avec la radio" },
    { 0, "Ciao a tutti, qualcuno mi sente? Sono vicino alla stazione" },
    { 3, "Hoi allemaal, mijn batterij is bijna leeg" },
};
#define NTL ((int)(sizeof(TEXTS_L) / sizeof(TEXTS_L[0])))
/* `-c`: visitors writing Russian and Ukrainian (in Cyrillic) and Polish */
static const sim_text_t TEXTS_C[] = {
    { 1, "Всем привет! Кто-нибудь меня слышит? Я на холме с рацией" },
    { 3, "Привіт, я вдома, батарея майже розряджена" },
    { 0, "Cześć, słyszę cię dobrze, jestem na wzgórzu z radiem" },
};
#define NTC ((int)(sizeof(TEXTS_C) / sizeof(TEXTS_C[0])))

static uint8_t key[32];
static int key_len;
static uint8_t hash;
static uint32_t next_id = 0x1000;
static int frame_no = 20, frame_step = 6, steps, max_steps = 1 << 30;

static uint32_t emit_to(const sim_node_t *n, uint32_t dest, uint32_t port, const uint8_t *pl, size_t pn,
                        uint32_t reply_id, uint32_t emoji, int hop_start)
{
    if (steps >= max_steps) return 0;
    uint8_t buf[MT_HEADER_LEN + MT_MAX_PAYLOAD];
    mt_header_t h;
    memset(&h, 0, sizeof(h));
    h.dest = dest; h.from = n->num; h.id = next_id++;
    h.hop_start = (uint8_t)hop_start; h.hop_limit = (uint8_t)(hop_start > n->hops ? hop_start - n->hops : 0); h.channel = hash;
    mt_header_pack(&h, buf);
    mt_data_t d = { port, pl, pn, false, 0, reply_id, emoji };
    const int dl = mt_data_encode(&d, buf + MT_HEADER_LEN, MT_MAX_PAYLOAD);
    if (dl <= 0) return 0;
    mt_crypt(key, key_len, h.from, h.id, buf + MT_HEADER_LEN, (size_t)dl);
    printf("%s%d:iorssi=%d;%d:packethex=", steps ? ";" : "", frame_no, n->rssi, frame_no);
    for (int i = 0; i < MT_HEADER_LEN + dl; i++) printf("%02x", buf[i]);
    frame_no += frame_step;
    steps++;
    return h.id;
}
static uint32_t emit(const sim_node_t *n, uint32_t port, const uint8_t *pl, size_t pn)
{
    return emit_to(n, MT_BROADCAST, port, pl, pn, 0, 0, 3);
}

int main(int argc, char **argv)
{
    bool text = true, extra = false, langs = false, cyr = false;
    uint32_t dm_to = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-q")) text = false;
        else if (!strcmp(argv[i], "-x")) extra = true;
        else if (!strcmp(argv[i], "-l")) langs = true;
        else if (!strcmp(argv[i], "-c")) cyr = true;
        else if (!strcmp(argv[i], "-d") && i + 1 < argc) dm_to = (uint32_t)strtoul(argv[++i], NULL, 16);
        else if (!strcmp(argv[i], "-s") && i + 1 < argc) frame_step = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-f") && i + 1 < argc) frame_no = atoi(argv[++i]);
        else max_steps = atoi(argv[i]);
    }
    const uint8_t psk = 1;
    key_len = mt_channel_key(&psk, 1, key);
    hash = mt_channel_hash("LongFast", key, key_len);
    uint8_t pl[MT_MAX_PAYLOAD];
    for (int i = 0; i < NN; i++) {
        const sim_node_t *n = &NODES[i];
        mt_user_t u;
        memset(&u, 0, sizeof(u));
        snprintf(u.id, sizeof(u.id), "!%08x", (unsigned)n->num);
        snprintf(u.long_name, sizeof(u.long_name), "%s", n->long_name);
        snprintf(u.short_name, sizeof(u.short_name), "%s", n->short_name);
        u.hw_model = 43;
        const uint8_t mac[6] = { 0, 0, (uint8_t)(n->num >> 24), (uint8_t)(n->num >> 16), (uint8_t)(n->num >> 8), (uint8_t)n->num };
        int l = mt_user_encode(&u, mac, pl, sizeof(pl));
        if (l > 0) emit(n, MT_PORT_NODEINFO, pl, (size_t)l);
        l = mt_position_encode(n->lat, n->lon, 25, 1763100000u, 32, pl, sizeof(pl));
        if (l > 0) emit(n, MT_PORT_POSITION, pl, (size_t)l);
        mt_device_t dev = { (uint32_t)n->battery, 3600u * (uint32_t)(i + 1), n->volts, 4.5f + (float)i, 0.8f * (float)i, true };
        l = mt_telemetry_encode(1763100000u, &dev, pl, sizeof(pl));
        if (l > 0) emit(n, MT_PORT_TELEMETRY, pl, (size_t)l);
    }
    uint32_t ids[NT] = { 0 };
    for (int i = 0; text && i < NT; i++)
        ids[i] = emit(&NODES[TEXTS[i].node], MT_PORT_TEXT, (const uint8_t *)TEXTS[i].text, strlen(TEXTS[i].text));
    for (int i = 0; text && langs && i < NTL; i++)
        emit(&NODES[TEXTS_L[i].node], MT_PORT_TEXT, (const uint8_t *)TEXTS_L[i].text, strlen(TEXTS_L[i].text));
    for (int i = 0; text && cyr && i < NTC; i++)
        emit(&NODES[TEXTS_C[i].node], MT_PORT_TEXT, (const uint8_t *)TEXTS_C[i].text, strlen(TEXTS_C[i].text));
    if (extra && text) {
        const char *re = "Te escucho fuerte y claro desde el centro";
        emit_to(&NODES[4], MT_BROADCAST, MT_PORT_TEXT, (const uint8_t *)re, strlen(re), ids[0], 0, 3);
        const char *heart = "\xe2\x9d\xa4\xef\xb8\x8f", *up = "\xf0\x9f\x91\x8d", *lol = "\xf0\x9f\x98\x82";
        emit_to(&NODES[2], MT_BROADCAST, MT_PORT_TEXT, (const uint8_t *)heart, strlen(heart), ids[2], 1, 3);
        emit_to(&NODES[1], MT_BROADCAST, MT_PORT_TEXT, (const uint8_t *)up, strlen(up), ids[2], 1, 3);
        emit_to(&NODES[0], MT_BROADCAST, MT_PORT_TEXT, (const uint8_t *)up, strlen(up), ids[2], 1, 3);
        emit_to(&NODES[0], MT_BROADCAST, MT_PORT_TEXT, (const uint8_t *)lol, strlen(lol), ids[6], 1, 3);
    }
    if (extra) {
        mt_waypoint_t w;
        memset(&w, 0, sizeof(w));
        w.id = 0x57A11E57u; w.lat = -34.6083; w.lon = -58.3712; w.has_pos = true; w.icon = 0x1F4CD;
        snprintf(w.name, sizeof(w.name), "Plaza de Mayo"); snprintf(w.desc, sizeof(w.desc), "meetup at 10");
        int l = mt_waypoint_encode(&w, pl, sizeof(pl));
        if (l > 0) emit(&NODES[1], MT_PORT_WAYPOINT, pl, (size_t)l);
        mt_neighbors_t nb;
        memset(&nb, 0, sizeof(nb));
        nb.node = nb.last_sent_by = NODES[1].num; nb.interval_s = 21600;
        nb.id[0] = NODES[0].num; nb.snr[0] = 6.0f; nb.id[1] = NODES[3].num; nb.snr[1] = -3.5f; nb.n = 2;
        l = mt_neighbors_encode(&nb, pl, sizeof(pl));
        if (l > 0) emit(&NODES[1], MT_PORT_NEIGHBORINFO, pl, (size_t)l);
        memset(&nb, 0, sizeof(nb));
        nb.node = nb.last_sent_by = NODES[3].num; nb.interval_s = 21600;
        nb.id[0] = NODES[2].num; nb.snr[0] = -8.0f; nb.id[1] = NODES[1].num; nb.snr[1] = -2.0f; nb.n = 2;
        l = mt_neighbors_encode(&nb, pl, sizeof(pl));
        if (l > 0) emit(&NODES[3], MT_PORT_NEIGHBORINFO, pl, (size_t)l);
        const uint8_t hb[] = { 0x08, 0x02, 0x22, 0x03, 0x08, 0x84, 0x07 };   /* ROUTER_HEARTBEAT, period 900 s */
        emit(&NODES[3], MT_PORT_STORE_FORWARD, hb, sizeof(hb));
        for (int s = 1; s <= 4; s++) {
            if (s == 3) continue;                                            /* one lost on the way */
            char t[16];
            const int n = snprintf(t, sizeof(t), "seq %d", s);
            emit_to(&NODES[4], MT_BROADCAST, MT_PORT_RANGE_TEST, (const uint8_t *)t, (size_t)n, 0, 0, 0);
        }
    }
    if (dm_to) {
        const char *t = "Hola! te mando esto directo, me escuchas?";
        emit_to(&NODES[0], dm_to, MT_PORT_TEXT, (const uint8_t *)t, strlen(t), 0, 0, 3);
    }
    /* One node this board has no name for: a packet on a channel it cannot read. */
    const sim_node_t stranger = { 0x00C0FFEEu, "", "", 0, 0, 0, 0, 2, -1215 };
    hash ^= 0x55;
    emit(&stranger, MT_PORT_TEXT, (const uint8_t *)"x", 1);
    printf("\n");
    return 0;
}
