#ifndef CARTO_GEOM_H
#define CARTO_GEOM_H

#ifdef __cplusplus
extern "C" {
#endif

double carto_lon_to_norm(double lon);
double carto_lat_to_norm(double lat);
void   carto_tile_center(int tx, int ty, int tz, double *lat, double *lon);

#ifdef __cplusplus
}
#endif

#endif
