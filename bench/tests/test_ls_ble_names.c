/* ls_ble_names (components/apps/tui): the firmware's Bluetooth names. The
   screen calls read a cache and never the card; the worker pass loads the
   files, looks names up, reads changed files again and appends learned
   names, at a limited rate. Real files in the test's folder. */
#include "ls_test.h"
#include "ls_ble_names.h"
#include "lap_ids.h"
#include "esp_timer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IDS  "lsbn_ids.bin"
#define USER "lsbn_names.txt"
#define DEVS "lsbn_devices.txt"

/* ---- a small name file, laid out as build_ble_ids.py lays one out ---- */
typedef struct { uint32_t key; const char *name; } ent_t;
static uint8_t img[4096];
static uint32_t build_img(void)
{
    struct { uint8_t kind, keyb; int n; ent_t e[3]; } t[4] = {
        {LAP_IDS_COMPANY, 2, 2, {{0x004C, "Apple, Inc."}, {0x0075, "Samsung"}}},
        {LAP_IDS_SERVICE, 2, 2, {{0x180F, "Battery"}, {0xFEED, "Tile, Inc."}}},
        {LAP_IDS_OUI, 3, 1, {{0xE06E41, "Shenzhen Printer"}}},
        {LAP_IDS_APPEARANCE, 2, 1, {{0x00C1, "Sports Watch"}}},
    };
    memset(img, 0, sizeof(img));
    uint32_t off = 32 + 16 * 4, idx[4], rec[4];
    for (int i = 0; i < 4; i++) { idx[i] = off; off += t[i].keyb; }
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

static void put(const char *path, const char *mode, const void *d, size_t n)
{
    FILE *f = fopen(path, mode);
    if (f) { fwrite(d, 1, n, f); fclose(f); }
}
static void put_text(const char *path, const char *mode, const char *s) { put(path, mode, s, strlen(s)); }
static int lines_in(const char *path, const char *needle)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    char l[128];
    int n = 0;
    while (fgets(l, sizeof(l), f)) if (!needle || strstr(l, needle)) n++;
    fclose(f);
    return n;
}

static int64_t clock_us;
static void at_ms(uint32_t ms) { clock_us = (int64_t)ms * 1000; ls_shim_time_set(clock_us); }
static void later_ms(uint32_t ms) { at_ms((uint32_t)(clock_us / 1000) + ms); }

/* A clean start: the files as given (NULL: none), the service forgotten. */
static void setup(bool ids, const char *user, const char *devs)
{
    ls_shim_time_live(0);
    at_ms(100000);
    remove(IDS); remove(USER); remove(DEVS);
    if (ids) put(IDS, "wb", img, build_img());
    if (user) put_text(USER, "wb", user);
    if (devs) put_text(DEVS, "wb", devs);
    ls_ble_names_test_paths(IDS, USER, DEVS);
    ls_ble_names_reset();
}

static void done(void) { ls_ble_names_reset(); remove(IDS); remove(USER); remove(DEVS); }

static const uint8_t PRINTER[6] = {0xe0, 0x6e, 0x41, 0x00, 0x10, 0x62};    /* public, the maker in the file */
static const uint8_t STATIC[6] = {0xc0, 0x11, 0x22, 0x33, 0x44, 0x55};     /* static random: top bits 11 */
static const uint8_t RPA[6] = {0x41, 0x22, 0x33, 0x44, 0x55, 0x66};        /* resolvable private: top bits 01 */

