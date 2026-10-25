/* CARRIER's UI contract. Coordinates are right/up/back; forward is local -Z.
 * Views remain owned by the game until shutdown. UI: update, render, then dash.
 * No game rule depends on screen dimensions, text drawing or panel geometry. */
#ifndef CARRIER_GAME_H
#define CARRIER_GAME_H
#include "lap3d.h"
/* combat: fixed simulation pools and read-only screen observations. */
#define GAME_MOUNTS 4
#define GAME_PROJECTILES 48
#define GAME_HIT_DIRECTIONS 6
typedef enum { DAMAGE_THERMAL, DAMAGE_KINETIC, DAMAGE_EXPLOSIVE } game_damage_type;
typedef enum { WEAPON_PULSE, WEAPON_BEAM, WEAPON_RATTLE, WEAPON_SLUG,
    WEAPON_SEEKER, WEAPON_DUMBFIRE, WEAPON_COUNT, WEAPON_NONE = 255 } game_weapon_kind;
typedef enum { MODULE_POWER, MODULE_DRIVES, MODULE_WEAPONS, MODULE_SHIELDS,
    MODULE_SENSORS, MODULE_LIFE, MODULE_CARGO, GAME_MODULES } game_module;
typedef struct { uint8_t kind, group; bool firing; float charge, heat;
    uint16_t ammo; uint8_t mode; } game_mount;
typedef struct { l3_vec position, velocity; float life; uint8_t kind;
    bool player; } game_projectile;
typedef struct { l3_vec direction; float age; } game_hit_direction;
typedef enum { COMBAT_HULL_50=1, COMBAT_HULL_25=2, COMBAT_HULL_10=4,
    COMBAT_CANOPY=8, COMBAT_SHIELD_DOWN=16, COMBAT_SHIELD_UP=32 } game_combat_event;
