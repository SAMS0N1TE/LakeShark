/* Host regression tests. See README.md for the standalone compiler command. */
#include "lakeshark_app.h"
#undef LAP_APP
#define LAP_APP(...)
#include "../meshling.c"
#include <assert.h>

static uint8_t disk[4096];
static size_t disk_len, disk_pos;
static uint32_t test_ms = 2000;
uint32_t lap_millis(void) { return test_ms; }
uint32_t lap_random(void) { return 0; }
int lap_file_open(const char *name, const char *mode)
{
    (void)name; disk_pos = 0;
    if (*mode == 'w') disk_len = 0;
    return disk_len || *mode == 'w' ? 1 : -1;
}
int lap_file_read(int fd, void *buf, size_t n)
{
    (void)fd; if (n > disk_len - disk_pos) n = disk_len - disk_pos;
    memcpy(buf, disk + disk_pos, n); disk_pos += n; return (int)n;
}
int lap_file_write(int fd, const void *buf, size_t n)
{
    (void)fd; assert(disk_pos + n <= sizeof(disk));
    memcpy(disk + disk_pos, buf, n); disk_pos += n; disk_len = disk_pos; return (int)n;
}
void lap_file_close(int fd) { (void)fd; }
bool lap_tone(int h, int m, int v, int w) { (void)h; (void)m; (void)v; (void)w; return true; }
bool lap_haptic(int pattern) { (void)pattern; return true; }
bool lap_touch(lap_touch_t *t) { (void)t; return false; }
void lap_cell_px(int *w, int *h) { *w = 10; *h = 17; }

static uint32_t mock_rx = 10, mock_wifi = 1;
bool lap_mesh_self(lap_mesh_self_t *out) { *out = (lap_mesh_self_t){.running = true, .rx_packets = mock_rx}; return true; }
int lap_mesh_nodes(lap_mesh_node_t *out, int max)
{
    (void)max; *out = (lap_mesh_node_t){0}; strcpy(out->id, "0123456789abcdef"); return 1;
}
uint32_t lap_mesh_seq(void) { return 0; }
int lap_mesh_messages(lap_mesh_msg_t *out, int max) { (void)out; (void)max; return 0; }
int lap_ble_devices(lap_ble_dev_t *out, int max)
{
    (void)max; *out = (lap_ble_dev_t){.addr = {1,2,3,4,5,6}, .age_ms = 100}; return 1;
}
uint32_t lap_wifi_seq(void) { return mock_wifi; }
int lap_wifi_aps(lap_wifi_ap_t *out, int max)
{
    (void)max; *out = (lap_wifi_ap_t){0}; strcpy(out->ssid, "TEST-AP"); return 1;
}

static void migration(void)
{
    /* This is the exact original layout, with nonzero hash arrays and ABI pad. */
    pet_v1_t old;
    memset(&old, 0x5a, sizeof(old));
    old.magic = PET_MAGIC; old.version = 1;
    memcpy(old.name, "LEGACY\0\0\0\0\0\0", 12);
    old.born = 1700000000; old.last = 1792756800;
    old.food = 64; old.joy = 56; old.energy = 71;
    old.xp = 1300; old.packets = 900; old.pets = 127; old.messages = 19;
    old.n_nodes = 20; old.n_nets = 3; old.n_ble = 9;
    memcpy(disk, &old, sizeof(old)); disk_len = sizeof(old);
    active_form = -1; load();
    assert(sizeof(pet_v1_t) == 2000 && sizeof(pet_t) == 2016);
    old.version = 2;
    assert(memcmp(&old, &pet.core, sizeof(old)) == 0);
    assert(pet.diet[0] == 1400 && pet.diet[1] == 90 && pet.diet[2] == 75);
    assert(form() == RELAY);
    assert((pet.forms & 15) == 15);
    pet.forms |= 1u << SWARM; save();
    pet_t expected = pet;
    memset(&pet, 0xff, sizeof(pet)); load();
    assert(!memcmp(&pet, &expected, sizeof(pet)));
    disk_len--; load(); assert(pet.core.xp == 0 && pet.core.magic == PET_MAGIC);
    save(); ((pet_t *)disk)->core.n_nodes = SEEN_NODES + 1;
    load(); assert(pet.core.n_nodes == 0);
    save(); ((pet_t *)disk)->core.version = 99;
    load(); assert(pet.core.xp == 0);
    puts("PASS v1 migration, v2 round trip, truncated/invalid saves");
}

