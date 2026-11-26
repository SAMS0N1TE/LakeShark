/* lap_ids (app-sdk/lib): the shared Bluetooth name file, the owner's names
   and remembered device names, on a fake card. */
#include "ls_test.h"
#include "lap_ids.h"
#include "lakeshark_app.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- a fake card: files in memory, each job done when polled ---- */
typedef struct { char path[96]; uint8_t data[8192]; uint32_t len; bool there; } ffile_t;
static ffile_t files[6];
typedef struct { int kind; char path[96]; uint32_t off, len; void *buf; uint8_t wdata[256]; bool used; } fjob_t;
static fjob_t jobs[8];
static int writes_seen;

static ffile_t *ff(const char *path, bool make)
{
    for (int i = 0; i < 6; i++) if (files[i].there && !strcmp(files[i].path, path)) return &files[i];
    if (!make) return NULL;
    for (int i = 0; i < 6; i++)
        if (!files[i].there) { memset(&files[i], 0, sizeof(files[i])); snprintf(files[i].path, 96, "%s", path); files[i].there = true; return &files[i]; }
    return NULL;
}
static void put_file(const char *path, const void *d, uint32_t n)
{
    ffile_t *f = ff(path, true);
    memcpy(f->data, d, n);
    f->len = n;
}
static int new_job(int kind, const char *path, uint32_t off, void *buf, uint32_t len)
{
    for (int i = 0; i < 8; i++)
        if (!jobs[i].used) {
            jobs[i] = (fjob_t){.kind = kind, .off = off, .len = len, .buf = buf, .used = true};
            snprintf(jobs[i].path, 96, "%s", path);
            return i;
        }
    return LAP_SD_EBUSY;
}
int lap_sd_read(const char *path, uint32_t off, void *buf, uint32_t len) { return new_job(0, path, off, buf, len); }
int lap_sd_size(const char *path) { return new_job(1, path, 0, NULL, 0); }
int lap_sd_write(const char *path, uint32_t off, const void *data, uint32_t len)
{
    const int j = new_job(2, path, off, NULL, len);
    if (j >= 0) memcpy(jobs[j].wdata, data, len < 256 ? len : 256);
    return j;
}
int lap_sd_poll(int j)
{
    fjob_t *b = &jobs[j];
    b->used = false;
    ffile_t *f = ff(b->path, b->kind == 2);
    if (!f) return LAP_SD_ENOENT;
    if (b->kind == 1) return (int)f->len;
    if (b->kind == 2) {
        writes_seen++;
        const uint32_t at = b->off == LAP_SD_APPEND ? f->len : 0;
        memcpy(f->data + at, b->wdata, b->len);
        f->len = at + b->len;
        return (int)b->len;
    }
    if (b->off >= f->len) return 0;
    const uint32_t n = f->len - b->off < b->len ? f->len - b->off : b->len;
    memcpy(b->buf, f->data + b->off, n);
    return (int)n;
}

