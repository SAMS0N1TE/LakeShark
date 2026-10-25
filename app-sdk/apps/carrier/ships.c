/* Hulls from bench/tools/carrier_hulls.py: convex parts, mirrored, faces tagged
 * with the scene's shared materials (engine glow, canopy, panels, slot, lamps). */
#include "internal.h"
#include "art.h"
#include "hulls.inc"
const l3_mesh *game_mesh(game_class c) { return c >= 0 && c < SH_COUNT ? &hulls[c] : NULL; }
const l3_mesh *art_mesh(int which) { return which >= 0 && which < ART_MESHES ? &hulls[which] : NULL; }
const game_ship_stats *game_ship(game_class c) {
    return cg && c >= 0 && c < 13 ? &cg->rules.ships[c] : NULL;
}
const char *game_faction(int f) {
    static const char *const names[] = {
        "Independent",   "Aster Guild",  "Blue Meridian", "Copper Union", "Dawn Carriers",
        "Eos Combine",   "Far Reach",    "Glass Line",    "Helix House",  "Ion Traders",
        "Jade Assembly", "Kepler Trust", "Lumen Fleet"};
    return names[(f >= 0 && f < 13) ? f : 0];
}
uint8_t game_faction_ink(int f) { return (uint8_t)(1 + ((f >= 0 ? f : 0) % 7)); }
const char *game_rank(int r) {
    static const char *const rank[] = {"Minnow", "Sprat",    "Pup",  "Reef", "Prowler",
                                       "Hunter", "Predator", "Apex", "Leviathan"};
    return rank[r >= 0 && r < 9 ? r : 0];
}
