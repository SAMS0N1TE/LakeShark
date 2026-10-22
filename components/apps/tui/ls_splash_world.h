/* Country outlines for the boot splash globe: closed rings of unit vectors
   (x east at 0 lon, y north, z out through 0 lat 0 lon), scaled by 32767.
   Ring i is points start[i] .. start[i+1]-1. */
#ifndef LS_SPLASH_WORLD_H
#define LS_SPLASH_WORLD_H
#include <stdint.h>

extern const uint16_t ls_splash_world_rings;
extern const uint16_t ls_splash_world_start[];
extern const int16_t  ls_splash_world_xyz[][3];

#endif