#define GAME_SYSTEMS 256
#define GAME_CONTACTS 24
#define GAME_GOODS 16
#define GAME_REGISTRY 128
#define GAME_MESSAGES 8
typedef enum {
    SH_SIDEWINDER,
    SH_ADDER,
    SH_COBRA,
    SH_PYTHON,
    SH_ANACONDA,
    SH_BOA,
    SH_VIPER,
    SH_KRAIT,
    SH_MORAY,
    SH_ASP,
    SH_WORM,
    SH_PROBE,
    SH_DRONE,
    SH_STATION,
    SH_ROCK1,
    SH_ROCK2,
    SH_ROCK3,
    SH_COUNT
} game_class;
typedef enum { ROLE_TRADER, ROLE_PIRATE, ROLE_POLICE, ROLE_DERELICT, ROLE_RESCUE, /* part 2 */ ROLE_BEACON } game_role;
typedef enum {
    ACT_NONE,
    ACT_LAUNCH,
    ACT_HAIL,
    ACT_DOCK,
    ACT_JUMP,
    ACT_SCOOP,
    ACT_RESCUE
} game_action_t;
typedef enum { PANEL_NONE, PANEL_STATUS, PANEL_MARKET, PANEL_GALAXY, PANEL_LOG } game_panel;
typedef enum { JUMP_IDLE, JUMP_CHARGE, JUMP_TUNNEL } game_jump;
enum {
    EQ_LASER,
    EQ_MISSILE,
    EQ_SHIELD,
    EQ_DOCKING,
    EQ_SCOOP,
    EQ_ECM,
    EQ_ENERGY,
    EQ_CARGO,
    EQ_COUNT
};
typedef struct {
    char name[16], description[88];
    uint32_t seed;
    float x, y, population;
    uint8_t government, economy, tech;
} game_system;
typedef struct {
    float radius, orbit;
    l3_vec position;
    uint32_t seed;
    uint8_t palette;
    bool rings, moon;
} game_planet;
typedef struct {
    game_planet planets[5];
    int planet_count;
    l3_vec star, station;
    float star_radius;
    uint8_t star_type, nebula;
    bool belt;
    /* part 2 */
    uint8_t station_kind; /* 0 Coriolis, 1 dodecahedral */
    /* lore */
    char station_name[20];
} game_contents;
typedef struct {
    const char *name;
    float speed, turn, hull, shield, weapon, radius;
    int cargo;
} game_ship_stats;
typedef struct {
    uint32_t id;
    char callsign[9];
    game_class ship_class;
    game_role role;
    uint8_t faction;
    bool active, hostile, live;
    l3_vec position, velocity;
    l3_quat attitude;
    float range, bearing, height, hull, shield, speed, heading;
    int32_t altitude_ft;
    int16_t aircraft_speed_kt;
    uint16_t squawk;
    uint8_t emergency;
    /* part 2 */
    char name[16];
    /* encounters */
    float jump_out; /* seconds remaining in departure flash; zero otherwise */
    /* combat: unscanned identity/module details are zero/empty until scanned. */
    bool scanned, wanted, attacking_player;
    float scan_progress, modules[GAME_MODULES], shield_reboot;
    uint32_t bounty; char rank_text[16]; uint8_t threat;
    game_mount weapons[GAME_MOUNTS];
    bool detectable; /* sensor-health/range/occlusion result; simulation stays active */
} game_contact;
typedef struct {
    uint32_t id, visits;
    char callsign[9];
    uint8_t ship_class, faction;
    int32_t altitude_ft;
    int16_t speed_kt;
    bool live;
    /* part 2 */
    char name[20];
    uint32_t refuel_day; /* day + 1, zero means unused */
} game_registry_entry;
typedef struct {
    char name[16];
    int price, stock;
    bool illegal;
} game_market_item;
typedef struct {
    const char *name;
    uint32_t price;
    uint8_t min_tech;
    bool owned, available;
    uint8_t mode; uint32_t mode_price;
} game_equipment_offer;
typedef struct {
    l3_vec position, velocity;
    l3_quat attitude;
    float speed, throttle, pitch, roll, yaw;
    float shield_fore, shield_aft, hull, energy, heat, fuel;
    uint32_t credits, kills, equipment;
    uint16_t cargo[GAME_GOODS];
    uint8_t missiles, legal, rank;
    bool docked, docking, mass_locked, motion_steer, firing, dead;
    /* part 2 */
    float cabin_heat;
    uint32_t combat_score;
    /* cruise */
    bool cruising;
    float cruise_factor; /* 1 = normal */
    float cruise_charge; /* 0..1 while spooling */
    const char *cruise_reason; /* <=24 chars; owned by game */
    /* shipyard */
    uint8_t ship_class;
    float fuel_max;
    int seats;
    uint8_t pylons, mounts, lasers;
    /* combat: SYS/ENG/WEP are half-step values, total 6; heat is percent. */
    float pips[3], modules[GAME_MODULES], heat_level, weapon_capacitor;
    float shield_reboot[2], boost_left, boost_cooldown, chaff_left;
    float canopy, oxygen, seeker_lock;
    uint8_t shield_cells, chaff, heat_sinks;
    bool firing2; game_mount weapons[GAME_MOUNTS];
    game_hit_direction hit_directions[GAME_HIT_DIRECTIONS]; uint8_t hit_head, hit_count;
    /* aim + reverse */
    bool reversing;
    float weapons_hot;
} game_player;
/* part 2 */
typedef enum { MISSION_DELIVERY, MISSION_COURIER, MISSION_BOUNTY, MISSION_RESCUE } game_mission_kind;
typedef struct { game_mission_kind kind; char title[25]; int destination;
    uint32_t reward, deadline, target_id; /* deadline is an absolute game day */
    uint16_t amount; uint8_t good; bool accepted, done, failed, ready;
    /* lore */
    char brief[61];
    uint16_t progress; /* recovered canisters; amount is seats for passengers */
} game_mission;
typedef struct { int who, line_count, choice_count; char lines[3][31], choices[3][13];
    int good, amount, price; /* offered trade; price is total */
    bool open;
} game_talk;
typedef struct { l3_vec position; float radius, hull; bool active; } game_asteroid;
typedef enum { EVENT_NONE, EVENT_FESTIVAL, EVENT_FAMINE, EVENT_WAR, EVENT_PLAGUE } game_event;

/* poi */
#define GAME_POIS 6
typedef enum { POI_NEBULA, POI_DARK_STAR, POI_COMET, POI_WRECK, POI_RELIC, POI_SIGNAL,
    POI_PULSAR, POI_BINARY, POI_ION_STORM, POI_RUINS, POI_GENERATION, POI_TOOTHY,
    POI_BEACON, POI_KIND_COUNT } game_poi_kind;
