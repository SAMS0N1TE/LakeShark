/* MESHLING: a pocket creature that lives on radio.
 *
 * It eats MeshCore traffic: every packet the board hears is a snack that
 * flies in from the tower, and a node it has never heard is a feast. It
 * keeps company with the Bluetooth devices around it and gets lonely
 * without them. SNIFF has it nose through the Wi-Fi networks nearby; each
 * new one is a discovery. Messages on the mesh make it chatter. Everything
 * it has ever heard goes in its DEX, and the more it collects the more it
 * grows: egg, hatchling, sprout, and the full meshling. Stroke it with a
 * finger. It sleeps at night by the board's clock.
 *
 * Uses API 1.1: the mesh, Wi-Fi and BLE reads, lap_mesh_advert, lap_time,
 * lap_touch, lap_tone, lap_haptic; it lives in its own file pet.bin. */
#include "lakeshark_app.h"
#include "lap_canvas.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- what is kept -------------------------------------------------------- */

#define SEEN_NODES 96
#define SEEN_NETS 192
#define SEEN_BLE 192
#define PET_MAGIC 0x4D455348u          /* "MESH" */

typedef struct {
    uint32_t magic, version;
    char name[12];
    int64_t born, last;                /* Unix seconds, 0 when the clock is not set */
    float food, joy, energy;           /* 0..100 */
    uint32_t xp, packets, pets, messages;
    uint16_t n_nodes, n_nets, n_ble;
    uint32_t nodes[SEEN_NODES], nets[SEEN_NETS], ble[SEEN_BLE];   /* hashes of what it has met */
} pet_v1_t;

/* Keep the COMPLETE v1 object, including its trailing ABI padding. Old saves
 * are read into this prefix before reading the v2 tail; no large stack copy. */
typedef struct {
    pet_v1_t core;
    uint32_t diet[3];                  /* mesh, Bluetooth company, Wi-Fi */
    uint32_t forms;                    /* permanent DEX discovery bits */
} pet_t;
_Static_assert(sizeof(pet_v1_t) == 2000 && offsetof(pet_t, diet) == 2000,
               "pet.bin v1 ABI prefix must remain unchanged");
static pet_t pet;
enum { DIET_MESH, DIET_BLE, DIET_WIFI };
enum { FORM_EGG, FORM_HATCHLING, FORM_SPROUT, RELAY, SWARM, SCANNER,
       NEXUS, DRIFTER, RELIC, FORM_COUNT };
static const char *const FORM_NAME[FORM_COUNT] = {
    "EGG", "HATCHLING", "SPROUT", "RELAY", "SWARM", "SCANNER",
    "NEXUS", "DRIFTER", "RELIC"
};
static int active_form = -1;
static uint32_t evolve_at;

/* Bounded history: preserve the proportions and leave room for new meals. */
static void diet_add(int kind, uint32_t amount)
{
    if (amount > 65535u) amount = 65535u;
    if (pet.diet[0] + pet.diet[1] + pet.diet[2] > 1000000u)
        for (int i = 0; i < 3; i++) pet.diet[i] /= 2;
    pet.diet[kind] += amount;
}

enum { EGG, HATCHLING, SPROUT, MESHLING };
static const uint32_t STAGE_XP[4] = { 0, 40, 300, 1200 };
static const char *const STAGE_NAME[4] = { "EGG", "HATCHLING", "SPROUT", "MESHLING" };

static int stage(void)
{
    int s = EGG;
    for (int i = 1; i < 4; i++) if (pet.core.xp >= STAGE_XP[i]) s = i;
    return s;
}

static uint32_t fnv(const void *p, size_t n)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) { h ^= ((const uint8_t *)p)[i]; h *= 16777619u; }
    return h ? h : 1;
}
/* True when h is new; remembers it if there is room. */
static bool meet(uint32_t *set, uint16_t *n, int cap, uint32_t h)
{
    for (int i = 0; i < *n; i++) if (set[i] == h) return false;
    if (*n < cap) set[(*n)++] = h;
    else set[h % (uint32_t)cap] = h;
    return true;
}

/* Health branches have a recovery margin, so a small snack cannot make the
 * silhouette oscillate. Sleep is a mood, never a reason to become neglected. */
static int form(void)
{
    const int st = stage();
    if (st != MESHLING) return st;
    if ((pet.core.food < 1 && pet.core.joy < 1) ||
        (active_form == RELIC && pet.core.food < 12 && pet.core.joy < 12)) return RELIC;
    if (pet.core.food < 20 || pet.core.joy < 20 ||
        (active_form == DRIFTER && (pet.core.food < 30 || pet.core.joy < 30))) return DRIFTER;
    const uint32_t total = pet.diet[0] + pet.diet[1] + pet.diet[2];
    for (int i = 0; i < 3; i++)
        if (total && pet.diet[i] * 100u >= total * 60u) return RELAY + i;
    return NEXUS;
}

static void load(void)
{
    memset(&pet, 0, sizeof(pet));
    int fd = lap_file_open("pet.bin", "r");
    if (fd >= 0) {
        const int n = lap_file_read(fd, &pet.core, sizeof(pet.core));
        bool ok = n == (int)sizeof(pet.core) && pet.core.magic == PET_MAGIC;
        if (ok && pet.core.version == 1) {
            /* Historical discoveries are the best available diet estimate. */
            diet_add(DIET_MESH, pet.core.packets);
            diet_add(DIET_MESH, 25u * pet.core.n_nodes);
            diet_add(DIET_BLE, 10u * pet.core.n_ble);
            diet_add(DIET_WIFI, 25u * pet.core.n_nets);
            for (int i = 0; i <= stage(); i++)
                pet.forms |= 1u << (i == MESHLING ? form() : i);
        } else if (ok && pet.core.version == 2) {
            ok = lap_file_read(fd, pet.diet, sizeof(pet) - sizeof(pet.core)) ==
                 (int)(sizeof(pet) - sizeof(pet.core));
        } else ok = false;
        ok = ok && pet.core.n_nodes <= SEEN_NODES && pet.core.n_nets <= SEEN_NETS &&
             pet.core.n_ble <= SEEN_BLE && isfinite(pet.core.food) &&
             isfinite(pet.core.joy) && isfinite(pet.core.energy);
        if (!ok) memset(&pet, 0, sizeof(pet));
        lap_file_close(fd);
    }
    if (pet.core.magic != PET_MAGIC) {
        pet.core.magic = PET_MAGIC;
        static const char *const NAMES[] = { "BLIP", "PIP", "NODO", "ZAP", "LORA", "MOTE", "CHIRP", "BYTE" };
        snprintf(pet.core.name, sizeof(pet.core.name), "%s", NAMES[lap_random() % 8]);
        pet.core.food = 70; pet.core.joy = 70; pet.core.energy = 90;
    }
    pet.core.version = 2;
    for (int i = 0; i < 3; i++) if (pet.diet[i] > 1065535u) pet.diet[i] = 1065535u;
    pet.forms &= (1u << FORM_COUNT) - 1;
}
static void save(void)
{
    int fd = lap_file_open("pet.bin", "w");
    if (fd < 0) return;
    lap_file_write(fd, &pet, sizeof(pet));
    lap_file_close(fd);
}

/* ---- the moment ---------------------------------------------------------- */

