/* WARDRIVE signatures (wd_sig.h): the glob, the built-in rows, the card
   file's rows and what a heard device matches. */
#include "wardrive/wd_core.h"
#include "wardrive/wd_feed.h"
#include "wardrive/wd_sig.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static char dir[256];
void wd_feed_start(const bool src[WD_KINDS], int pace) { (void)src; (void)pace; }
void wd_feed_stop(void) {}
void wd_feed_pace(int pace) { (void)pace; }
bool wd_feed_has(int kind) { (void)kind; return true; }
bool wd_feed_fix(wd_fix_t *out, uint32_t now) { (void)now; memset(out, 0, sizeof(*out)); return false; }
int wd_feed_poll(wd_obs_t *out, int cap, uint32_t now, unsigned *scans) { (void)out; (void)cap; (void)now; (void)scans; return 0; }
bool wd_feed_spawn(void) { return false; }
bool wd_feed_worker_alive(void) { return false; }
void wd_lock(void) {}
void wd_unlock(void) {}
void *wd_feed_alloc(size_t bytes) { return calloc(1, bytes); }
void wd_feed_free(void *p) { free(p); }
bool wd_feed_card(void) { return true; }
const char *wd_feed_dir(void) { return dir; }
const char *wd_feed_board(void) { return "bench"; }
bool wd_feed_upload(const char *path) { (void)path; return false; }
bool wd_feed_hub(char *out, size_t cap, bool *busy) { (void)out; (void)cap; (void)busy; return false; }

static wd_obs_t obs(int kind, const char *mac, const char *name)
{
    wd_obs_t o;
    memset(&o, 0, sizeof(o));
    o.kind = (uint8_t)kind;
    for (int i = 0; i < 6; i++) { unsigned v; sscanf(mac + i * 3, "%2x", &v); o.addr[i] = (uint8_t)v; }
    snprintf(o.name, sizeof(o.name), "%s", name);
    return o;
}

static const char *id_of(uint16_t s) { return s ? wd_sig_at(s - 1)->id : ""; }

static uint16_t m(int kind, const char *mac, const char *name, uint16_t *tags)
{
    wd_obs_t o = obs(kind, mac, name);
    *tags = 0;
    return wd_sig_match(&o, tags);
}

static uint16_t mb(const char *mac, uint16_t co, int mfg0, uint16_t uuid, int svc0, uint16_t *tags)
{
    wd_obs_t o = obs(WD_BLE, mac, "");
    const wd_sig_ble_t b = { .company = co, .mfg0 = (int16_t)mfg0, .uuid16 = uuid, .svc0 = (int16_t)svc0 };
    *tags = 0;
    return wd_sig_match_ble(&o, &b, tags);
}

/* Every row line of the card file, in order, must be a built-in row. */
static void file_equals_builtin(void)
{
    FILE *f = fopen(WD_SIGS_TXT, "rb");
    assert(f);
    static char text[65536];
    const size_t n = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    text[n] = 0;
    static char rows[65536];
    size_t used = 0;
    for (char *p = strtok(text, "\n"); p; p = strtok(NULL, "\n")) {
        size_t l = strlen(p);
        while (l && (p[l - 1] == '\r' || p[l - 1] == ' ')) p[--l] = 0;
        if (!l || p[0] == '#') continue;
        memcpy(rows + used, p, l);
        used += l;
        rows[used++] = '\n';
    }
    rows[used] = 0;
    if (strcmp(rows, wd_sig_builtin())) {
        fprintf(stderr, "data/wardrive/sigs.txt and wd_sig.c's built-in rows differ\n");
        assert(0);
    }
}