/* ---- a small name file, built the way build_ble_ids.py builds one ---- */
typedef struct { uint32_t key; const char *name; } ent_t;
static uint8_t img[4096];
static uint32_t build_img(void)
{
    uint8_t nus[16], printer[16];
    lap_ids_parse_uuid("6e400001-b5a3-f393-e0a9-e50e24dcca9e", nus, NULL);
    lap_ids_parse_uuid("49535343-fe7d-4ae5-8fa9-9fafd205e455", printer, NULL);
    uint32_t a = lap_ids_uuid_key(nus), b = lap_ids_uuid_key(printer);
    ent_t u128[2] = {{a < b ? a : b, a < b ? "Nordic UART" : "Microchip UART"}, {a < b ? b : a, a < b ? "Microchip UART" : "Nordic UART"}};
    struct { uint8_t kind, keyb; int n; ent_t e[3]; } t[4] = {
        {LAP_IDS_SERVICE, 2, 2, {{0x180F, "Battery"}, {0xFEE7, "Tencent Holdings"}}},
        {LAP_IDS_CHAR, 2, 1, {{0x2A19, "Battery Level"}}},
        {LAP_IDS_OUI, 3, 1, {{0xE06E41, "Shenzhen Printer"}}},
        {LAP_IDS_UUID128, 4, 2, {u128[0], u128[1]}},
    };
    memset(img, 0, sizeof(img));
    uint32_t off = 32 + 16 * 4, idx[4], rec[4];
    for (int i = 0; i < 4; i++) { idx[i] = off; off += t[i].keyb; }          /* one index key each (every 64) */
    for (int i = 0; i < 4; i++) { rec[i] = off; off += (uint32_t)t[i].n * (t[i].keyb + 3u); }
    const uint32_t pool = off;
    uint32_t pl = 0;
    for (int i = 0; i < 4; i++) {
        memcpy(img + 32 + 16 * i, (uint8_t[]){t[i].kind, t[i].keyb, 64, 0}, 4);
        memcpy(img + 32 + 16 * i + 4, &(uint32_t){(uint32_t)t[i].n}, 4);
        memcpy(img + 32 + 16 * i + 8, &rec[i], 4);
        memcpy(img + 32 + 16 * i + 12, &idx[i], 4);
        memcpy(img + idx[i], &t[i].e[0].key, t[i].keyb);
        for (int k = 0; k < t[i].n; k++) {
            uint8_t *r = img + rec[i] + k * (t[i].keyb + 3u);
            memcpy(r, &t[i].e[k].key, t[i].keyb);
            memcpy(r + t[i].keyb, &pl, 3);
            strcpy((char *)img + pool + pl, t[i].e[k].name);
            pl += (uint32_t)strlen(t[i].e[k].name) + 1;
        }
    }
    memcpy(img, "BHID", 4);
    memcpy(img + 4, (uint16_t[]){1, 4}, 4);
    memcpy(img + 8, &pool, 4);
    memcpy(img + 12, &pl, 4);
    return pool + pl;
}

static void pump(int n) { for (int i = 0; i < n; i++) lap_ids_pump(); }
static const char *get_now(int kind, uint32_t key)
{
    const char *n = lap_ids_get(kind, key);
    for (int i = 0; !n && i < 20; i++) { pump(1); n = lap_ids_get(kind, key); }
    return n;
}

LS_CASE(lap_ids_uuids)
{
    uint8_t be[16];
    int bits;
    LS_CHECK(lap_ids_parse_uuid("180f", be, &bits) && bits == 16 && lap_ids_uuid_short(be) == 0x180F);
    LS_CHECK(lap_ids_parse_uuid("0x2A19", be, &bits) && bits == 16);
    LS_CHECK(lap_ids_parse_uuid("0000180f-0000-1000-8000-00805f9b34fb", be, &bits) && bits == 16 && lap_ids_uuid_short(be) == 0x180F);
    LS_CHECK(lap_ids_parse_uuid("6E400001-B5A3-F393-E0A9-E50E24DCCA9E", be, &bits) && bits == 128);
    char t[40];
    lap_ids_uuid_text(be, t, sizeof(t));
    LS_EQ_STR(t, "6e400001-b5a3-f393-e0a9-e50e24dcca9e");
    LS_EQ_UINT(lap_ids_uuid_key(be), 0x17dfdc52u);       /* the same as build_ble_ids.py's fnv() */
    LS_CHECK(!lap_ids_parse_uuid("18f", be, &bits));
    LS_CHECK(!lap_ids_parse_uuid("xyz1", be, &bits));
}