static lap_canvas_t cv;
static uint32_t last_ms, tick_ms, radio_ms, born_ms;
static int64_t now_unix;
static int hour = 12, minute;
static bool asleep, clock_ok;
static float px_pet = 0.5f, target_x = 0.5f, bob;  /* position as a fraction of the width */
static bool facing_left;
static uint32_t blink_at, chomp_until, hearts_until, wake_until, sniff_until, ping_until;
static uint32_t last_rx, msg_seq, wifi_seq, last_msg_id;
static int near_ble, mesh_nodes;
static bool mesh_up, tx_armed;

typedef struct { float x, y, vx, vy; uint32_t until; uint8_t kind; } fx_t;   /* 0 packet, 1 heart, 2 z, 3 spark */
#define FX 24
static fx_t fx[FX];

static char toast[48];
static uint32_t toast_until;
static char bubble[64];
static uint32_t bubble_until;

static int page;                       /* 0 the room, 1 the dex */
static lap_mesh_node_t nodes[12];
static lap_wifi_ap_t aps[16];
static lap_ble_dev_t devs[24];
static int n_aps;

static void say(const char *t, uint32_t ms) { snprintf(toast, sizeof(toast), "%s", t); toast_until = lap_millis() + ms; }

static void spawn(uint8_t kind, float x, float y, float vx, float vy, uint32_t life)
{
    for (int i = 0; i < FX; i++)
        if (fx[i].until <= lap_millis()) { fx[i] = (fx_t){ x, y, vx, vy, lap_millis() + life, kind }; return; }
}

static void gain(uint32_t xp)
{
    const int before = stage();
    pet.core.xp = xp > UINT32_MAX - pet.core.xp ? UINT32_MAX : pet.core.xp + xp;
    for (int i = before; i <= stage() && i < MESHLING; i++) pet.forms |= 1u << i;
    if (stage() != before) {
        char t[48];
        snprintf(t, sizeof(t), before == EGG ? "%s HATCHED!" : "%s GREW!", pet.core.name);
        say(t, 3500);
        lap_tone(0, 1, 0, LAP_WAVE_SINE);
        static const int up[] = { 523, 659, 784, 1047, 1319 };
        for (int i = 0; i < 5; i++) lap_tone(up[i], 90, 45, LAP_WAVE_SQUARE);
        lap_haptic(LAP_HAPTIC_CONFIRM);
        for (int i = 0; i < 12; i++) spawn(3, px_pet, 0.62f, (lc_frand() - .5f) * 0.6f, -0.2f - lc_frand() * 0.5f, 900);
    }
}
static void evolve(void)
{
    const int next = form();
    pet.forms |= 1u << next;
    if (next == active_form) return;
    if (active_form >= 0) {
        evolve_at = lap_millis();
        char t[48];
        snprintf(t, sizeof(t), "%s > %s", FORM_NAME[active_form], FORM_NAME[next]);
        say(t, 1800);
    }
    active_form = next;
}

static void feed(float f) { pet.core.food = fminf(100, pet.core.food + f); }
static void cheer(float j) { pet.core.joy = fminf(100, pet.core.joy + j); }

/* ---- the radios ----------------------------------------------------------- */

static void listen(void)
{
    const uint32_t now = lap_millis();
    lap_mesh_self_t me;
    mesh_up = lap_mesh_self(&me) && me.running;
    tx_armed = mesh_up && me.tx_armed;
    if (mesh_up) {
        if (last_rx && me.rx_packets > last_rx) {
            /* Every packet heard is a snack, flown in from the tower. */
            const uint32_t n = me.rx_packets - last_rx;
            for (uint32_t i = 0; i < n && i < 4; i++) spawn(0, (cv.w - 12.0f) / (cv.w ? cv.w : 1), 0.18f, 0, 0, 1400 + i * 120);
            pet.core.packets += n;
            if (!asleep) diet_add(DIET_MESH, n);
            if (!asleep) { feed(2.0f * n); gain(n); }
            lap_tone(1760, 15, 12, LAP_WAVE_SINE);
        }
        last_rx = me.rx_packets;
        mesh_nodes = lap_mesh_nodes(nodes, 12);
        int fresh = 0, first = -1;
        for (int i = 0; i < mesh_nodes; i++)
            if (meet(pet.core.nodes, &pet.core.n_nodes, SEEN_NODES, fnv(nodes[i].id, 16))) { fresh++; if (first < 0) first = i; }
        if (fresh) {
            /* One notice for the lot, however many arrived together. */
            char t[48];
            if (fresh == 1) snprintf(t, sizeof(t), "NEW NODE: %s", nodes[first].name[0] ? nodes[first].name : nodes[first].id);
            else snprintf(t, sizeof(t), "MET %d NEW NODES", fresh);
            say(t, 3000);
            diet_add(DIET_MESH, 25u * (uint32_t)fresh);
            feed(fminf(60, 18.0f * fresh)); cheer(6); gain(25u * (uint32_t)fresh);
            chomp_until = now + 900;
            lap_tone(0, 1, 0, LAP_WAVE_SINE);
            lap_tone(988, 60, 30, LAP_WAVE_TRIANGLE);
            lap_tone(1319, 120, 30, LAP_WAVE_TRIANGLE);
            static uint32_t buzzed;
            if (!buzzed || now - buzzed > 30000) { buzzed = now ? now : 1; lap_haptic(LAP_HAPTIC_TICK); }
        }
        const uint32_t seq = lap_mesh_seq();
        if (seq != msg_seq) {
            msg_seq = seq;
            static lap_mesh_msg_t msgs[6];
            const int n = lap_mesh_messages(msgs, 6);
            for (int i = 0; i < n; i++)
                if (!msgs[i].mine && msgs[i].id > last_msg_id) {
                    last_msg_id = msgs[i].id;
                    snprintf(bubble, sizeof(bubble), "%s", msgs[i].text);
                    bubble_until = now + 6000;
                    pet.core.messages++;
                    cheer(4); gain(3);
                }
        }
    }
    /* Company: Bluetooth devices about. */
    const int nd = lap_ble_devices(devs, 24);
    near_ble = 0;
    for (int i = 0; i < nd; i++) {
        if (devs[i].age_ms < 30000) near_ble++;
        if (meet(pet.core.ble, &pet.core.n_ble, SEEN_BLE, fnv(devs[i].addr, 6))) { diet_add(DIET_BLE, 10); cheer(3); gain(2); }
    }
    /* Nearby company accrues once per minute, capped at three devices. This
     * lets a stable household grow a pet without farming new addresses. */
    static uint32_t company_ms;
    if (now - company_ms >= 60000) {
        company_ms = now;
        if (near_ble && !asleep) {
            const uint32_t company = (uint32_t)(near_ble > 3 ? 3 : near_ble);
            diet_add(DIET_BLE, company * 10); gain(company * 2);
        }
    }
    /* Wi-Fi, after a SNIFF. */
    const uint32_t ws = lap_wifi_seq();
    if (ws != wifi_seq) {
        wifi_seq = ws;
        n_aps = lap_wifi_aps(aps, 16);
        int fresh = 0;
        for (int i = 0; i < n_aps; i++)
            if (aps[i].ssid[0] && meet(pet.core.nets, &pet.core.n_nets, SEEN_NETS, fnv(aps[i].ssid, strlen(aps[i].ssid)))) fresh++;
        if (fresh) {
            char t[48];
            snprintf(t, sizeof(t), "SNIFFED %d NEW NETWORK%s", fresh, fresh > 1 ? "S" : "");
            say(t, 2500);
            diet_add(DIET_WIFI, 25u * (uint32_t)fresh);
            cheer(2.0f * fresh); gain(3u * fresh);
            lap_tone(1200, 40, 30, LAP_WAVE_SQUARE);
            lap_tone(1500, 40, 30, LAP_WAVE_SQUARE);
        } else if (sniff_until) say("NOTHING NEW TO SNIFF", 1800);
        sniff_until = 0;
    }
}

