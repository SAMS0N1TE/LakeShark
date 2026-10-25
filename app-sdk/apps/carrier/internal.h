/* Private systems and fixed PSRAM pools. Never expose mutable state to the UI.
 */
#ifndef CARRIER_INTERNAL_H
#define CARRIER_INTERNAL_H
#include "game.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CG_PI 3.14159265f
#define CG_RAD 0.01745329252f
#define CG_BEAMS 12
#define CG_MISSILES 8
#define CG_DEBRIS 64
#define CG_CANS 12
typedef struct {
    l3_vec a, b;
    float life;
    bool player;
} cg_beam;
typedef struct {
    l3_vec p, v;
    float life;
    int target;
    bool player;
    uint8_t kind; float damage; uint32_t target_id, owner_id;
    int owner, subtarget; bool lost_lock;
} cg_missile;
typedef struct {
    l3_vec p, v;
    float life;
    uint8_t ink;
} cg_debris;
typedef struct {
    l3_vec p;
    float life;
    int good;
    uint32_t mission_id; /* lore: zero for ordinary cargo */
} cg_can;
/* shipyard */
/* Editable fittings and station eligibility, in the app PSRAM pool. */
typedef struct { uint32_t price; float fuel_max; uint8_t pylons, mounts, seats;
    uint8_t min_tech, government_mask, stock_percent; char pitch[41]; } cg_yard_hull;
typedef struct { cg_yard_hull hulls[11]; uint8_t trade_percent, insurance_percent; } cg_yard_rules;
/* combat: tuning rows are validated at entry, all mutable storage is PSRAM. */
typedef struct { float damage, interval, capacitor, heat, speed, life;
    uint16_t ammo; uint32_t price; uint8_t tech, group; } cg_weapon_rule;
enum { CD_SYS_BASE, CD_SYS_STEP, CD_RESIST, CD_SPEED, CD_TURN, CD_ENG_BASE,
    CD_ENG_STEP, CD_WEP_BASE, CD_WEP_STEP, CD_COUNT };
enum { CF_REBOOT, CF_CELL, CF_CELL_HEAT, CF_BOOST_TIME, CF_BOOST_COOLDOWN,
    CF_BOOST_SPEED, CF_BOOST_HEAT, CF_CHAFF, CF_SINK, CF_OXYGEN, CF_HEAT_DAMAGE,
    CF_CANOPY_DAMAGE, CF_MODULE_DAMAGE, CF_SCAN_TIME, CF_SCAN_RANGE, CF_LOCK_TIME,
    CF_ACCURACY, CF_EARLY_ACCURACY, CF_EARLY_REGEN, CF_COUNT };
typedef struct { float damage[3][2], distributor[CD_COUNT], defence[CF_COUNT];
    cg_weapon_rule weapons[WEAPON_COUNT]; uint16_t supplies[3], supply_price[3];
    uint8_t npc_weapons[3][GAME_MOUNTS];
} cg_combat_rules;
typedef struct { uint32_t id; float modules[GAME_MODULES], cooldown[GAME_MOUNTS];
    float reboot, last_attack, capacitor, heat; uint8_t mounts;
    game_mount weapons[GAME_MOUNTS]; bool spilled, initialized;
} cg_combat_contact;
typedef struct {
    game_ship_stats ships[13];
    int base[GAME_GOODS], slope[GAME_GOODS], equipment[EQ_COUNT];
    float laser_dps, laser_range, cooling, fuel_price, repair_price, npc_damage;
    int version;
    /* shipyard */
    cg_yard_rules yard;
    cg_combat_rules combat;
    float reverse, aim_assist_deg[3];
    float weapons_cool_s, gimbal_deg, turret_deg, turret_rate_dps;
    float mount_damage[3], mount_price[3];
} cg_rules;
typedef struct {
    float day_seconds, scan_seconds, heat_gain, heat_cool, scoop_rate, heat_damage;
    int event_percent, mission_days, mission_reward, fine, trade_percent;
    uint32_t rank[9];
} cg_system_rules;
/* Encounter director: bounded rules, chatter and steering live in app PSRAM. */
#define CG_ENCOUNTER_KINDS 11
#define CG_CHATTER_LINES 64
typedef enum { CE_NONE, CE_PIRATES, CE_CONVOY, CE_PATROL, CE_HUNTERS, CE_DERELICT,
    CE_DISTRESS, CE_SMUGGLERS, CE_PROSPECTOR, CE_DRONES, CE_EXPLORER, CE_SALVAGE, CE_LANE } cg_encounter_kind;