int main(void)
{
    /* the glob */
    assert(wd_sig_glob("Flock-######", "Flock-0A1b2C"));
    assert(wd_sig_glob("Flock-######", "FLOCK-0A1B2C"));
    assert(!wd_sig_glob("Flock-######", "Flock-0A1B2"));
    assert(!wd_sig_glob("Flock-######", "Flock-0A1B2CD"));
    assert(!wd_sig_glob("Flock-######", "Flock-XA1B2C"));
    assert(wd_sig_glob("*bodycam*", "Axon BodyCam 3"));
    assert(wd_sig_glob("Penguin-%%%%", "penguin-1234"));
    assert(!wd_sig_glob("Penguin-%%%%", "penguin-12a4"));
    assert(wd_sig_glob("a\\*b", "a*b") && !wd_sig_glob("a\\*b", "axb"));
    assert(wd_sig_glob("a\\,b", "a,b"));
    assert(wd_sig_glob("*", "") && wd_sig_glob("**x", "abx") && !wd_sig_glob("x", ""));
    assert(wd_sig_glob("Flipper *", "Flipper Zebra") && !wd_sig_glob("Flipper *", "FlipperZebra"));
    assert(wd_sig_glob("S################C", "S0123456789abcdefC"));
    assert(!wd_sig_glob("S################C", "Samsung Galaxy S2C"));
    assert(wd_sig_tag("ALPR", 4) == WD_T_ALPR && wd_sig_tag("GLASSES", 7) == WD_T_WEARABLE && !wd_sig_tag("ALP", 3));

    /* the built-ins: every row loads */
    const int n = wd_sig_load(NULL, 0);
    printf("built-in signatures: %d (%s)\n", n, wd_sig_note());
    assert(n >= 35 && n == wd_sig_count());
    int per[16] = {0};
    for (int i = 0; i < n; i++) {
        const wd_sig_info_t *s = wd_sig_at(i);
        assert(s->id[0] && s->label[0] && s->source[0] && s->tags && s->kinds);
        assert(strlen(s->label) <= 23);
        for (int b = 0; b < 16; b++) if (s->tags & (1u << b)) per[b]++;
        for (int j = 0; j < i; j++) assert(strcmp(s->id, wd_sig_at(j)->id));
    }
    printf("by tag:");
    for (int b = 0; b < 12; b++) if (per[b]) printf(" %s=%d", wd_tag_word((uint16_t)(1u << b)), per[b]);
    printf("\n");
    file_equals_builtin();

    uint16_t t;
    uint16_t s = m(WD_WIFI, "02:11:22:33:44:55", "Flock-5A0B11", &t);
    assert(!strcmp(id_of(s), "flock-cam") && t == WD_T_ALPR);
    s = m(WD_WIFI, "02:11:22:33:44:55", "Flock-NOPE00", &t);
    assert(!s && !t);
    s = m(WD_BLE, "C0:00:00:00:00:01", "Penguin-0123456789", &t);
    assert(!strcmp(id_of(s), "flock-penguin") && t == WD_T_ALPR);
    s = m(WD_WIFI, "B4:1E:52:01:02:03", "", &t);
    assert(!strcmp(id_of(s), "flock-oui"));
    s = m(WD_BLE, "00:25:DF:01:02:03", "", &t);
    assert(!strcmp(id_of(s), "axon") && t == WD_T_BODYCAM);
    s = m(WD_WIFI, "00:30:44:01:02:03", "IBR600-1A2", &t);
    assert(!strcmp(id_of(s), "cradlepoint") && t == WD_T_POLICE);
    s = m(WD_BLE, "D1:22:33:44:55:66", "S1a2b3c4d5e6f7a8bC", &t);
    assert(!strcmp(id_of(s), "tesla-key") && t == WD_T_VEHICLE);
    s = m(WD_BLE, "D1:22:33:44:55:66", "Flipper Kujota", &t);
    assert(!strcmp(id_of(s), "flipper") && t == WD_T_HACKTOOL);
    s = m(WD_WIFI, "D2:22:33:44:55:66", "Flipper Kujota", &t);      /* a Wi-Fi name is not the BLE row */
    assert(!s && !t);
    s = m(WD_WIFI, "DE:AD:BE:EF:DE:AD", "", &t);
    assert(!strcmp(id_of(s), "pwnagotchi"));
    s = m(WD_DRONE, "60:60:1F:22:33:44", "1581F5FJD239C00DW22E", &t);
    assert(s && t == WD_T_DRONE);
    s = m(WD_WIFI, "12:34:56:78:9A:BC", "NETGEAR42", &t);
    assert(!s && !t);
    s = m(WD_WIFI, "18:7F:88:00:00:01", "", &t);
    assert(!strcmp(id_of(s), "ring") && wd_sig_at(s - 1)->alert == 0 && t == WD_T_CAMERA);

    /* the advert's IDs */
    s = mb("40:11:22:33:44:55", 0x004C, 0x12, 0, -1, &t);
    assert(!strcmp(id_of(s), "findmy") && t == WD_T_TRACKER);
    s = mb("40:11:22:33:44:55", 0x004C, 0x10, 0, -1, &t);        /* an iPhone's nearby info is not a tag */
    assert(!s);
    s = mb("40:11:22:33:44:55", 0, -1, 0xFEAA, 0x41, &t);
    assert(!strcmp(id_of(s), "findhub"));
    s = mb("40:11:22:33:44:55", 0, -1, 0xFEAA, 0x10, &t);        /* an Eddystone URL beacon */
    assert(!s);
    s = mb("40:11:22:33:44:55", 0, -1, 0x3300, -1, &t);
    assert(!strcmp(id_of(s), "raven") && t == WD_T_INFRA);
    s = mb("40:11:22:33:44:55", 0x01AB, -1, 0, -1, &t);
    assert(!strcmp(id_of(s), "meta-glasses") && t == WD_T_WEARABLE);
    s = mb("40:11:22:33:44:55", 0x067C, -1, 0, -1, &t);
    assert(!strcmp(id_of(s), "tile"));

    /* the card's file: change one, add two, turn one off, one bad line */
    const char *user =
        "# mine\n"
        "tile|TRACKER|BLE|1|TILE (MINE)|co:067C|me\n"
        "my-van|WATCH+VEHICLE|WIFI|2|THE VAN|mac:A1:B2:C3:D4:E5:F6,name:Van\\,Inc|me\n"
        "ring|off\n"
        "bad|NOPE|WIFI|1|BAD|name:x|me\n"
        "obd-ble|VEHICLE|BLE|0|OBD BLE|name:OBDII|me\n";
    const int nu = wd_sig_load(user, strlen(user));
    printf("with the card: %d (%s)\n", nu, wd_sig_note());
    assert(nu == n + 2);
    assert(strstr(wd_sig_note(), "line 5") && strstr(wd_sig_note(), "NOPE"));
    s = mb("40:11:22:33:44:55", 0x067C, -1, 0, -1, &t);
    assert(!strcmp(wd_sig_at(s - 1)->label, "TILE (MINE)"));
    s = mb("40:11:22:33:44:55", 0, -1, 0xFEED, -1, &t);           /* its old UUIDs went with it */
    assert(!s);
    s = m(WD_WIFI, "A1:B2:C3:D4:E5:F6", "", &t);
    assert(!strcmp(id_of(s), "my-van") && t == (WD_T_WATCH | WD_T_VEHICLE));
    s = m(WD_WIFI, "02:00:00:00:00:01", "van,inc", &t);
    assert(!strcmp(id_of(s), "my-van"));
    s = m(WD_WIFI, "18:7F:88:00:00:01", "", &t);
    assert(!s);

    /* wd_sig_init reads the card, and again only when it changed */
    snprintf(dir, sizeof(dir), "test_wd_sig_dir");
#ifdef _WIN32
    mkdir(dir);
#else
    mkdir(dir, 0755);
#endif
    char p[300];
    snprintf(p, sizeof(p), "%s/sigs.txt", dir);
    FILE *f = fopen(p, "w");
    assert(f);
    fputs("cafe|CAMERA|WIFI|1|CAFE CAM|name:CafeCam*|me\n", f);
    fclose(f);
    wd_sig_init();
    assert(wd_sig_count() == n + 1);
    s = m(WD_WIFI, "02:00:00:00:00:01", "CafeCam 2", &t);
    assert(!strcmp(id_of(s), "cafe"));
    remove(p);
    wd_sig_init();
    assert(wd_sig_count() == n);
    printf("test_wd_sig: ok\n");
    return 0;
}