typedef struct { game_poi_kind kind; l3_vec position, velocity, tail;
    float radius, size, scan_range, scan_progress, hold_progress;
    uint32_t seed, source_id; uint16_t squawk; uint8_t colour, variant;
    char name[21], scan_text[3][31]; bool discovered, scanned, answered;
} game_poi;
typedef struct { uint32_t count; char names[16][21]; uint8_t rank; } game_discoveries;
typedef struct {
    game_player player;
    game_system systems[GAME_SYSTEMS];
    game_contents contents;
    game_contact contacts[GAME_CONTACTS];
    game_registry_entry registry[GAME_REGISTRY];
    game_market_item market[GAME_GOODS];
    char messages[GAME_MESSAGES][80];
    int message_count, registry_count, contact_count, selected, system, plotted;
    uint32_t galaxy_seed, market_day, revision;
    float clock, station_angle, jump_progress, plotted_ly, rescue_progress;
    game_jump jump;
    game_action_t action;
    const char *action_label;
    game_panel panel;
    bool config_valid, save_ok;
    /* part 2 */
    game_talk talk;
    game_mission missions[6];
    char event_text[31];
    uint8_t local_hour;
    game_asteroid asteroids[18];
    /* encounters */
    int traffic_mode, encounter_level;
    /* lore */
    char news[4][41];
    /* poi */
    game_poi pois[GAME_POIS]; game_discoveries discoveries;
    int poi_count, scanning_poi, signal_poi, relic_waypoint;
    float poi_scan_progress, nebula_density, scanner_scale, signal_bearing;
    uint32_t found_eggs; uint8_t relic_chain;
    bool in_nebula, scanner_scrambled, hidden_system, hidden_system_found;
    char exploration_station[21];
    /* cruise */
    struct { bool active; int who; float tug; l3_vec escape; float progress; } interdiction;
    /* combat: events are a latched bitset, cleared explicitly by the consumer. */
    int subtarget; uint32_t combat_events;
    game_projectile projectiles[GAME_PROJECTILES];
    bool missile_warning; float missile_range;
    char combat_log[6][48]; uint8_t combat_log_count;
    /* aim + reverse */
    l3_vec aim_point;
    bool aim_valid, aim_assisted;
    l3_vec turret_aim[GAME_MOUNTS]; /* world direction */
    bool turret_on[GAME_MOUNTS];
} game_view;

bool game_init(void); /* allocates once, validates rules.json, loads commander */
void game_shutdown(void);
const game_view *game_state(void);
void game_update(float dt);        /* seconds, bounded/substepped; no allocation or SD */
void game_render(l3_scene *scene); /* owns begin/end; caller shows scene, then dash.
                                   First call per new scene prepares materials/textures. */