LS_CASE(ble_names_cache_off_the_screen)
{
    setup(true, "company 0x0059 My Nordic\ndevice e0:6e:41:00:10:62 Desk printer\n",
          "c0:11:22:33:44:55 Bike lock\n41:22:33:44:55:66 Gone soon\n");
    char n[LS_BLE_NAMES_LEN];
    /* the screen's first ask: nothing yet, nothing read */
    LS_CHECK(!ls_ble_names_company(0x004C, n, sizeof(n)));
    LS_CHECK(!ls_ble_names_device(STATIC, 1, n, sizeof(n)));
    char st[96];
    LS_EQ_INT(ls_ble_names_state(st, sizeof(st)), 1);
    ls_ble_names_work();
    LS_CHECK(ls_ble_names_company(0x004C, n, sizeof(n)));
    LS_EQ_STR(n, "Apple, Inc.");
    LS_CHECK(ls_ble_names_company(0x0059, n, sizeof(n)));                    /* the owner's */
    LS_EQ_STR(n, "My Nordic");
    LS_CHECK(!ls_ble_names_service(0xFEED, n, sizeof(n)));                   /* asked now ... */
    ls_ble_names_work();
    LS_CHECK(ls_ble_names_service(0xFEED, n, sizeof(n)));                    /* ... there the next pass */
    LS_EQ_STR(n, "Tile, Inc.");
    LS_CHECK(!ls_ble_names_appearance(0x00C1, n, sizeof(n)));
    LS_CHECK(!ls_ble_names_maker(PRINTER, n, sizeof(n)));
    ls_ble_names_work();
    LS_CHECK(ls_ble_names_appearance(0x00C1, n, sizeof(n)));
    LS_EQ_STR(n, "Sports Watch");
    LS_CHECK(ls_ble_names_maker(PRINTER, n, sizeof(n)));
    LS_EQ_STR(n, "Shenzhen Printer");
    const uint8_t local[6] = {0xe2, 0x6e, 0x41, 0, 0, 1};
    LS_CHECK(!ls_ble_names_maker(local, n, sizeof(n)));                      /* locally administered */
    /* unknown keys are remembered as unknown: no new card read */
    LS_CHECK(!ls_ble_names_company(0x1234, n, sizeof(n)));
    ls_ble_names_work();
    LS_CHECK(!ls_ble_names_company(0x1234, n, sizeof(n)));
    /* device names: the owner's, remembered ones, never a private address */
    LS_CHECK(ls_ble_names_device(PRINTER, 0, n, sizeof(n)));
    LS_EQ_STR(n, "Desk printer");
    LS_CHECK(ls_ble_names_device(STATIC, 1, n, sizeof(n)));
    LS_EQ_STR(n, "Bike lock");
    LS_CHECK(!ls_ble_names_device(RPA, 1, n, sizeof(n)));
    /* the label: own name first, then company, service, maker */
    LS_EQ_INT(ls_ble_names_label(PRINTER, 0, 0x004C, -1, n, sizeof(n)), LS_BLE_LABEL_NAME);
    LS_EQ_STR(n, "Desk printer");
    const uint8_t anon[6] = {0xe0, 0x6e, 0x41, 9, 9, 9};
    LS_EQ_INT(ls_ble_names_label(anon, 0, 0x004C, 0xFEED, n, sizeof(n)), LS_BLE_LABEL_GUESS);
    LS_EQ_STR(n, "Apple, Inc.");
    LS_EQ_INT(ls_ble_names_label(anon, 0, -1, 0xFEED, n, sizeof(n)), LS_BLE_LABEL_GUESS);
    LS_EQ_STR(n, "Tile, Inc.");
    LS_EQ_INT(ls_ble_names_label(anon, 0, -1, -1, n, sizeof(n)), LS_BLE_LABEL_GUESS);
    LS_EQ_STR(n, "Shenzhen Printer");
    LS_EQ_INT(ls_ble_names_label(RPA, 1, -1, -1, n, sizeof(n)), LS_BLE_LABEL_NONE);
    LS_EQ_INT(ls_ble_names_state(st, sizeof(st)), 2);
    LS_CHECK(strstr(st, "1 yours") != NULL);
    done();
}

LS_CASE(ble_names_files_read_again_when_changed)
{
    setup(true, "company 0x0059 My Nordic\n", "c0:11:22:33:44:55 Bike lock\n");
    char n[LS_BLE_NAMES_LEN];
    ls_ble_names_company(0x0059, n, sizeof(n));
    ls_ble_names_work();
    LS_CHECK(!ls_ble_names_service(0x18F0, n, sizeof(n)));
    ls_ble_names_work();
    LS_CHECK(!ls_ble_names_service(0x18F0, n, sizeof(n)));                   /* not in any file */
    /* an app names it and remembers a device */
    put_text(USER, "ab", "service 18f0 Thermal printer\n");
    put_text(DEVS, "ab", "e0:6e:41:00:10:62 Label maker\n");
    later_ms(1000);
    ls_ble_names_work();                                                     /* not looked at yet */
    LS_CHECK(!ls_ble_names_service(0x18F0, n, sizeof(n)));
    later_ms(3000);
    ls_ble_names_work();
    LS_CHECK(ls_ble_names_service(0x18F0, n, sizeof(n)));
    LS_EQ_STR(n, "Thermal printer");
    LS_CHECK(ls_ble_names_device(PRINTER, 0, n, sizeof(n)));
    LS_EQ_STR(n, "Label maker");
    LS_CHECK(ls_ble_names_device(STATIC, 1, n, sizeof(n)));
    /* a later line for the same address wins */
    put_text(DEVS, "ab", "c0:11:22:33:44:55 Blue bike lock\n");
    later_ms(3000);
    ls_ble_names_work();
    LS_CHECK(ls_ble_names_device(STATIC, 1, n, sizeof(n)));
    LS_EQ_STR(n, "Blue bike lock");
    /* the name file appears later (PACKS): names come without a restart */
    setup(false, NULL, NULL);
    LS_CHECK(!ls_ble_names_company(0x004C, n, sizeof(n)));
    ls_ble_names_work();
    LS_CHECK(!ls_ble_names_company(0x004C, n, sizeof(n)));
    char st[96];
    LS_EQ_INT(ls_ble_names_state(st, sizeof(st)), 0);
    put(IDS, "wb", img, build_img());
    later_ms(3000);
    ls_ble_names_work();
    LS_CHECK(!ls_ble_names_company(0x004C, n, sizeof(n)));
    ls_ble_names_work();
    LS_CHECK(ls_ble_names_company(0x004C, n, sizeof(n)));
    LS_EQ_INT(ls_ble_names_state(st, sizeof(st)), 2);
    done();
}

