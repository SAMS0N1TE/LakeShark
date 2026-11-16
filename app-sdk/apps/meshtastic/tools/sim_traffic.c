/* sim_traffic: Meshtastic frames for the simulator, so MESHTASTIC's screens
   can be looked at full instead of empty.

   Prints a LAPSIM_SCRIPT: node info, positions, telemetry and channel text
   from a handful of nodes around Buenos Aires, each a real frame on the
   default channel (LongFast, key AQ==), encrypted as a node would send it.

     cc -O1 -I.. -o sim_traffic sim_traffic.c ../mt_proto.c ../mt_aes.c -lm
     LAPSIM=meshtastic LAPSIM_SCRIPT="$(./sim_traffic)" lssim lap -A 40 -f 400 -o out.bmp

   `./sim_traffic N` stops after N steps (fewer messages), `-q` leaves the
   text out (nodes only), `-s F` spaces the frames F apart (40 ms each;
   the default 6). Frame numbers start at 20. */
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

static uint8_t key[32];
static int key_len;
static uint8_t hash;
static uint32_t next_id = 0x1000;
static int frame_no = 20, frame_step = 6, steps, max_steps = 1 << 30;

static void emit(const sim_node_t *n, uint32_t port, const uint8_t *pl, size_t pn)
{
    if (steps >= max_steps) return;
    uint8_t buf[MT_HEADER_LEN + MT_MAX_PAYLOAD];
    mt_header_t h;
    memset(&h, 0, sizeof(h));
    h.dest = MT_BROADCAST; h.from = n->num; h.id = next_id++;
    h.hop_start = 3; h.hop_limit = (uint8_t)(3 - n->hops); h.channel = hash;
    mt_header_pack(&h, buf);
    mt_data_t d = { port, pl, pn, false, 0 };
    const int dl = mt_data_encode(&d, buf + MT_HEADER_LEN, MT_MAX_PAYLOAD);
    if (dl <= 0) return;
    mt_crypt(key, key_len, h.from, h.id, buf + MT_HEADER_LEN, (size_t)dl);
    printf("%s%d:iorssi=%d;%d:packethex=", steps ? ";" : "", frame_no, n->rssi, frame_no);
    for (int i = 0; i < MT_HEADER_LEN + dl; i++) printf("%02x", buf[i]);
    frame_no += frame_step;
    steps++;
}

int main(int argc, char **argv)
{
    bool text = true;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-q")) text = false;
        else if (!strcmp(argv[i], "-s") && i + 1 < argc) frame_step = atoi(argv[++i]);
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
    for (int i = 0; text && i < NT; i++)
        emit(&NODES[TEXTS[i].node], MT_PORT_TEXT, (const uint8_t *)TEXTS[i].text, strlen(TEXTS[i].text));
    /* One node this board has no name for: a packet on a channel it cannot read. */
    const sim_node_t stranger = { 0x00C0FFEEu, "", "", 0, 0, 0, 0, 2, -1215 };
    hash ^= 0x55;
    emit(&stranger, MT_PORT_TEXT, (const uint8_t *)"x", 1);
    printf("\n");
    return 0;
}