/* Once a second: hunger, loneliness, tiredness, sleep. */
static void live(float minutes)
{
    const bool night = clock_ok && (hour >= 22 || hour < 7);
    pet.core.food = fmaxf(0, pet.core.food - minutes / 3.0f);
    pet.core.joy = fmaxf(0, pet.core.joy - minutes / 4.0f + minutes * 0.25f * (near_ble > 3 ? 3 : near_ble));
    pet.core.joy = fminf(100, pet.core.joy);
    if (asleep) pet.core.energy = fminf(100, pet.core.energy + minutes * 1.5f);
    else pet.core.energy = fmaxf(0, pet.core.energy - minutes / (night ? 2.0f : 6.0f));
    const bool wake = lap_millis() < wake_until;
    if (!asleep && !wake && stage() != EGG && (pet.core.energy < 15 || (night && pet.core.energy < 70))) { asleep = true; say("ZZZ...", 1500); }
    if (asleep && (pet.core.energy > 95 || (!night && pet.core.energy > 60))) { asleep = false; say("GOOD MORNING!", 2000); }
}

/* ---- layout ----------------------------------------------------------------
   The board's own look for everything but the room: its font, its box
   frames, its colours. The room is a window of flat, chunky dots. */

typedef struct { int x, y, w, h; } box_t;     /* cells */
static box_t room, b_ping, b_sniff, b_dex;
static int hud_rows;
static int scale, ground_y;                    /* in the room canvas, dots */
static int pet_x0, pet_y0, pet_w, pet_h;       /* where the pet was last drawn, room dots */
static float sky_t;                            /* 0 night .. 1 day */

static void layout(void)
{
    const int cols = lap_cols(), rows = lap_rows();
    const bool wide = cols > rows * 3 / 2;
    hud_rows = cols >= 50 ? 6 : 8;              /* narrow: the meters stack */
    if (wide) {
        /* Landscape: the buttons in a column on the right, under the close button. */
        const int bw = cols / 5 < 12 ? 12 : cols / 5, bx = cols - bw - 1;
        const int bh = (rows - 3 - 3) / 3;
        b_ping = (box_t){ bx, 3, bw, bh };
        b_sniff = (box_t){ bx, 3 + bh + 1, bw, bh };
        b_dex = (box_t){ bx, 3 + 2 * bh + 2, bw, bh };
        room = (box_t){ 0, hud_rows, bx - 1, rows - hud_rows };
    } else {
        /* Portrait: buttons along the bottom, in from the round corners. */
        const int bh = 4, by = rows - bh - 2, gap = 1, side = 3;
        const int bw = (cols - 2 * side - 2 * gap) / 3;
        b_ping = (box_t){ side, by, bw, bh };
        b_sniff = (box_t){ side + bw + gap, by, bw, bh };
        b_dex = (box_t){ cols - side - bw, by, bw, bh };
        room = (box_t){ 0, hud_rows, cols, by - 1 - hud_rows };
    }
    if (!lap_canvas_init(&cv, room.w - 2, room.h - 2)) return;
    ground_y = cv.h - cv.h / 5;
    int sc = (ground_y * 40 / 100) / 14;
    if (cv.w / 20 < sc) sc = cv.w / 20;
    scale = sc < 2 ? 2 : sc > 6 ? 6 : sc;
}

/* ---- the heads-up, in text ------------------------------------------------- */

#define A(fg) LAP_ATTR(fg, LAP_BLACK)

static void bar(int x, int y, int w, float v, uint8_t col)
{
    const int n = (int)(w * fminf(100, fmaxf(0, v)) / 100 + 0.5f);
    for (int i = 0; i < w; i++) lap_put_cell(x + i, y, i < n ? LAP_BLOCK : LAP_SHADE_25, A(i < n ? col : LAP_BLACK | LAP_BRIGHT));
}

static void meter(int x, int y, const char *label, float v, uint8_t col)
{
    const bool low = v < 25 && (lap_millis() / 400) % 2;
    lap_put_str(x, y, label, A(low ? LAP_RED | LAP_BRIGHT : LAP_WHITE));
    bar(x + 5, y, 9, v, low ? LAP_RED | LAP_BRIGHT : col);
}

static void draw_hud(void)
{
    const int cols = lap_cols(), st = stage();
    static const uint8_t HUE[4] = { LAP_WHITE, LAP_CYAN, LAP_CYAN | LAP_BRIGHT, LAP_WHITE | LAP_BRIGHT };
    char t[64];
    lap_put_str(1, 0, pet.core.name, A(LAP_WHITE | LAP_BRIGHT));
    lap_put_str(2 + (int)strlen(pet.core.name), 0, FORM_NAME[active_form < 0 ? form() : active_form], A(HUE[st]));
    /* How far into this form. */
    if (st < MESHLING) {
        const float p = 100.0f * (pet.core.xp - STAGE_XP[st]) / (STAGE_XP[st + 1] - STAGE_XP[st]);
        snprintf(t, sizeof(t), "XP %lu", (unsigned long)pet.core.xp);
        lap_put_str(1, 1, t, A(LAP_BLACK | LAP_BRIGHT));
        const int bx = 2 + (int)strlen(t);
        bar(bx, 1, 12, p, HUE[st + 1]);
        snprintf(t, sizeof(t), "%s at %lu", STAGE_NAME[st + 1], (unsigned long)STAGE_XP[st + 1]);
        if (bx + 13 + (int)strlen(t) < cols - 5) lap_put_str(bx + 13, 1, t, A(LAP_BLACK | LAP_BRIGHT));
    } else {
        snprintf(t, sizeof(t), "XP %lu  fully grown", (unsigned long)pet.core.xp);
        lap_put_str(1, 1, t, A(LAP_BLACK | LAP_BRIGHT));
    }
    if (hud_rows == 6) {
        meter(1, 3, "FOOD", pet.core.food, LAP_YELLOW);
        meter(18, 3, "JOY", pet.core.joy, LAP_CYAN);
        meter(35, 3, "ZZZ", pet.core.energy, LAP_BLUE | LAP_BRIGHT);
    } else {
        meter(1, 3, "FOOD", pet.core.food, LAP_YELLOW);
        meter(1, 4, "JOY", pet.core.joy, LAP_CYAN);
        meter(1, 5, "ZZZ", pet.core.energy, LAP_BLUE | LAP_BRIGHT);
    }
    /* What it can hear right now. */
    const int y = hud_rows - 2;
    int x = 1;
    snprintf(t, sizeof(t), mesh_up ? "MESH %d nodes%s" : "MESH off", mesh_nodes, tx_armed ? "" : ", tx off");
    lap_put_str(x, y, t, A(mesh_up ? LAP_GREEN | LAP_BRIGHT : LAP_BLACK | LAP_BRIGHT));
    x += (int)strlen(t) + 3;
    snprintf(t, sizeof(t), "BT %d near", near_ble);
    lap_put_str(x, y, t, A(near_ble ? LAP_CYAN | LAP_BRIGHT : LAP_BLACK | LAP_BRIGHT));
    x += (int)strlen(t) + 3;
    if (x + 10 < cols) {
        snprintf(t, sizeof(t), "NETS %u", pet.core.n_nets);
        lap_put_str(x, y, t, A(LAP_GREEN));
    }
}