LS_CASE(ble_names_learning_rules)
{
    setup(true, "device e0:6e:41:00:10:62 Desk printer\n", NULL);
    LS_EQ_INT(ls_ble_names_learn(STATIC, 1, "Early"), 0);                    /* files not read yet */
    ls_ble_names_work();
    LS_EQ_INT(ls_ble_names_learn(RPA, 1, "Phone"), 0);                       /* private: never kept */
    LS_EQ_INT(ls_ble_names_learn(STATIC, 3, "  "), 0);                       /* nothing to keep */
    LS_EQ_INT(ls_ble_names_learn(PRINTER, 0, "PRT-01"), 0);                  /* the owner's name wins */
    LS_EQ_INT(ls_ble_names_learn(STATIC, 1, " Bike lock "), 1);
    LS_EQ_INT(ls_ble_names_learn(STATIC, 1, "Bike lock"), 0);                /* the same */
    char n[LS_BLE_NAMES_LEN];
    LS_CHECK(ls_ble_names_device(STATIC, 1, n, sizeof(n)));
    LS_EQ_STR(n, "Bike lock");
    LS_EQ_INT(ls_ble_names_learn(STATIC, 1, "Bike lock 2"), 0);              /* changed too soon */
    later_ms(1000);
    ls_ble_names_work();
    LS_EQ_INT(lines_in(DEVS, "c0:11:22:33:44:55 Bike lock"), 1);
    later_ms(61000);
    LS_EQ_INT(ls_ble_names_learn(STATIC, 1, "Bike lock 2"), 1);
    later_ms(1000);
    ls_ble_names_work();
    LS_EQ_INT(lines_in(DEVS, NULL), 2);
    LS_EQ_INT(lines_in(DEVS, "c0:11:22:33:44:55 Bike lock 2"), 1);
    /* our own append is not a change to read again; an app's is */
    later_ms(3000);
    ls_ble_names_work();
    LS_CHECK(ls_ble_names_device(STATIC, 1, n, sizeof(n)));
    LS_EQ_STR(n, "Bike lock 2");
    /* a name learned but not written yet survives a reload of the files */
    const uint8_t pub[6] = {0x00, 0x1a, 0x7d, 1, 2, 3};
    LS_EQ_INT(ls_ble_names_learn(pub, 0, "Speaker"), 1);
    put_text(USER, "ab", "company 0x0059 My Nordic\n");
    later_ms(3000);
    ls_ble_names_work();                                                     /* reads the files again first, then writes */
    LS_CHECK(ls_ble_names_device(pub, 0, n, sizeof(n)));
    LS_EQ_STR(n, "Speaker");
    LS_CHECK(ls_ble_names_company(0x0059, n, sizeof(n)));
    LS_EQ_INT(lines_in(DEVS, "00:1a:7d:01:02:03 Speaker"), 1);
    done();
}

LS_CASE(ble_names_append_rate_limit)
{
    setup(false, NULL, NULL);
    char st[64];
    ls_ble_names_state(st, sizeof(st));
    ls_ble_names_work();
    uint8_t m[6] = {0x00, 0x11, 0x22, 0, 0, 0};
    int kept = 0;
    for (int i = 0; i < 12; i++) { m[5] = (uint8_t)i; char nm[16]; snprintf(nm, sizeof(nm), "dev %d", i); kept += ls_ble_names_learn(m, 0, nm); }
    LS_EQ_INT(kept, 12);                                                     /* all held in RAM */
    later_ms(1000);
    ls_ble_names_work();
    LS_EQ_INT(lines_in(DEVS, NULL), 8);                                      /* a burst of 8 written */
    char n[LS_BLE_NAMES_LEN];
    m[5] = 11;
    LS_CHECK(ls_ble_names_device(m, 0, n, sizeof(n)));
    LS_EQ_STR(n, "dev 11");
    later_ms(3000);                                                          /* one more token */
    m[5] = 20;
    LS_EQ_INT(ls_ble_names_learn(m, 0, "dev 20"), 1);
    m[5] = 21;
    LS_EQ_INT(ls_ble_names_learn(m, 0, "dev 21"), 1);
    later_ms(1000);
    ls_ble_names_work();
    LS_EQ_INT(lines_in(DEVS, NULL), 9);
    LS_EQ_INT(lines_in(DEVS, "dev 20"), 1);
    LS_EQ_INT(lines_in(DEVS, "dev 21"), 0);
    done();
}