static void branches(void)
{
    active_form = -1;
    meshling_preview("nexus"); assert(form() == NEXUS);
    for (int i = 0; i < 3; i++) {
        pet.diet[0] = pet.diet[1] = pet.diet[2] = 200;
        pet.diet[i] = 600; assert(form() == RELAY + i);
        pet.diet[i] = 599; assert(form() == NEXUS);
    }
    meshling_preview("relay"); evolve();
    pet.core.food = 19; evolve(); assert(active_form == DRIFTER);
    pet.core.food = 25; evolve(); assert(active_form == DRIFTER);
    pet.core.food = 30; evolve(); assert(active_form == RELAY);
    pet.core.food = pet.core.joy = 0; evolve(); assert(active_form == RELIC);
    pet.core.food = pet.core.joy = 10; evolve(); assert(active_form == RELIC);
    pet.core.food = 12; evolve(); assert(active_form == DRIFTER);
    pet.core.food = pet.core.joy = 80; evolve(); assert(active_form == RELAY);
    assert(pet.forms & (1u << RELIC)); assert(pet.forms & (1u << DRIFTER));
    assert(evolve_at == test_ms);
    pet.core.energy = 0; asleep = true; assert(form() == RELAY);
    pet.core.xp = 39; assert(form() == FORM_EGG);
    gain(1161); evolve(); assert(active_form == RELAY);
    assert((pet.forms & 7) == 7);
    pet.core.xp = UINT32_MAX - 1; gain(10); assert(pet.core.xp == UINT32_MAX);
    pet.diet[0] = 1000001; pet.diet[1] = pet.diet[2] = 0;
    diet_add(DIET_BLE, UINT32_MAX); assert(pet.diet[0] == 500000 && pet.diet[1] == 65535);
    puts("PASS diet threshold, recovery hysteresis, DEX history, XP saturation");
}

static void rendering(void)
{
    static uint8_t dots[120 * 200 + 32];
    static const char *const names[] = {"egg", "hatchling", "sprout", "relay", "swarm", "scanner", "nexus", "drifter", "relic"};
    memset(dots, 0xa5, sizeof(dots));
    for (int layout_id = 0; layout_id < 3; layout_id++) {
    const int widths[] = {120, 184, 64}, heights[] = {200, 57, 100};
    cv = (lap_canvas_t){dots + 16, widths[layout_id], heights[layout_id], 0, 0};
    scale = layout_id ? 2 : 3; ground_y = cv.h - cv.h / 5;
    for (int f = 0; f < FORM_COUNT; f++) {
        active_form = -1; meshling_preview(names[f]); evolve();
        assert(active_form == f);
        for (int mood = 0; mood < 4; mood++) {
            asleep = mood == 1; hearts_until = mood == 2 ? test_ms + 100 : 0;
            chomp_until = mood == 3 ? test_ms + 100 : 0;
            for (int frame = 0; frame < 20; frame++) {
                test_ms += 40; lc_clear(&cv, LC_BLACK); draw_pet(); draw_evolution();
                for (int i = 0; i < 16; i++) assert(dots[i] == 0xa5 && dots[sizeof(dots)-1-i] == 0xa5);
                assert(pet_x0 >= 0 && pet_x0 + pet_w <= cv.w);
                for (int x = 0; x < cv.w; x++) assert(lc_get(&cv, x, 0) == LC_BLACK);
            }
        }
    }
    }
    puts("PASS all forms/moods raster bounds, portrait/landscape/narrow layouts");
}

static void radios(void)
{
    memset(&pet, 0, sizeof(pet)); asleep = false; last_rx = wifi_seq = 0;
    test_ms = 2000; listen();
    assert(pet.diet[0] == 25 && pet.diet[1] == 10 && pet.diet[2] == 25);
    listen(); assert(pet.diet[0] == 25 && pet.diet[1] == 10 && pet.diet[2] == 25);
    mock_rx += 7; mock_wifi++; listen();
    assert(pet.diet[0] == 32 && pet.diet[2] == 25); /* Same AP is not a new meal. */
    const uint32_t xp = pet.core.xp;
    test_ms = 60000; listen(); assert(pet.diet[1] == 20 && pet.core.xp == xp + 2);
    test_ms = 61000; listen(); assert(pet.diet[1] == 20);
    asleep = true; test_ms = 120000; mock_rx++; listen();
    assert(pet.diet[0] == 32 && pet.diet[1] == 20);
    puts("PASS real radio diet accounting, deduplication, timed BLE company, sleep");
}

int main(void) { migration(); branches(); rendering(); radios(); return 0; }