/* ---- the room, the way COMPASS draws ----------------------------------------
   Black, a grid floor going away from you, a lattice mast, and the creature:
   a lit metal sphere, every dot shaded from the upper left and dithered
   between the theme's own inks. */

static const uint8_t METAL[4] = { LC_BLUE, LC_GREY, LC_WHITE, LC_BR_WHITE };
static const uint8_t DARK_METAL[4] = { LC_BLACK, LC_BLUE, LC_GREY, LC_WHITE };
static const float LX = -0.45f, LY = 0.35f, LZ = 0.82f;   /* toward the light, y up */

static void draw_sky(void)
{
    const float h = hour + minute / 60.0f;
    sky_t = !clock_ok ? 1.0f : h < 6 || h >= 20 ? 0 : h < 8 ? (h - 6) / 2 : h < 18 ? 1 : (20 - h) / 2;
    const int hz = ground_y - cv.h / 6;                       /* the horizon */
    lc_fill_rect(&cv, 0, 0, cv.w, cv.h, LC_BLACK);
    /* A low glow along the horizon by day; stars by night. */
    const int glow_top = hz - hz / 3;
    for (int y = glow_top; y < hz; y++) {
        const float g = (float)(y - glow_top) / (hz - glow_top);
        const float b = sky_t * g * 0.6f;
        for (int x = 0; x < cv.w; x++) if (b > lc_bayer(x, y)) lc_pset(&cv, x, y, LC_BLUE);
    }
    const uint32_t t = lap_millis();
    if (sky_t < 0.5f)
        for (int i = 0; i < 30; i++) {
            const uint32_t k = (uint32_t)i * 2654435761u;
            const int x = (int)(k % (uint32_t)cv.w), y = (int)((k >> 12) % (uint32_t)(hz * 3 / 4 + 1));
            if (((t / 400 + i * 7) % 13) != 0) lc_pset(&cv, x, y, i % 5 ? LC_GREY : LC_WHITE);
        }
    /* The floor: a grid in perspective, one dot wide wherever a grid line
       falls between this dot and the next, fading with distance. */
    for (int y = hz + 1; y < cv.h; y++) {
        const float depth = 14.0f / (y - hz), next = 14.0f / (y + 1 - hz);
        const float fade = fminf(1.0f, 2.0f / depth);
        const bool row_line = depth < 5.0f && floorf(depth * 2.5f) != floorf(next * 2.5f);   /* none crowding the horizon */
        for (int x = 0; x < cv.w; x++) {
            const float wx = (x - cv.w / 2) * depth * 0.16f, wn = (x + 1 - cv.w / 2) * depth * 0.16f;
            const bool col_line = floorf(wx) != floorf(wn);
            if ((row_line || col_line) && fade > lc_bayer(x, y)) lc_pset(&cv, x, y, fade > 0.7f ? LC_GREY : LC_BLUE);
        }
    }
}

/* The mast on the right, in thin metal; rings go out when packets come in. */
static void draw_tower(void)
{
    const int hz = ground_y - cv.h / 6, x = cv.w - 12, top = hz / 3, base = hz + 4;
    for (int y = top; y < base; y++) {
        const int half = (y - top) / 6 + 1;
        lc_pset(&cv, x - half, y, LC_GREY);
        lc_pset(&cv, x + half, y, LC_GREY);
        if ((y - top) % 6 == 0) for (int k = -half; k <= half; k++) lc_pset(&cv, x + k, y, k & 1 ? LC_BLUE : LC_GREY);
        else if ((y - top) % 6 == 3) { lc_pset(&cv, x - half / 2, y, LC_BLUE); lc_pset(&cv, x + half / 2, y, LC_BLUE); }
    }
    const uint32_t t = lap_millis();
    lc_pset(&cv, x, top - 1, (t / 600) % 2 ? LC_BR_RED : LC_RED);
    const uint32_t since = t - radio_ms;
    if (mesh_up && since < 1500) {
        const int r = 2 + (int)(since / 80);
        for (int a = 0; a < 48; a++) {
            const float th = a * 0.1309f;
            if (((a + (int)(since / 40)) & 3) == 0) continue;                 /* a dashed ring */
            lc_pset(&cv, x + (int)roundf(cosf(th) * r), top - 1 + (int)roundf(sinf(th) * r * 0.8f), r < 10 ? LC_CYAN : LC_BLUE);
        }
    }
}

/* A lit sphere (or a squashed one): metal ramp, Bayer dither, a hot spot. */
static void sphere(float cx, float cy, float rx, float ry, const uint8_t *ramp, float boost)
{
    const int x0 = (int)(cx - rx) - 1, x1 = (int)(cx + rx) + 1, y0 = (int)(cy - ry) - 1, y1 = (int)(cy + ry) + 1;
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            const float nx = (x + 0.5f - cx) / rx, ny = (y + 0.5f - cy) / ry, d2 = nx * nx + ny * ny;
            if (d2 > 1) continue;
            const float nz = sqrtf(1 - d2);
            const float lam = fmaxf(0, nx * LX - ny * LY + nz * LZ);
            const float rz = 2 * lam * nz - LZ;                              /* reflection toward the eye */
            float b = 0.10f + 0.70f * lam + boost;
            if (rz > 0.93f) b += (rz - 0.93f) * 6;                           /* the hot spot */
            if (d2 > 0.86f) b *= 0.7f;                                       /* a darker rim reads as round */
            lc_pset(&cv, x, y, lc_ramp(b, x, y, ramp, 4));
        }
}

/* Rounded antenna stock. Its projected surface normal is evaluated per dot,
 * including the end caps; the long direction costs only a narrow bounding box. */
static void capsule(float ax, float ay, float bx, float by, float r, const uint8_t *ramp)
{
    const float dx = bx - ax, dy = by - ay, len2 = dx * dx + dy * dy;
    const float inv = len2 > 0 ? 1.0f / len2 : 0;
    const int x0 = (int)(fminf(ax, bx) - r - 1), x1 = (int)(fmaxf(ax, bx) + r + 1);
    const int y0 = (int)(fminf(ay, by) - r - 1), y1 = (int)(fmaxf(ay, by) + r + 1);
    for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++) {
        const float u = lc_clampf(((x + .5f - ax) * dx + (y + .5f - ay) * dy) * inv, 0, 1);
        const float nx = (x + .5f - ax - u * dx) / r, ny = (y + .5f - ay - u * dy) / r;
        const float d = nx * nx + ny * ny;
        if (d > 1) continue;
        const float lam = fmaxf(0, nx * LX - ny * LY + sqrtf(1 - d) * LZ);
        lc_pset(&cv, x, y, lc_ramp(.1f + .8f * lam, x, y, ramp, 4));
    }
}

/* A concave reflector: the normal points inward; a bright rolled rim and
 * central feed distinguish the dish from another flattened sphere. */
static void dish(float cx, float cy, float rx, float ry, float sweep)
{
    for (int y = (int)(cy - ry - 1); y <= (int)(cy + ry + 1); y++)
        for (int x = (int)(cx - rx - 1); x <= (int)(cx + rx + 1); x++) {
            const float nx = (x + .5f - cx) / rx, ny = (y + .5f - cy) / ry;
            const float d = nx * nx + ny * ny;
            if (d > 1) continue;
            const float nz = sqrtf(1 - d);
            const float lam = fmaxf(0, -nx * LX + ny * LY + nz * LZ);
            float b = .08f + .65f * lam;
            if (d > .72f) b = .3f + .6f * fmaxf(0, nx * LX - ny * LY + nz * LZ);
            lc_pset(&cv, x, y, lc_ramp(b, x, y, METAL, 4));
        }
    capsule(cx, cy, cx + rx * .55f * sweep, cy - ry * .7f, 1.0f, DARK_METAL);
    sphere(cx, cy, 2, 2, METAL, .05f);
    lc_pset(&cv, (int)cx, (int)cy - 1, LC_BR_CYAN);
}