typedef struct {
    float cadence[6], deep_scale, safe_radius, chatter_seconds[2];
    int weights[CG_ENCOUNTER_KINDS];
    char messages[CG_ENCOUNTER_KINDS][41], chatter[CG_CHATTER_LINES][41];
} cg_encounter_rules;
typedef struct {
    uint32_t leader;
    l3_vec offset;
    float life, scan, phase;
    int rock;
    uint8_t kind;
    bool fleeing, escort, scanned;
} cg_encounter_contact;

/* poi: bounded exploration rules and persistence, inside the PSRAM app pool. */
typedef struct { char name[21], text[3][31]; float radius, size;
    uint32_t reward; uint8_t colour, weight; } cg_poi_kind;
typedef struct { float scan_seconds, scoop_seconds, scanner_range, gravity, ion_drain;
    cg_poi_kind kinds[POI_KIND_COUNT]; uint32_t ranks[9]; char rank_names[9][21];
} cg_poi_rules;
typedef struct { uint8_t discovered, scanned, answered; } cg_poi_marks;
typedef struct { cg_poi_rules rules;
    cg_poi_marks marks[GAME_SYSTEMS + 1];
    unsigned char buffer[1536];
    uint32_t galaxy, radio_ids[16], radio_done; uint8_t route_step;
    float scoop[GAME_POIS], radio_poll, star_hold, quiet_hold;
    float shield_fore, shield_aft; bool shield_valid;
    uint8_t activated; int route[3], chain[5], saved_hidden;
    bool loaded, configured, dirty, hidden_pending;
} cg_poi_state;
typedef struct {
    game_view v;
    cg_rules rules;
    l3_vec anchor[GAME_CONTACTS], desired[GAME_CONTACTS];
    float contact_age[GAME_CONTACTS], cooldown[GAME_CONTACTS], dead_until[GAME_CONTACTS];
    bool provoked[GAME_CONTACTS], rescued[GAME_CONTACTS];
    int rescue_target;
    float poll, fire_cooldown, rescue, ecm, dock_time, dust_distance;
    uint32_t rng, next_id, save_seq;
    cg_beam beams[CG_BEAMS];
    cg_missile missiles[CG_MISSILES];
    cg_debris debris[CG_DEBRIS];
    cg_can cans[CG_CANS];
    lap_aircraft_t aircraft[16];
    l3_vec stars[3][64], star_world[3][64], dust[24];
    uint8_t texture[5][32 * 16];
    l3_scene *scene;
    int faction_mat[13], planet_mat[5], star_mat[4], engine_mat, rock_mat, slot_mat, ring_mat,
        tex_id[32];
    uint32_t render_seed;
    bool render_ready, dock_slot;
    unsigned char save_buffer[8192];
    /* part 2: all pools live in the app allocator's PSRAM. */
    cg_system_rules systems;
    float flight_seconds, sensor_seconds;
    int wifi_count, talk_kind;
    uint32_t last_tick_ms, last_scan_ms;
    bool scan_started;
    uint32_t talk_id, talk_day, talk_used[GAME_CONTACTS];
    uint32_t friendly[GAME_REGISTRY];
    int friend_count;
    uint32_t ble_seen[24];
    lap_wifi_ap_t wifi[16];
    lap_ble_dev_t ble[24];
    lap_mesh_node_t nodes[4];
    bool police_origin[GAME_CONTACTS], lane_out[GAME_CONTACTS], mapped[GAME_CONTACTS], npc_control[GAME_CONTACTS];
    /* encounters */
    cg_encounter_rules encounters;
    cg_encounter_contact encounter[GAME_CONTACTS];
    int encounter_enemy[GAME_CONTACTS], encounter_system, last_chatter;
    float encounter_tick, encounter_due, chatter_due, ambient_due;
    uint32_t encounter_rng, encounter_count;
    bool importing;
    /* poi */
    cg_poi_state poi;
    /* cruise */
    struct { float bleed, bleed_factor, hit, stall, due, elapsed;
        uint32_t who_id; l3_vec axis; char reason[25]; } cruise;
    /* shipyard */
    int yard_station;
    /* combat: fixed pools, projectile ownership survives contact slot reuse. */
    cg_combat_contact fighters[GAME_CONTACTS];
    float mount_cooldown[GAME_MOUNTS], npc_lock[GAME_CONTACTS];
    uint32_t npc_lock_id[GAME_CONTACTS];
    struct { float damage; int owner, subtarget; uint32_t id, target_id; } shots[GAME_PROJECTILES];
    uint32_t selected_id; int selected_slot; bool missile_pending, cargo_spilled;
    float alarm_hull;
    int aim_assist;
    float thrust_x, thrust_y;   /* lateral and vertical thrusters, -1..1, while held */
} cg_state;
extern cg_state *cg;
static inline float cg_clamp(float v, float a, float b) { return fminf(b, fmaxf(a, v)); }
static inline float cg_len(l3_vec a) { return sqrtf(l3_dot(a, a)); }
static inline l3_vec cg_forward(void) {
    return l3_quat_rotate(cg->v.player.attitude, l3v(0, 0, -1));
}
static inline uint32_t cg_hash(uint32_t n) {
    n ^= n >> 16;
    n *= 0x7feb352d;
    n ^= n >> 15;
    n *= 0x846ca68b;
    return n ^ (n >> 16);
}
static inline uint32_t cg_rand(uint32_t *s) {
    *s += 0x9e3779b9;
    return cg_hash(*s);
}
static inline float cg_unit(uint32_t *s) { return (cg_rand(s) >> 8) * (1.0f / 16777216); }
bool cg_system_config(void);
void cg_system_update(float dt);
void cg_system_arrive(void);
void cg_day(void);
void cg_bulletin(void);
void cg_mission_dock(void);
void cg_mission_kill(uint32_t id);
void cg_mission_rescue(uint32_t id);
void cg_talk_open(int who);
bool cg_beacon_use(int who);
void cg_ores(l3_vec p, uint32_t seed);
void cg_friend(uint32_t id);
bool cg_friendly(uint32_t id);
void cg_name(game_contact *c);
int cg_enemy(int i);
int cg_register(const game_contact *c);
void cg_spawn(int i, uint32_t id, game_class type, game_role role, l3_vec p);
void cg_award(uint32_t credits);
void cg_message(const char *text);
bool cg_config(void);
void cg_traffic(float dt);
void cg_contacts(void);
void cg_flight(float dt);
void cg_combat(float dt);
void cg_damage(int target, float damage, l3_vec source, bool player);
void cg_explode(l3_vec p, int target, bool player);
void cg_beam_add(l3_vec a, l3_vec b, bool player);
void cg_market(void);
void cg_jump(float dt);
void cg_arrive(int system);
void cg_launch(void);
void cg_actions(void);
void cg_render_reset(void);
/* Encounter scheduling/AI share the existing contact, cargo and weapon pools. */
bool cg_encounter_config(void);
void cg_encounters(float dt);
bool cg_encounter_spawn(cg_encounter_kind kind);
bool cg_encounter_flight(int i, float dt);
int cg_encounter_enemy(int i);
void cg_encounter_hit(int i, bool player);
void cg_depart(int i);
/* lore: bounded text generators and mission observations, no IO or allocation. */
void cg_description(game_system *s);
void cg_station_name(uint32_t seed, char out[20]);
const char *cg_lore_open(game_role role, uint32_t seed);
const char *cg_lore_outcome(int outcome, uint32_t seed);
void cg_news(void);
void cg_mission_observe(void);
void cg_mission_offer(int slot, game_mission *out); /* today's public offer */
/* poi */
void cg_poi_arrive(void);
void cg_poi_update(float dt);
void cg_poi_route(int destination);
bool cg_poi_load(void);
bool cg_poi_config(void);
/* cruise: bounded flight/encounter state in the existing PSRAM allocation. */
void cg_cruise_reset(void);
bool cg_cruise_mass_locked(void);
void cg_cruise_update(float dt);
bool cg_interdict(int who);
void cg_cruise_director(float dt);
/* shipyard */
/* Derive view fittings; save validation uses an explicit player. */
void cg_ship_fit(game_player *p, game_class c);
bool cg_ship_valid(const game_player *p);
uint32_t cg_ship_trade_in(void);
/* combat: typed damage, lifecycle hooks and shared observations. */
void cg_damage_typed(int target, float amount, game_damage_type type,
    l3_vec source, bool player, int subtarget);
void cg_combat_player_init(game_player *p);
void cg_combat_player_ensure(game_player *p); /* migrate absent combat state, preserve damage */
void cg_combat_contact_init(int i);
void cg_combat_reset(void);
void cg_combat_dock(void);
void cg_combat_status(void);
void cg_combat_log(const char *text);
float cg_drive_scale(const game_player *p);
void cg_aim_observe(void);
void cg_weapons_hot(void);
void cg_cruise_hot(void);
#endif
