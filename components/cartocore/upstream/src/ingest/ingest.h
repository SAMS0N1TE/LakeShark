#ifndef CC_INGEST_H
#define CC_INGEST_H
#include "cartocore/core.h"
int cc_inflate(const unsigned char *src, size_t size, unsigned char *dst, size_t capacity, size_t *written);
/* Streaming inflate: caller supplies a 32 KiB history window. */
int cc_inflate_stream(int (*read_byte)(void *),void *input,size_t size,
    int (*emit)(void *,unsigned char),void *output,unsigned char *history,
    size_t capacity,size_t *written);
extern int cc_building_min_zoom,cc_build_overview;
extern int cc_build_profile,cc_phase_min,cc_phase_max,cc_phase_codec,cc_coord_codec;
int cc_build(const char *pbf, const double bbox[4], int zmin, int zmax, const char *out);
int cc_map_cli(int argc, char **argv);
int cc_tui_cli(int argc,char **argv);
int cc_places_cli(int argc,char **argv);
#endif