void game_stick(float pitch, float roll);
void game_yaw(float yaw);
void game_thrust(float lateral, float vertical);   /* side and up thrusters, held: -1..1 */
void game_throttle(float fraction);
void game_fire(bool on);
void game_target_next(void);
void game_target_nearest(bool hostile_only);
void game_target_ahead(void);       /* the contact nearest the nose, in a 30 degree cone */
void game_context(void);
void game_panel_open(game_panel panel);
void game_motion_steer(bool on);
bool game_market_buy(int commodity, int amount);
bool game_market_sell(int commodity, int amount);
bool game_equip(int equipment);
bool game_equipment_info(int equipment, game_equipment_offer *out);
uint32_t game_refuel_cost(void);
uint32_t game_repair_cost(void);
bool game_refuel(void);
bool game_repair(void);
bool game_missile(void);
bool game_ecm(void);
bool game_plot_jump(int system);
bool game_save(void);
bool game_load(void);
int game_cargo_used(void);
int game_cargo_capacity(void);
const game_ship_stats *game_ship(game_class type);
const l3_mesh *game_mesh(game_class type);
const char *game_faction(int faction);
uint8_t game_faction_ink(int faction);
const char *game_rank(int rank);
/* Pure deterministic rules, also useful to chart previews and host tests. */
uint32_t game_gps_seed(bool fix, float lat, float lon);
void game_universe(uint32_t seed, game_system *out);
void game_system_contents(uint32_t seed, game_contents *out);
float game_distance(const game_system *a, const game_system *b);
game_class game_aircraft_class(const lap_aircraft_t *a);
int game_aircraft_faction(const lap_aircraft_t *a);
bool game_aircraft_hostile(uint32_t icao, int government);
int game_price(const game_system *system, int good, uint32_t day);
/* part 2 */
bool game_talk_choose(int i);
void game_talk_close(void);
bool game_mission_accept(int i);
game_event game_system_event(uint32_t seed, uint32_t day);
int game_event_price_percent(game_event event, int good);
int game_mining_yield(uint32_t seed, int wifi_count, int *good);
int game_pirate_pack(uint32_t seed, int government);
uint8_t game_combat_rank(uint32_t score);
/* encounters */
void game_set_traffic(int mode); /* 0 MIXED, 1 SIM ONLY, 2 LIVE ONLY; screen stores preference */
void game_set_encounters(int level); /* 0 CALM, 1 NORMAL, 2 BUSY; screen stores preference */
/* lore */
#define MISSION_PASSENGER ((game_mission_kind)4)
#define MISSION_SURVEY ((game_mission_kind)5)
#define MISSION_SALVAGE ((game_mission_kind)6)
#define MISSION_SMUGGLING ((game_mission_kind)7)
#define MISSION_ESCORT ((game_mission_kind)8)
const char *game_good_note(int good); /* <=30 chars, current economy */
int game_passenger_seats(void); /* free seats; separate from cargo */
/* poi */
int game_poi_generate(uint32_t seed, game_poi out[GAME_POIS]);
const char *game_explorer_rank(int rank);
bool game_poi_detectable(l3_vec observer, l3_vec target);
bool game_explore_save(void);
/* cruise */
bool game_cruise(bool on);
float game_cruise_eta(l3_vec target); /* seconds at current factor; normal when off */
/* shipyard */
/* Shark hull IDs retain the existing mesh/save class numbers. */
#define SH_DOGFISH ((game_class)0)
#define SH_HOUNDSHARK ((game_class)1)
#define SH_MAKO ((game_class)2)
#define SH_THRESHER ((game_class)3)
#define SH_WHALE_SHARK ((game_class)4)
#define SH_BASKER ((game_class)5)
#define SH_BLACKTIP ((game_class)6)
#define SH_TIGER ((game_class)7)
#define SH_HAMMERHEAD ((game_class)8)
#define SH_GOBLIN ((game_class)9)
#define SH_COOKIECUTTER ((game_class)10)
typedef struct { game_class ship_class; int32_t price; uint32_t trade_in;
    bool available; float fuel_max; uint8_t seats, pylons, mounts;
    const char *pitch; } game_yard_offer; /* negative price refunds credits */
game_class game_player_class(void);
const game_ship_stats *game_player_ship(void);
int game_shipyard(game_yard_offer *out, int max); /* docked; includes unavailable hulls */
bool game_buy_ship(game_class c); /* refusal reason in messages; fuel bought separately */
uint32_t game_insurance_cost(void);
bool game_rebuy(void); /* destroyed only; insufficient funds provides a free Dogfish */
/* combat */
void game_fire2(bool on);
void game_pips_preset(int preset);
void game_pip(int which); /* +1, taking half-steps from the other bars */
bool game_fit_weapon(int mount, game_weapon_kind kind);
bool game_weapon_info(game_weapon_kind kind, game_equipment_offer *out);
bool game_weapon_group(int mount, int group); /* 0 PRIMARY, 1 SECONDARY */
bool game_rearm(void); /* dockside ammo and consumables, atomic price check */
uint32_t game_rearm_cost(void);
bool game_shield_cell(void);
bool game_boost(void);
bool game_chaff(void);
bool game_heat_sink(void);
void game_subtarget_next(void); /* scanned target only; -1 means hull */
void game_combat_ack(uint32_t events);
/* aim + reverse */
float game_reverse_max(void);
void game_aim_assist(int level); /* 0 off, 1 normal, 2 strong; session only */
/* turrets + weapons hot */
typedef enum { MOUNT_FIXED, MOUNT_GIMBAL, MOUNT_TURRET } game_mount_kind;
const char *game_drive_refusal(void);
bool game_mount_mode(int mount, int mode);
/* Catalog game_weapon_info quotes FIXED; this quotes a fitted mount and its mode. */
bool game_mount_info(int mount, game_equipment_offer *out);
bool game_jump_direction(l3_vec *out);   /* unit world-space direction toward the plotted system; false if none plotted */
float game_jump_alignment(void);         /* degrees between the ship's nose (attitude * (0,0,-1)) and that direction; 180 if none */
#define GAME_JUMP_ALIGN_DEG 8.f          /* a jump starts only with the nose within this */
#define GAME_JUMP_HOLD_DEG 20.f          /* while charging, drifting past this aborts */
#endif
