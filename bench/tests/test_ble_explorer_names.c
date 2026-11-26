/* BLE EXPLORER names (app-sdk/apps/ble_explorer/bx_core.c with the shared
   name library app-sdk/lib/lap_ids.c on a fake card): the order a name is
   found in (owner's names, then the card's, then the app's own SIG list
   while a card lookup is pending), what NAME files and saves, and device
   names learnt from adverts and the Device Name characteristic. */
#include "ls_test.h"
#include "bx.h"
#include "lap_ids.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- a fake card: files in memory, each job done when polled ---- */
typedef struct { char path[96]; uint8_t data[8192]; uint32_t len; bool there; } ffile_t;
static ffile_t files[6];
typedef struct { int kind; char path[96]; uint32_t off, len; void *buf; uint8_t wdata[256]; bool used; } fjob_t;
static fjob_t jobs[8];

static ffile_t *ff(const char *path, bool make)
{
    for (int i = 0; i < 6; i++) if (files[i].there && !strcmp(files[i].path, path)) return &files[i];
    if (!make) return NULL;
    for (int i = 0; i < 6; i++)
        if (!files[i].there) { memset(&files[i], 0, sizeof(files[i])); snprintf(files[i].path, 96, "%s", path); files[i].there = true; return &files[i]; }
    return NULL;
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
static void pump(int n) { for (int i = 0; i < n; i++) lap_ids_pump(); }

/* the app's namer: what main.c hands bx_set_namer */
static const char *card(int kind, const uint8_t be[16]) { return lap_ids_attr(kind, be); }

/* a namer that answers a frame late, like a card lookup */
static int asked;
static const char *slow(int kind, const uint8_t be[16])
{
    if (kind == BX_NAME_SVC && be[2] == 0x18 && be[3] == 0x0F) return asked++ ? "Battery (card)" : NULL;
    return NULL;
}

LS_CASE(names_card_first_built_in_while_pending) {
    const bx_uuid_t batt = {{0x0F, 0x18}, 2}, level = {{0x19, 0x2A}, 2};
    bx_set_namer(NULL);
    LS_EQ_STR(bx_service_name(&batt), "Battery");
    asked = 0;
    bx_set_namer(slow);
    LS_EQ_STR(bx_service_name(&batt), "Battery");         /* pending: the built-in name at once */
    LS_EQ_STR(bx_service_name(&batt), "Battery (card)");  /* a frame later: the card's */
    LS_EQ_STR(bx_char_name(&level), "Battery Level");      /* the card does not know it: built-in */
    const bx_uuid_t odd = {{0x34, 0x12}, 2};
    LS_CHECK(bx_char_name(&odd) == NULL);
    bx_set_namer(NULL);
}

LS_CASE(names_uuid_forms_and_keys) {
    bx_uuid_t u;
    char t[48];
    uint8_t be[16];
    LS_CHECK(bx_uuid_parse("0x180F", 6, &u));
    LS_EQ_INT(u.len, 2);
    LS_EQ_UINT(bx_uuid16(&u), 0x180Fu);
    bx_uuid_be(&u, be);
    uint8_t want[16];
    lap_ids_uuid16_be(0x180F, want);
    LS_CHECK(!memcmp(be, want, 16));                       /* the same bytes the name library files under */
    const char *nus = "6e400001-b5a3-f393-e0a9-e50e24dcca9e";
    LS_CHECK(bx_uuid_parse(nus, strlen(nus), &u));
    LS_EQ_INT(u.len, 16);
    LS_EQ_INT(u.b[15], 0x6e);
    bx_uuid_key(&u, t, sizeof(t));
    LS_EQ_STR(t, nus);
    int bits = 0;
    LS_CHECK(lap_ids_parse_uuid(t, want, &bits));
    bx_uuid_be(&u, be);
    LS_CHECK(!memcmp(be, want, 16));
    LS_EQ_INT(bits, 128);
    LS_CHECK(bx_uuid_parse("0000180f-0000-1000-8000-00805f9b34fb", 36, &u));
    LS_EQ_INT(u.len, 2);
    bx_uuid_key(&u, t, sizeof(t));
    LS_EQ_STR(t, "180f");
    const bx_uuid_t u32 = {{0x78, 0x56, 0x34, 0x12}, 4};
    bx_uuid_key(&u32, t, sizeof(t));
    LS_EQ_STR(t, "12345678-0000-1000-8000-00805f9b34fb");
    LS_CHECK(!bx_uuid_parse("18f", 3, &u));
    LS_CHECK(!bx_uuid_parse("xyzw", 4, &u));
}

LS_CASE(names_name_saves_and_shows_at_once) {
    memset(files, 0, sizeof(files));
    memset(jobs, 0, sizeof(jobs));
    lap_ids_open();
    pump(30);
    bx_set_namer(card);
    const bx_uuid_t printer = {{0xF0, 0x18}, 2};
    bx_uuid_t vendor;
    const char *vt = "49535343-fe7d-4ae5-8fa9-9fafd205e455";
    LS_CHECK(bx_uuid_parse(vt, strlen(vt), &vendor));
    for (int i = 0; i < 5; i++) { bx_service_name(&printer); pump(2); }   /* no card file: nothing */
    LS_CHECK(bx_service_name(&printer) == NULL);
    LS_CHECK(bx_service_name(&vendor) == NULL);
    /* NAME on a service row: the kind word and the key the app files */
    char key[48];
    bx_uuid_key(&printer, key, sizeof(key));
    LS_EQ_INT(lap_ids_user_add("service", key, "Thermal printer"), 0);
    LS_EQ_STR(bx_service_name(&printer), "Thermal printer");               /* at once */
    bx_uuid_key(&vendor, key, sizeof(key));
    LS_EQ_INT(lap_ids_user_add("service", key, "Microchip UART"), 0);
    LS_EQ_STR(bx_service_name(&vendor), "Microchip UART");
    /* an owner's name wins over the app's own */
    const bx_uuid_t level = {{0x19, 0x2A}, 2};
    LS_EQ_STR(bx_char_name(&level), "Battery Level");
    LS_EQ_INT(lap_ids_user_add("char", "2a19", "Cell charge"), 0);
    LS_EQ_STR(bx_char_name(&level), "Cell charge");
    const bx_uuid_t cccd = {{0x02, 0x29}, 2};
    LS_EQ_INT(lap_ids_user_add("desc", "2902", "Notify switch"), 0);
    LS_EQ_STR(bx_desc_name(&cccd), "Notify switch");
    pump(10);
    const ffile_t *uf = ff(LAP_IDS_USER_FILE, false);
    LS_CHECK(uf != NULL);
    if (!uf) return;
    LS_CHECK(strstr((const char *)uf->data, "service 18f0 Thermal printer\n") != NULL);
    LS_CHECK(strstr((const char *)uf->data, "service 49535343-fe7d-4ae5-8fa9-9fafd205e455 Microchip UART\n") != NULL);
    LS_CHECK(strstr((const char *)uf->data, "char 2a19 Cell charge\n") != NULL);
    /* reopened, the names come back from the file */
    lap_ids_open();
    pump(40);
    const char *n = NULL;
    for (int i = 0; !n && i < 20; i++) { n = bx_service_name(&printer); pump(1); }
    LS_EQ_STR(n, "Thermal printer");
    bx_set_namer(NULL);
}

LS_CASE(names_devices_learnt_and_owner_names_win) {
    memset(files, 0, sizeof(files));
    memset(jobs, 0, sizeof(jobs));
    lap_ids_open();
    pump(30);
    /* an advert with both names: the complete one */
    const uint8_t ad[] = {2, 0x01, 0x06, 5, 0x08, 'F', 'o', 'r', 'e', 11, 0x09, 'F', 'o', 'r', 'e', 'r', 'u', 'n', 'n', 'e', 'r', 3, 0xFF, 0x87, 0x00};
    char nm[24];
    LS_EQ_INT(bx_ad_name(ad, sizeof(ad), nm, sizeof(nm)), 0x09);
    LS_EQ_STR(nm, "Forerunner");
    const uint8_t shortonly[] = {5, 0x08, 'M', 'X', '0', '6'};
    LS_EQ_INT(bx_ad_name(shortonly, sizeof(shortonly), nm, sizeof(nm)), 0x08);
    LS_EQ_STR(nm, "MX06");
    const uint8_t none[] = {2, 0x01, 0x06, 3, 0x03, 0x0F, 0x18};
    LS_CHECK(!bx_ad_name(none, sizeof(none), nm, sizeof(nm)));
    const uint8_t broken[] = {9, 0x09, 'a'};
    LS_CHECK(!bx_ad_name(broken, sizeof(broken), nm, sizeof(nm)));
    /* learnt from the advert, then shown when it advertises without one */
    const uint8_t watch[6] = {0x0c, 0xfe, 0x11, 0x72, 0x39, 0xd0};       /* d0:39:72:11:fe:0c, public */
    LS_CHECK(lap_ids_device(watch, 0) == NULL);
    LS_EQ_INT(lap_ids_device_learn(watch, 0, "Forerunner"), 1);
    LS_EQ_INT(lap_ids_device_learn(watch, 0, "Forerunner"), 0);           /* the same again writes nothing */
    LS_EQ_STR(lap_ids_device(watch, 0), "Forerunner");
    /* the Device Name characteristic renames it */
    LS_EQ_INT(lap_ids_device_learn(watch, 0, "Forerunner 265"), 1);
    LS_EQ_STR(lap_ids_device(watch, 0), "Forerunner 265");
    /* NAME on a device: an owner name, which a later advert does not undo */
    LS_EQ_INT(lap_ids_user_add("device", "d0:39:72:11:fe:0c", "My watch"), 0);
    LS_EQ_STR(lap_ids_device(watch, 0), "My watch");
    LS_EQ_INT(lap_ids_device_learn(watch, 0, "Forerunner 265"), 0);
    LS_EQ_STR(lap_ids_device(watch, 0), "My watch");
    /* a resolvable private address changes in minutes: never kept */
    const uint8_t rpa[6] = {1, 2, 3, 4, 5, 0x5a};
    LS_EQ_INT(lap_ids_device_learn(rpa, 1, "Phone"), 0);
    LS_CHECK(lap_ids_device(rpa, 1) == NULL);
    pump(10);
    const ffile_t *df = ff(LAP_IDS_DEVICE_FILE, false), *uf = ff(LAP_IDS_USER_FILE, false);
    LS_CHECK(df && strstr((const char *)df->data, "d0:39:72:11:fe:0c Forerunner 265\n") != NULL);
    LS_CHECK(uf && strstr((const char *)uf->data, "device d0:39:72:11:fe:0c My watch\n") != NULL);
    /* reopened: both come back and the owner's still wins */
    lap_ids_open();
    pump(40);
    LS_EQ_STR(lap_ids_device(watch, 0), "My watch");
}