/* A metal hoop, split for correct occlusion around the body. */
static void hoop(float cx, float cy, float rx, float ry, bool front, float spin)
{
    for (int a = 0; a < 64; a++) {
        const float th = a * .09817477f;
        const float sn = sinf(th), cs = cosf(th);
        if ((sn >= 0) != front) continue;
        const int x = (int)(cx + cs * rx), y = (int)(cy + sn * ry);
        const float lam = fmaxf(0, cs * LX - sn * LY + .6f * LZ);
        lc_pset(&cv, x, y, lc_ramp(.2f + .7f * lam, x, y, METAL, 4));
        lc_pset(&cv, x, y + 1, LC_BLUE);
    }
    if ((sinf(spin) >= 0) == front)
        sphere(cx + cosf(spin) * rx, cy + sinf(spin) * ry, 2, 2, METAL, 0);
}

/* The creature's place in the room, set each frame. */
static float pcx, pcy, prx, pry;

static void draw_pet(void)
{
    const int st = stage();
    const uint32_t t = lap_millis();
    const int f = active_form < 0 ? form() : active_form;
    /* Reserve space for RELAY's full aerial even in the short landscape room. */
    const float R = fmaxf(3, fminf((float)scale * 7.0f,
                            fminf((ground_y - 3) / 3.65f, (cv.w - 6) / 3.7f)));
    const float hop = asleep ? 0 : fabsf(sinf(bob)) * R * 0.12f;
    const float squash = asleep ? .03f : (1 - fabsf(sinf(bob))) * .04f;
    /* Deliberate silhouettes, not just a change in the number of antennae. */
    static const float WIDTH[FORM_COUNT]  = { .78f, .64f, .83f, .70f, .72f, .85f, .93f, 1.0f, .86f };
    static const float HEIGHT[FORM_COUNT] = { 1.0f, .73f, .83f, 1.08f, .78f, .70f, .92f, .57f, .76f };
    const float reach = R * 1.85f + 2;
    pcx = lc_clampf(px_pet * cv.w, reach, cv.w - reach);
    px_pet = pcx / cv.w;
    target_x = lc_clampf(target_x, reach / cv.w, 1 - reach / cv.w);
    prx = R * WIDTH[f] * (1 + squash); pry = R * HEIGHT[f] * (1 - squash);
    pcy = ground_y - pry - hop - (f == SWARM ? R * .3f : 0);
    pet_x0 = (int)(pcx - reach); pet_y0 = (int)(pcy - pry - R * 1.55f);
    pet_w = (int)(2 * reach); pet_h = (int)(ground_y - pet_y0 + 3);
    /* Its shadow on the grid, a dithered ellipse that shrinks as it rises. */
    const float sr = prx * (1.1f - hop / (R + 1) * 0.5f);
    for (int y = -3; y <= 3; y++)
        for (int x = (int)-sr; x <= (int)sr; x++)
            if ((x * x) / (sr * sr) + (y * y) / 9.0f <= 1 && lc_bayer(x, y) < 0.6f) lc_pset(&cv, (int)pcx + x + 3, ground_y + y, LC_BLACK);
    const bool faded = f == RELIC;
    const bool glow = t < chomp_until || (t - radio_ms) < 400;
    if (st == EGG) {
        sphere(pcx, pcy, prx, pry, METAL, 0);
        /* A seam round the middle and cracks as it nears hatching. */
        for (int x = (int)(pcx - prx); x <= (int)(pcx + prx); x++) lc_pset(&cv, x, (int)pcy + 1, LC_BLUE);
        const float due = (float)pet.core.xp / STAGE_XP[1];
        if (due > 0.4f) lc_line(&cv, (int)pcx - 3, (int)(pcy - pry * 0.4f), (int)pcx + 1, (int)(pcy - pry * 0.1f), LC_BLACK);
        if (due > 0.7f) lc_line(&cv, (int)pcx + 1, (int)(pcy - pry * 0.1f), (int)pcx + 4, (int)(pcy - pry * 0.5f), LC_BLACK);
        return;
    }
    const uint8_t *metal = faded ? DARK_METAL : METAL;
    const float spin = t / 1200.0f;
    const float mast_y = pcy - pry;
    if (f == FORM_HATCHLING) {
        capsule(pcx, mast_y + 2, pcx, mast_y - R * .4f, 1.2f, METAL);
        sphere(pcx, mast_y - R * .4f, 2, 2, METAL, 0);
        lc_pset(&cv, (int)pcx, (int)(mast_y - R * .4f) - 1, glow ? LC_BR_YELLOW : LC_RED);
    } else if (f == FORM_SPROUT) {
        for (int side = -1; side <= 1; side += 2) {
            capsule(pcx + side * R * .7f, pcy, pcx + side * R, mast_y - R * .45f, 1.3f, METAL);
            sphere(pcx + side * R * .85f, pcy - R * .15f, R * .26f, R * .45f, METAL, 0);
            lc_pset(&cv, (int)(pcx + side * R), (int)(mast_y - R * .45f), glow ? LC_BR_YELLOW : LC_RED);
        }
    } else if (f == RELAY) {
        capsule(pcx, mast_y + 3, pcx, mast_y - R * 1.35f, 1.7f, METAL);
        for (int j = 0; j < 3; j++) {
            const float y = mast_y - R * (.35f + j * .35f), w = R * (.65f - j * .13f);
            capsule(pcx - w, y, pcx + w, y, 1.1f, METAL);
        }
        lc_pset(&cv, (int)pcx, (int)(mast_y - R * 1.35f) - 1, (t / 500) % 2 ? LC_BR_RED : LC_RED);
        capsule(pcx - R * .6f, pcy, pcx - R, pcy - R * .75f, 1.2f, METAL);
        dish(pcx - R * .95f, pcy - R * .8f, R * .48f, R * .3f, sinf(spin) * .3f);
    } else if (f == SCANNER) {
        capsule(pcx, mast_y + 3, pcx, mast_y - R * .7f, 2, METAL);
        const float cy = mast_y - R * .75f, rx = R * (1.12f + .18f * sinf(spin));
        dish(pcx, cy, rx, R * .47f, sinf(spin));
        const float sx = sinf(spin * 1.3f) * rx * .8f;
        const float sy = R * .36f * sqrtf(1 - sx * sx / (rx * rx));
        lc_line(&cv, (int)(pcx + sx), (int)(cy - sy), (int)(pcx + sx), (int)(cy + sy), LC_CYAN);
    } else if (f == NEXUS) {
        capsule(pcx, mast_y, pcx, mast_y - R * .6f, 1.4f, METAL);
        sphere(pcx, mast_y - R * .6f, 2, 2, METAL, 0);
        hoop(pcx, pcy + R * .3f, R * 1.45f, R * .45f, false, spin);
    } else if (f == DRIFTER || f == RELIC) {
        /* Folded aerial and broad ballast vs. a split, unlit shell. */
        const float hinge_x = pcx - R * .4f, hinge_y = mast_y - 2;
        capsule(pcx, mast_y + 3, hinge_x, hinge_y, 1.4f, metal);
        capsule(hinge_x, hinge_y, hinge_x - R * .75f, hinge_y + R * .28f, 1.4f, metal);
        sphere(pcx - R * .9f, pcy + R * .25f, R * .4f, R * .27f, metal, 0);
        sphere(pcx + R * .9f, pcy + R * .25f, R * .4f, R * .27f, metal, 0);
    }
    /* Satellite orbs pass behind the central receiver, then in front. */
    if (f == SWARM)
        for (int i = 0; i < 3; i++) {
            const float th = spin + i * 2.094395f;
            if (sinf(th) >= 0) continue;
            sphere(pcx + cosf(th) * R * 1.4f, pcy + sinf(th) * R * .8f,
                   R * .32f, R * .32f, METAL, 0);
        }
    sphere(pcx, pcy, prx, pry, metal, 0);
    if (f == RELIC) {
        lc_line(&cv, (int)(pcx - R * .1f), (int)(pcy - pry + 1), (int)(pcx - R * .35f), (int)(pcy - pry * .55f), LC_BLACK);
        lc_line(&cv, (int)(pcx - R * .35f), (int)(pcy - pry * .55f), (int)(pcx + R * .1f), (int)(pcy - pry * .42f), LC_BLACK);
    }
    /* The face: two recessed slots with LEDs, and a grille. They turn a
       little toward where it is looking, as on the surface of the ball. */
    int look = facing_left ? -1 : 1;
    lap_touch_t tc;
    if (lap_touch(&tc) && tc.down) {
        int cw, ch;
        lap_cell_px(&cw, &ch);
        const int fx2 = tc.x * 2 / cw - 2 * (room.x + 1);
        look = fx2 < pcx - 4 ? -1 : fx2 > pcx + 4 ? 1 : 0;
    }
    const float turn = look * prx * 0.18f;
    const int ey = (int)(pcy - pry * 0.18f), ew = (int)(prx * 0.22f) + 1, eh = (int)(pry * 0.28f) + 1;
    const bool blink = (t % 4100) < 150 || t < blink_at;
    for (int e = -1; e <= 1; e += 2) {
        const int ex = (int)(pcx + e * prx * 0.38f + turn);
        if (asleep || blink) { lc_fill_rect(&cv, ex - ew, ey, 2 * ew + 1, 1, LC_BLACK); continue; }
        lc_fill_rect(&cv, ex - ew, ey - eh, 2 * ew + 1, 2 * eh + 1, LC_BLACK);
        if (t < hearts_until) {
            /* Content: the LEDs curve up. */
            lc_pset(&cv, ex - 1, ey, LC_BR_CYAN); lc_pset(&cv, ex, ey - 1, LC_BR_CYAN); lc_pset(&cv, ex + 1, ey, LC_BR_CYAN);
        } else {
            const bool low = pet.core.food < 25 || pet.core.joy < 25;
            lc_fill_rect(&cv, ex - 1 + look, ey - 1 + (low ? 1 : 0), 2, 2, low ? LC_CYAN : LC_BR_CYAN);
        }
    }
    /* The grille: open when it eats, a short slot when it sleeps. */
    const int my = (int)(pcy + pry * 0.35f), mw = (int)(prx * 0.35f) + 1;
    if (t < chomp_until && (t / 140) % 2) lc_fill_rect(&cv, (int)(pcx + turn) - mw, my - 1, 2 * mw + 1, 3, LC_BLACK);
    else
        for (int x = -mw; x <= mw; x += 2) lc_pset(&cv, (int)(pcx + turn) + x, my, asleep && abs(x) > 1 ? LC_GREY : LC_BLACK);
    if (f == NEXUS) hoop(pcx, pcy + R * .3f, R * 1.45f, R * .45f, true, spin);
    if (f == SWARM)
        for (int i = 0; i < 3; i++) {
            const float th = spin + i * 2.094395f;
            if (sinf(th) < 0) continue;
            const float x = pcx + cosf(th) * R * 1.4f, y = pcy + sinf(th) * R * .8f;
            sphere(x, y, R * .32f, R * .32f, METAL, 0);
            lc_pset(&cv, (int)x, (int)y, (t / 400 + i) % 3 ? LC_CYAN : LC_BR_CYAN);
        }

}

