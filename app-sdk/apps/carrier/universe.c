/* Repeatable, independent system seeds: visiting one cannot perturb another. */
#include "internal.h"

uint32_t game_gps_seed(bool fix, float lat, float lon) {
    if (!fix || !isfinite(lat) || !isfinite(lon) || fabsf(lat) > 90 || fabsf(lon) > 180)
        return 0x43415252;
    int32_t y = (int32_t)roundf(lat * 11.1f), x = (int32_t)roundf(lon * 11.1f * cosf(lat * CG_RAD));
    return cg_hash((uint32_t)y ^ cg_hash((uint32_t)x));
}
void game_universe(uint32_t seed, game_system *out) {
    static const char syllables[][4] = {"Ar", "Be", "Ca",  "Di",  "En",  "Fa",  "Go",  "Ha",
                                        "Io", "Ju", "Ke",  "Lu",  "Ma",  "Ni",  "Or",  "Pi",
                                        "Qu", "Ra", "So",  "Tu",  "Ur",  "Ve",  "Wo",  "Xe",
                                        "Ya", "Za", "Tor", "Lan", "Ris", "Den", "Mir", "Sol"};
    for (int i = 0; i < GAME_SYSTEMS; i++) {
        game_system *s = &out[i];
        memset(s, 0, sizeof(*s));
        uint32_t r = cg_hash(seed + (uint32_t)i * 0x9e3779b9);
        s->seed = r;
        int count = 2 + (cg_rand(&r) & 1);
        for (int k = 0; k < count; k++) {
            const char *sy = syllables[cg_rand(&r) % 32];
            size_t n = strlen(s->name);
            for (int j = 0; sy[j]; j++)
                s->name[n + j] = (k && sy[j] >= 'A' && sy[j] <= 'Z') ? sy[j] + 32 : sy[j];
        }
        s->x = cg_unit(&r) * 64;
        s->y = cg_unit(&r) * 64;
        s->government = cg_rand(&r) % 8;
        s->economy = cg_rand(&r) % 8;
        s->tech = (uint8_t)(1 + (7 - s->economy) / 2 + s->government / 2 + cg_rand(&r) % 5);
        s->population = (s->tech + 1) * (s->government + 1) * 0.13f;
        cg_description(s);
    }
}
void game_system_contents(uint32_t seed, game_contents *o) {
    memset(o, 0, sizeof(*o));
    uint32_t r = seed;
    o->star_type = cg_rand(&r) % 4;
    o->star_radius = 360 + cg_unit(&r) * 260;
    o->star = l3v(-4200, 1800, -9000);
    o->nebula = (cg_rand(&r) & 1) ? LAP_BLUE : LAP_MAGENTA;
    o->planet_count = 1 + cg_rand(&r) % 5;
    o->belt = (cg_rand(&r) % 3) == 0;
    for (int i = 0; i < o->planet_count; i++) {
        game_planet *p = &o->planets[i];
        p->seed = cg_rand(&r);
        p->radius = 240 + cg_unit(&r) * 350;
        p->orbit = 2700 + i * 1900;
        float a = (i ? cg_unit(&r) * 5 : 0.42f);
        p->position = l3v(sinf(a) * p->orbit, -180 - cg_unit(&r) * 300, -cosf(a) * p->orbit);
        p->palette = cg_rand(&r) % 4;
        p->rings = (cg_rand(&r) % 3) == 0;
        p->moon = (cg_rand(&r) & 1) != 0;
    }
    o->station = l3v(0, 0, 0);
    cg_station_name(seed,o->station_name);
}
float game_distance(const game_system *a, const game_system *b) {
    return hypotf(a->x - b->x, a->y - b->y);
}