LS_CASE(lap_ids_lines)
{
    lap_ids_user_t u;
    LS_EQ_INT(lap_ids_parse_line("# a comment", 11, &u), 0);
    LS_EQ_INT(lap_ids_parse_line("   ", 3, &u), 0);
    const char *l = "service 18f0 Thermal printer   # mine";
    LS_EQ_INT(lap_ids_parse_line(l, strlen(l), &u), 1);
    LS_EQ_INT(u.kind, LAP_IDS_SERVICE); LS_EQ_UINT(u.key, 0x18F0u); LS_EQ_STR(u.name, "Thermal printer");
    l = "char 49535343-aca3-481c-91ec-d85e28a60318 UART control";
    LS_EQ_INT(lap_ids_parse_line(l, strlen(l), &u), 1);
    LS_EQ_INT(u.kind, LAP_IDS_UUID128);
    l = "company 0x004c Apple";
    LS_EQ_INT(lap_ids_parse_line(l, strlen(l), &u), 1); LS_EQ_UINT(u.key, 0x4Cu);
    l = "company 76 Apple";
    LS_EQ_INT(lap_ids_parse_line(l, strlen(l), &u), 1); LS_EQ_UINT(u.key, 76u);
    l = "maker E0-6E-41 Printer maker";
    LS_EQ_INT(lap_ids_parse_line(l, strlen(l), &u), 1); LS_EQ_UINT(u.key, 0xE06E41u);
    l = "device e0:6e:41:00:10:62 Kitchen printer";
    LS_EQ_INT(lap_ids_parse_line(l, strlen(l), &u), 1);
    LS_EQ_INT(u.kind, LAP_IDS_DEVICE); LS_EQ_UINT(u.mac[0], 0xE0u); LS_EQ_UINT(u.mac[5], 0x62u);
    l = "service 18f0";
    LS_EQ_INT(lap_ids_parse_line(l, strlen(l), &u), -1);              /* no name */
    l = "gadget 18f0 x";
    LS_EQ_INT(lap_ids_parse_line(l, strlen(l), &u), -1);
    l = "device e0:6e:41 short";
    LS_EQ_INT(lap_ids_parse_line(l, strlen(l), &u), -1);
}