/* Two moving white scan rows across the body. Restrict the effect to the
 * body ellipse so background hardware inside the touch bounds stays dark. */
static void draw_evolution(void)
{
    const uint32_t elapsed = lap_millis() - evolve_at;
    if (!evolve_at || elapsed >= 1100) return;
    const int line = (int)(pcy - pry + elapsed * (2 * pry) / 1100);
    for (int y = line; y <= line + 1; y++) {
        if (y < 0 || y >= cv.h) continue;
        for (int x = pet_x0; x < pet_x0 + pet_w; x++) {
            const float nx = (x + .5f - pcx) / prx, ny = (y + .5f - pcy) / pry;
            if (nx * nx + ny * ny > 1) continue;
            const uint8_t col = lc_get(&cv, x, y);
            if (col == LC_BLACK || col == LC_CLEAR) continue;
            lc_pset(&cv, x, y, LC_BR_WHITE);
        }
    }
}

static void draw_fx(void)
{
    const uint32_t t = lap_millis();
    for (int i = 0; i < FX; i++) {
        const fx_t *e = &fx[i];
        if (e->until <= t) continue;
        const int x = (int)(e->x * cv.w), y = (int)(e->y * cv.h);
        switch (e->kind) {
        case 0: lc_pset(&cv, x, y, LC_BR_WHITE); lc_pset(&cv, x + 1, y, LC_CYAN); lc_pset(&cv, x + 2, y, LC_BLUE); break;   /* a packet and its trail */
        case 1: lc_pset(&cv, x, y, LC_BR_RED); lc_pset(&cv, x - 1, y - 1, LC_RED); lc_pset(&cv, x + 1, y - 1, LC_RED); break; /* a small heart */
        case 2: lc_text(&cv, x, y, "Z", 1, LC_GREY); break;
        default: lc_pset(&cv, x, y, (t / 80 + i) % 2 ? LC_BR_YELLOW : LC_WHITE); break;
        }
    }
}

static void move_fx(float dt)
{
    const uint32_t t = lap_millis();
    const float mx = px_pet, my = (pcy + pry * 0.35f) / (cv.h ? cv.h : 1);   /* the grille */
    for (int i = 0; i < FX; i++) {
        fx_t *e = &fx[i];
        if (e->until <= t) continue;
        if (e->kind == 0) {
            const float dx = mx - e->x, dy = my - e->y, d = sqrtf(dx * dx + dy * dy);
            if (d < 0.03f) { e->until = t; chomp_until = t + 500; continue; }
            e->x += dx / d * 0.7f * dt; e->y += dy / d * 0.7f * dt;
        } else {
            e->x += e->vx * dt; e->y += e->vy * dt;
            if (e->kind == 3) e->vy += 0.8f * dt;
        }
    }
}

/* Words over the room, in the board's font: the toast on the frame's top
   edge, a speech bubble in the room's top rows. */
