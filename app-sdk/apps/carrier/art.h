/* CARRIER's look: shared material numbers, inks and meshes the screen side owns.
 * Space is black, hulls are grey metal, the HUD is amber; colour is kept for
 * engines, lasers, stars, planets, explosions and danger. */
#ifndef CARRIER_ART_H
#define CARRIER_ART_H
#include "game.h"

/* The scene makes these first, in this order, so hull faces can name them. */
enum { AM_ENGINE, AM_GLASS, AM_DARK, AM_METAL, AM_SLOT, AM_LAMP, AM_SHARED };

/* Meshes past the ship classes. */
enum { ART_CANISTER = SH_COUNT, ART_MISSILE, ART_DODECA, ART_MESHES };
const l3_mesh *art_mesh(int which);

/* HUD inks (the board palette): amber the instrument colour, as in a modern cockpit. */
#define HUD      LAP_YELLOW                 /* amber */
#define HUD_HI   (LAP_YELLOW | LAP_BRIGHT)  /* bright amber */
#define HUD_DIM  8                          /* dark slate: rails and rings only, never words */
#define HUD_SUB  LAP_WHITE                  /* secondary words: readable grey */
#define HUD_WARN (LAP_RED | LAP_BRIGHT)
#define HUD_COOL (LAP_CYAN | LAP_BRIGHT)    /* shields, friendly */
#define HUD_OK   (LAP_GREEN | LAP_BRIGHT)
#define HUD_LAW  (LAP_BLUE | LAP_BRIGHT)    /* police */
#endif