LS_CASE(lap_ids_engine)
{
    memset(files, 0, sizeof(files)); memset(jobs, 0, sizeof(jobs)); writes_seen = 0;
    put_file(LAP_IDS_FILE1, img, build_img());
    const char *user = "service 180f My battery\nchar 49535343-1e4d-4bd9-ba61-23c647249616 Printer TX\n"
                       "device e0:6e:41:00:10:62 Kitchen printer\nnonsense line here\n";
    put_file(LAP_IDS_USER_FILE, user, (uint32_t)strlen(user));
    const char *devs = "11:22:33:44:55:66 Old name\n11:22:33:44:55:66 Watch\nc0:00:00:00:00:01 Static random\n";
    put_file(LAP_IDS_DEVICE_FILE, devs, (uint32_t)strlen(devs));
    lap_ids_open();
    pump(30);
    char st[96];
    LS_EQ_INT(lap_ids_state(st, sizeof(st)), 2);
    LS_EQ_INT(lap_ids_user_count(), 2);
    /* the owner's name wins over the file */
    LS_EQ_STR(lap_ids_get(LAP_IDS_SERVICE, 0x180F), "My battery");
    LS_EQ_STR(get_now(LAP_IDS_SERVICE, 0xFEE7), "Tencent Holdings");
    LS_CHECK(get_now(LAP_IDS_SERVICE, 0x1234) == NULL);
    /* a characteristic missing from its table falls back to the service table */
    uint8_t be[16];
    lap_ids_uuid16_be(0xFEE7, be);
    const char *n = lap_ids_attr(LAP_IDS_CHAR, be);
    for (int i = 0; !n && i < 20; i++) { pump(1); n = lap_ids_attr(LAP_IDS_CHAR, be); }
    LS_EQ_STR(n, "Tencent Holdings");
    lap_ids_parse_uuid("6e400001-b5a3-f393-e0a9-e50e24dcca9e", be, NULL);
    n = lap_ids_attr(LAP_IDS_SERVICE, be);
    for (int i = 0; !n && i < 20; i++) { pump(1); n = lap_ids_attr(LAP_IDS_SERVICE, be); }
    LS_EQ_STR(n, "Nordic UART");
    lap_ids_parse_uuid("49535343-1e4d-4bd9-ba61-23c647249616", be, NULL);
    LS_EQ_STR(lap_ids_attr(LAP_IDS_CHAR, be), "Printer TX");
    /* makers: public only */
    const uint8_t pub[6] = {0xE0, 0x6E, 0x41, 0, 0x10, 0x62}, rnd[6] = {0xE2, 0x6E, 0x41, 0, 0, 1};
    n = lap_ids_maker(pub);
    for (int i = 0; !n && i < 20; i++) { pump(1); n = lap_ids_maker(pub); }
    LS_EQ_STR(n, "Shenzhen Printer");
    LS_CHECK(lap_ids_maker(rnd) == NULL);
    /* devices: the radio's little-endian order; later lines win; owner names win */
    const uint8_t printer_le[6] = {0x62, 0x10, 0x00, 0x41, 0x6E, 0xE0};
    const uint8_t watch_le[6] = {0x66, 0x55, 0x44, 0x33, 0x22, 0x11};
    const uint8_t static_le[6] = {0x01, 0, 0, 0, 0, 0xC0}, rpa_le[6] = {0x01, 0, 0, 0, 0, 0x40};
    LS_EQ_STR(lap_ids_device(printer_le, 0), "Kitchen printer");
    LS_EQ_STR(lap_ids_device(watch_le, 0), "Watch");
    LS_EQ_STR(lap_ids_device(static_le, 1), "Static random");
    LS_EQ_INT(lap_ids_device_learn(printer_le, 0, "RP425-1062-BLE"), 0);   /* the owner's name stays */
    LS_EQ_STR(lap_ids_device(printer_le, 0), "Kitchen printer");
    LS_EQ_INT(lap_ids_device_learn(watch_le, 0, "Watch"), 0);              /* same: nothing written */
    LS_EQ_INT(lap_ids_device_learn(rpa_le, 1, "Phone"), 0);                /* resolvable private: never kept */
    LS_CHECK(lap_ids_device(rpa_le, 1) == NULL);
    const uint8_t new_le[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x00};
    LS_EQ_INT(lap_ids_device_learn(new_le, 0, "  Heart strap  "), 1);
    LS_EQ_STR(lap_ids_device(new_le, 0), "Heart strap");
    pump(5);
    const ffile_t *df = ff(LAP_IDS_DEVICE_FILE, false);
    LS_CHECK(df && strstr((const char *)df->data, "00:ee:dd:cc:bb:aa Heart strap\n") != NULL);
    /* naming something: held now, written as a line */
    LS_EQ_INT(lap_ids_user_add("char", "2af1", "Printer data"), 0);
    LS_EQ_STR(lap_ids_get(LAP_IDS_CHAR, 0x2AF1), "Printer data");
    LS_EQ_INT(lap_ids_user_add("device", "AA-BB-CC-DD-EE-01", "Desk lamp"), 0);
    const uint8_t lamp_le[6] = {0x01, 0xEE, 0xDD, 0xCC, 0xBB, 0xAA};
    LS_EQ_STR(lap_ids_device(lamp_le, 0), "Desk lamp");
    LS_EQ_INT(lap_ids_user_add("gizmo", "1", "x"), -1);
    pump(5);
    const ffile_t *uf = ff(LAP_IDS_USER_FILE, false);
    LS_CHECK(strstr((const char *)uf->data, "char 2af1 Printer data\n") != NULL);
    LS_CHECK(strstr((const char *)uf->data, "device aa:bb:cc:dd:ee:01 Desk lamp\n") != NULL);
    /* a fresh open reads it all back */
    lap_ids_close();
    lap_ids_open();
    pump(30);
    LS_EQ_STR(lap_ids_get(LAP_IDS_CHAR, 0x2AF1), "Printer data");
    LS_EQ_STR(lap_ids_device(new_le, 0), "Heart strap");
    LS_EQ_STR(lap_ids_device(lamp_le, 0), "Desk lamp");
    lap_ids_close();
}

LS_CASE(lap_ids_no_file)
{
    memset(files, 0, sizeof(files)); memset(jobs, 0, sizeof(jobs));
    lap_ids_open();
    pump(30);
    char st[96];
    LS_EQ_INT(lap_ids_state(st, sizeof(st)), 0);
    LS_CHECK(lap_ids_get(LAP_IDS_SERVICE, 0x180F) == NULL);
    LS_EQ_INT(lap_ids_user_add("service", "18f0", "Printer"), 0);           /* names still work without the file */
    LS_EQ_STR(lap_ids_get(LAP_IDS_SERVICE, 0x18F0), "Printer");
    lap_ids_close();
}