static void draw_words(void)
{
    const uint32_t t = lap_millis();
    if (t < toast_until) {
        const int n = (int)strlen(toast), x = room.x + (room.w - n - 2) / 2;
        lap_put_str(x, room.y, " ", LAP_ATTR(LAP_BLACK, LAP_YELLOW | LAP_BRIGHT));
        lap_put_str(x + 1, room.y, toast, LAP_ATTR(LAP_BLACK, LAP_YELLOW | LAP_BRIGHT));
        lap_put_str(x + 1 + n, room.y, " ", LAP_ATTR(LAP_BLACK, LAP_YELLOW | LAP_BRIGHT));
    }
    if (t < bubble_until && bubble[0]) {
        const int w = room.w - 6;
        char line[3][64];
        int n = 0;
        const char *p = bubble;
        while (*p && n < 3) {
            int len = (int)strlen(p) < w ? (int)strlen(p) : w;
            if (p[len]) { int k = len; while (k > 0 && p[k] != ' ') k--; if (k > 0) len = k; }
            snprintf(line[n++], sizeof(line[0]), "%.*s", len, p);
            p += len;
            while (*p == ' ') p++;
        }
        const uint8_t at = LAP_ATTR(LAP_BLACK, LAP_WHITE | LAP_BRIGHT);
        for (int i = 0; i < n; i++) {
            lap_fill(room.x + 2, room.y + 2 + i, w + 2, 1, ' ', at);
            lap_put_str(room.x + 3, room.y + 2 + i, line[i], at);
        }
    }
}

/* ---- the dex, as a list --------------------------------------------------------- */

static void draw_dex(void)
{
    char t[80];
    int y = room.y + 1;
    const int x = room.x + 2, last = room.y + room.h - 2;
    snprintf(t, sizeof(t), "Met %u nodes, %u networks, %u Bluetooth devices", pet.core.n_nodes, pet.core.n_nets, pet.core.n_ble);
    lap_put_str(x, y++, t, A(LAP_WHITE | LAP_BRIGHT));
    snprintf(t, sizeof(t), "Ate %lu packets, read %lu messages", (unsigned long)pet.core.packets, (unsigned long)pet.core.messages);
    lap_put_str(x, y++, t, A(LAP_WHITE));
    y++;
    lap_put_str(x, y++, "FORMS REACHED", A(LAP_WHITE | LAP_BRIGHT));
    for (int i = 0; i < FORM_COUNT && y < last - 2; i++) {
        snprintf(t, sizeof(t), "%c %-10s %s", i == active_form ? '>' : ' ',
                 pet.forms & (1u << i) ? FORM_NAME[i] : "----------",
                 i == active_form ? "now" : pet.forms & (1u << i) ? "logged" : "unseen");
        lap_put_str(x, y++, t, A(i == active_form ? LAP_CYAN | LAP_BRIGHT : LAP_WHITE));
    }
    y++;
    lap_put_str(x, y++, mesh_up ? "HEARD ON THE MESH" : "THE MESH IS OFF: START IT IN MESH", A(mesh_up ? LAP_GREEN | LAP_BRIGHT : LAP_BLACK | LAP_BRIGHT));
    for (int i = 0; i < mesh_nodes && y < last - 4; i++, y++) {
        snprintf(t, sizeof(t), "%-16.16s %5.0f dBm %5lus", nodes[i].name[0] ? nodes[i].name : nodes[i].id, (double)nodes[i].rssi, (unsigned long)nodes[i].heard_s);
        lap_put_str(x, y, t, A(LAP_CYAN));
        const int bars = nodes[i].rssi > -70 ? 4 : nodes[i].rssi > -85 ? 3 : nodes[i].rssi > -100 ? 2 : 1;
        for (int b = 0; b < 4; b++) lap_put_cell(x + 36 + b, y, b < bars ? LAP_BLOCK : LAP_SHADE_25, A(b < bars ? LAP_GREEN | LAP_BRIGHT : LAP_BLACK | LAP_BRIGHT));
    }
    y++;
    if (y < last - 1) {
        snprintf(t, sizeof(t), "WI-FI, LAST SNIFF: %d", n_aps);
        lap_put_str(x, y++, t, A(LAP_GREEN | LAP_BRIGHT));
        for (int i = 0; i < n_aps && y < last; i++, y++) {
            snprintf(t, sizeof(t), "%-24.24s %4d  ch %d", aps[i].ssid[0] ? aps[i].ssid : "(hidden)", aps[i].rssi, aps[i].channel);
            lap_put_str(x, y, t, A(LAP_GREEN));
        }
    }
}

/* ---- buttons, the board's way ------------------------------------------------- */

static void button(box_t b, const char *label, const char *hint, uint8_t col, bool lit, bool dim)
{
    const uint8_t frame = A(dim ? LAP_BLACK | LAP_BRIGHT : col);
    if (lit) lap_fill(b.x, b.y, b.w, b.h, ' ', LAP_ATTR(LAP_BLACK, col));
    lap_box(b.x, b.y, b.w, b.h, NULL, lit ? LAP_ATTR(LAP_BLACK, col) : frame);
    const int ly = b.y + (b.h - 1) / 2 - (hint && b.h >= 4 ? 1 : 0);
    lap_put_str(b.x + (b.w - (int)strlen(label)) / 2, ly, label, lit ? LAP_ATTR(LAP_BLACK, col) : A(dim ? LAP_BLACK | LAP_BRIGHT : LAP_WHITE | LAP_BRIGHT));
    if (hint && b.h >= 4) lap_put_str(b.x + (b.w - (int)strlen(hint)) / 2, ly + 1, hint, lit ? LAP_ATTR(LAP_BLACK, col) : A(LAP_BLACK | LAP_BRIGHT));
}

/* ---- the app ------------------------------------------------------------- */

static void enter(void)
{
    lap_fullscreen(true);
    active_form = -1; evolve_at = 0;
    load();
    lap_time_t tm;
    lap_time(&tm);
    clock_ok = tm.synced && tm.unix > 1600000000;
    now_unix = tm.unix;
    if (clock_ok && pet.core.last > 1600000000 && now_unix > pet.core.last) {
        /* Time passes while the app is closed, up to a day of it. */
        int64_t gone = now_unix - pet.core.last;
        if (gone > 86400) gone = 86400;
        live((int32_t)gone / 60.0f);
    }
    if (clock_ok && !pet.core.born) pet.core.born = now_unix;
    evolve();
    lap_ble_scan(true);
    last_ms = tick_ms = lap_millis();
    born_ms = last_ms;
    blink_at = 0;
}

static void leave(void)
{
    lap_time_t tm;
    lap_time(&tm);
    if (tm.synced) pet.core.last = tm.unix;
    save();
    lap_canvas_free(&cv);
}

static void draw(void)
{
    if (!cv.d || cv.cw != room.w - 2 || room.y + room.h > lap_rows() || b_dex.x + b_dex.w > lap_cols()) layout();
    lap_clear(A(LAP_WHITE));
    if (!cv.d) return;
    const uint32_t t = lap_millis();
    float dt = (t - last_ms) / 1000.0f;
    last_ms = t;
    if (dt > 0.1f) dt = 0.1f;

    lap_time_t tm;
    lap_time(&tm);
    clock_ok = tm.synced && tm.unix > 1600000000;
    hour = tm.hour; minute = tm.minute;

    if (t - tick_ms >= 1000) {
        const float minutes = (t - tick_ms) / 60000.0f;
        tick_ms = t;
        const uint32_t before = last_rx;
        listen();
        if (last_rx != before) radio_ms = t;
        live(minutes);
        if (stage() == EGG && (t - born_ms) % 15000 < 1000) gain(1);   /* an egg warms up just by being looked after */
        if (asleep) spawn(2, px_pet + 0.08f, (float)pet_y0 / cv.h, 0.02f, -0.05f, 2500);
    }
    if (!asleep && stage() != EGG) {
        if (fabsf(target_x - px_pet) < 0.01f && (lap_random() % 200) == 0) target_x = 0.18f + lc_frand() * 0.55f;
        const float d = target_x - px_pet;
        if (fabsf(d) > 0.005f) { px_pet += (d > 0 ? 1 : -1) * fminf(fabsf(d), 0.10f * dt); facing_left = d < 0; bob += dt * 8; }
    }
    evolve();
    move_fx(dt);

    draw_hud();
    lap_box(room.x, room.y, room.w, room.h, page ? " DEX " : NULL, A(LAP_BLACK | LAP_BRIGHT));
    if (page == 0) {
        draw_sky();
        draw_tower();
        draw_pet();
        draw_evolution();
        draw_fx();
        lap_canvas_show(&cv, room.x + 1, room.y + 1);
        draw_words();
    } else
        draw_dex();
    button(b_ping, "PING", tx_armed ? "advert" : "tx off", LAP_BLUE | LAP_BRIGHT, t < ping_until, !tx_armed);
    button(b_sniff, "SNIFF", "Wi-Fi", LAP_GREEN | LAP_BRIGHT, sniff_until && (t / 250) % 2, false);
    button(b_dex, page ? "BACK" : "DEX", page ? "the room" : "collection", LAP_WHITE, false, false);

    /* Stroking it: a finger dragged across the pet. */
    lap_touch_t tc;
    static int last_tx = -1;
    if (page == 0 && lap_touch(&tc) && tc.down) {
        int cw, ch;
        lap_cell_px(&cw, &ch);
        const int dx = tc.x * 2 / cw - 2 * (room.x + 1), dy = tc.y * 3 / ch - 3 * (room.y + 1);
        if (dx >= pet_x0 && dx < pet_x0 + pet_w && dy >= pet_y0 && dy < pet_y0 + pet_h) {
            if (last_tx >= 0 && abs(tc.x - last_tx) > cw) {
                hearts_until = t + 1200;
                cheer(1.5f);
                pet.core.pets++;
                if (stage() == EGG) gain(2);
                if ((pet.core.pets & 3) == 0) {
                    spawn(1, px_pet + (lc_frand() - .5f) * 0.2f, (float)pet_y0 / cv.h, (lc_frand() - .5f) * 0.05f, -0.12f, 1300);
                    lap_tone(1568 + (int)(lap_random() % 300), 30, 25, LAP_WAVE_TRIANGLE);
                }
                if (asleep) { asleep = false; wake_until = t + 120000; say("HUH? I'M UP!", 1500); }
            }
            last_tx = tc.x;
        }
    } else last_tx = -1;
}

static bool in(box_t b, int x, int y) { return x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h; }

static bool touch(int col, int row)
{
    const uint32_t t = lap_millis();
    if (in(b_ping, col, row)) {
        /* A shout onto the mesh: our advert. Other nodes may answer. */
        if (!mesh_up) say("THE MESH IS OFF", 1800);
        else if (!tx_armed) say("TURN ON TRANSMIT IN MESH", 2200);
        else if (lap_mesh_advert()) {
            ping_until = t + 600;
            say("PING!", 1200);
            lap_tone(0, 1, 0, LAP_WAVE_SINE);
            lap_tone(880, 50, 40, LAP_WAVE_SQUARE);
            lap_tone(1760, 80, 40, LAP_WAVE_SQUARE);
            radio_ms = t;
            gain(1);
        }
        return true;
    }
    if (in(b_sniff, col, row)) {
        if (lap_wifi_scan()) { sniff_until = t + 8000; say("SNIFFING THE AIR...", 2500); }
        else say("WI-FI IS BUSY", 1500);
        lap_tone(600, 40, 25, LAP_WAVE_NOISE);
        return true;
    }
    if (in(b_dex, col, row)) { page = !page; lap_tone(page ? 990 : 660, 30, 30, LAP_WAVE_SQUARE); return true; }
    const int x = (col - room.x - 1) * 2 + 1, y = (row - room.y - 1) * 3 + 1;
    if (page == 0 && x >= pet_x0 && x < pet_x0 + pet_w && y >= pet_y0 && y < pet_y0 + pet_h) {
        blink_at = t + 200;
        hearts_until = t + 600;
        cheer(0.5f);
        lap_tone(1200, 25, 25, LAP_WAVE_TRIANGLE);
        if (asleep) { asleep = false; wake_until = t + 120000; say("HUH? I'M UP!", 1500); }
        return true;
    }
    if (page == 0 && in(room, col, row) && !asleep && stage() != EGG) { target_x = (float)x / cv.w; return true; }
    return true;
}

static bool key(lap_key_t k, char ch)
{
    (void)ch;
    if (k == LAP_KEY_TAB || k == LAP_KEY_RIGHT || k == LAP_KEY_LEFT) { page = !page; return true; }
    return false;
}

#ifndef __riscv
/* Simulator only: LAPSIM_SCRIPT="0:pet=relay;40:pet=swarm;80:pet=sleep".
 * Selecting a form seeds its real diet/health inputs; evolve() still chooses it. */
void meshling_preview(const char *command)
{
    for (int i = 0; i < FORM_COUNT; i++) {
        char lower[16];
        int j = 0;
        for (; FORM_NAME[i][j]; j++) lower[j] = (char)(FORM_NAME[i][j] + ('a' - 'A'));
        lower[j] = 0;
        if (strcmp(command, lower)) continue;
        pet.core.xp = i < RELAY ? STAGE_XP[i] : 1500;
        pet.core.food = pet.core.joy = 80; pet.core.energy = 80;
        pet.diet[0] = pet.diet[1] = pet.diet[2] = 1000;
        if (i >= RELAY && i <= SCANNER) pet.diet[i - RELAY] = 10000;
        if (i == DRIFTER) pet.core.food = 10;
        if (i == RELIC) pet.core.food = pet.core.joy = 0;
        asleep = false; px_pet = target_x = .5f;
        chomp_until = hearts_until = blink_at = 0;
        return;
    }
    if (!strcmp(command, "sleep")) { asleep = true; pet.core.energy = 30; }
    if (!strcmp(command, "happy")) hearts_until = lap_millis() + 20000;
    if (!strcmp(command, "hungry")) pet.core.food = fminf(pet.core.food, 24);
    if (!strcmp(command, "chomp")) chomp_until = lap_millis() + 20000;
    if (!strcmp(command, "dex")) page = 1;
    if (!strcmp(command, "room")) page = 0;
    if (!strcmp(command, "save")) save();
}
#endif

/* The tile icon, in colour dots. */
LAP_ICON_PX(
    "bb................bb",
    "bb................bb",
    "..8..............8..",
    "...8............8...",
    "....8....7f....8....",
    ".....87fff7f778.....",
    ".....ffffffff77.....",
    "....7fffffff7777....",
    "....fffffffff7f7....",
    "....ff00ff7f0077....",
    "...7ffe0ffffe0f77...",
    "...f7f007f7f00777...",
    "....fffffff7f777....",
    "....7f7f7f7f7777....",
    "....f7fff7f77777....",
    ".....77f0000777.....",
    "......77f77777......",
    ".........77.........");

LAP_APP(.id = "meshling", .name = "MESHLING", .sub = "radio pet", .version = "1.2",
        .author = "LakeShark", .color = LAP_CYAN,
        .icon_art = { "  o    o  ", "   \\  /   ", " .------. ", " | o  o | ", " |  \\/  | ", " '------' " },
        .enter = enter, .leave = leave, .draw = draw, .key = key, .touch = touch);
