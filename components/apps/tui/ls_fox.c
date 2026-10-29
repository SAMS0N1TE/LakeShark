#include "ls_fox.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ls_textpack.h"
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#else
#define EXT_RAM_BSS_ATTR
#endif

/* The fit is ordinary weighted least squares on lines (each bearing's
   perpendicular miss distance is the residual), solved in closed form for
   two unknowns; the ellipse is the eigen-decomposition of the 2x2
   covariance. Standard results, e.g. Gavish & Weiss, "Performance analysis
   of bearing-only target location algorithms", IEEE TAES 28(3), 1992. */

#define M_PER_DEG_LAT 111132.0
#define RANGE_MIN_M   30.0
#define ITERATIONS    6
#define SPREAD_MIN    15.0

typedef struct { double x, y, nx, ny, ux, uy, w0; } line_t;

static double rad(double d) { return d * M_PI / 180.0; }

static double axis_gap(double a, double b)
{
    /* Two lines differ by this much, ignoring which way each points. */
    double d = fmod(fabs(a - b), 180.0);
    return d > 90 ? 180 - d : d;
}

bool ls_fox_fix(const ls_df_bearing_t *b, int n, ls_fox_fix_t *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!b || n < 2) return false;
    if (n > LS_FOX_MAX) { b += n - LS_FOX_MAX; n = LS_FOX_MAX; }

    double lat0 = 0, lon0 = 0;
    int placed = 0;
    for (int i = 0; i < n; i++)
        if (isfinite(b[i].lat) && isfinite(b[i].lon) && isfinite(b[i].bearing)) { lat0 += b[i].lat; lon0 += b[i].lon; placed++; }
    if (placed < 2) return false;
    lat0 /= placed; lon0 /= placed;
    const double mx = M_PER_DEG_LAT * cos(rad(lat0));

    /* Work area, static: 1.3 KB of lines was most of this frame, and the
       screen reaches here on the UI task. Not reentrant; its one caller on
       the board (ls_fox_run.c refit) holds the hunt's lock. */
    EXT_RAM_BSS_ATTR static line_t L[LS_FOX_MAX];
    static bool use[LS_FOX_MAX];
    int m = 0;
    for (int i = 0; i < n; i++) {
        if (!isfinite(b[i].lat) || !isfinite(b[i].lon) || !isfinite(b[i].bearing)) continue;
        const double t = rad(b[i].bearing);
        double sigma = (isfinite(b[i].spread) ? b[i].spread : 10.0) / 2.0;
        if (sigma < 1.0) sigma = 1.0;
        L[m] = (line_t){ .x = (b[i].lon - lon0) * mx, .y = (b[i].lat - lat0) * M_PER_DEG_LAT,
                         .ux = sin(t), .uy = cos(t), .nx = cos(t), .ny = -sin(t), .w0 = 1.0 / (rad(sigma) * rad(sigma)) };
        use[m] = true;
        m++;
    }

    double px = 0, py = 0, A[3] = { 0 };
    int used = 0;
    for (int round = 0; round < 3; round++) {
        /* Enough angle between the lines in use for a crossing at all. */
        double widest = 0;
        for (int i = 0; i < m; i++) for (int j = i + 1; j < m; j++)
            if (use[i] && use[j]) {
                const double g = axis_gap(atan2(L[i].ux, L[i].uy) * 180 / M_PI, atan2(L[j].ux, L[j].uy) * 180 / M_PI);
                if (g > widest) widest = g;
            }
        if (widest < SPREAD_MIN) return false;
        bool have = false;
        for (int it = 0; it < ITERATIONS; it++) {
            double a = 0, bb = 0, c = 0, r0 = 0, r1 = 0;
            used = 0;
            for (int i = 0; i < m; i++) {
                if (!use[i]) continue;
                double w = L[i].w0;
                if (have) {
                    double r = hypot(px - L[i].x, py - L[i].y);
                    if (r < RANGE_MIN_M) r = RANGE_MIN_M;
                    w /= r * r;
                } else w /= 1e6;                     /* a kilometre, to start */
                const double d = L[i].nx * L[i].x + L[i].ny * L[i].y;
                a += w * L[i].nx * L[i].nx; bb += w * L[i].nx * L[i].ny; c += w * L[i].ny * L[i].ny;
                r0 += w * L[i].nx * d; r1 += w * L[i].ny * d;
                used++;
            }
            const double det = a * c - bb * bb;
            if (used < 2 || !(fabs(det) > 1e-30)) return false;
            px = (c * r0 - bb * r1) / det;
            py = (a * r1 - bb * r0) / det;
            /* The covariance is the inverse of the normal matrix. */
            A[0] = c / det; A[1] = -bb / det; A[2] = a / det;
            have = true;
        }
        /* Lines whose transmitter would be behind them go, then refit. */
        int behind = 0;
        for (int i = 0; i < m; i++)
            if (use[i] && (px - L[i].x) * L[i].ux + (py - L[i].y) * L[i].uy < -RANGE_MIN_M) { use[i] = false; behind++; }
        out->behind += behind;
        if (!behind) break;
        int left = 0;
        for (int i = 0; i < m; i++) left += use[i];
        if (left < 2) return false;
    }

    /* How well the lines agree, to scale the covariance: a priori with two
       lines (they always cross exactly), measured with more. */
    double chi = 0, miss = 0;
    for (int i = 0; i < m; i++) {
        if (!use[i]) continue;
        const double d = L[i].nx * (px - L[i].x) + L[i].ny * (py - L[i].y);
        double r = hypot(px - L[i].x, py - L[i].y);
        if (r < RANGE_MIN_M) r = RANGE_MIN_M;
        chi += d * d * L[i].w0 / (r * r);
        miss += d * d;
    }
    const double scale = used > 2 ? fmax(1.0, chi / (used - 2)) : 1.0;
    const double ca = A[0] * scale, cb = A[1] * scale, cc = A[2] * scale;
    const double mid = (ca + cc) / 2, rr = sqrt((ca - cc) * (ca - cc) / 4 + cb * cb);
    const double l1 = mid + rr, l2 = fmax(0.0, mid - rr);
    const double phi = 0.5 * atan2(2 * cb, ca - cc);      /* from east, anticlockwise */
    double brg = 90.0 - phi * 180 / M_PI;
    brg = fmod(fmod(brg, 180.0) + 180.0, 180.0);

    out->valid = true;
    out->lat = lat0 + py / M_PER_DEG_LAT;
    out->lon = lon0 + px / mx;
    out->major_m = (float)sqrt(l1);
    out->minor_m = (float)sqrt(l2);
    out->major_deg = (float)brg;
    out->rms_m = (float)sqrt(miss / used);
    out->used = used;
    /* From the newest place a bearing was taken. */
    for (int i = m - 1; i >= 0; i--) {
        const double dx = px - L[i].x, dy = py - L[i].y;
        out->range_m = (float)hypot(dx, dy);
        out->bearing = (float)fmod(atan2(dx, dy) * 180 / M_PI + 360.0, 360.0);
        break;
    }
    return isfinite(out->lat) && isfinite(out->lon) && fabs(out->lat) <= 90;
}

void ls_fox_project(double lat, double lon, float bearing, double metres, double *lat2, double *lon2)
{
    const double t = rad(bearing);
    const double mx = M_PER_DEG_LAT * cos(rad(lat));
    *lat2 = lat + metres * cos(t) / M_PER_DEG_LAT;
    *lon2 = lon + metres * sin(t) / (mx > 1 ? mx : 1);
}

/* ------------------------------------------------------------------ CSV -- */

int ls_fox_csv_header(char *out, size_t cap)
{
    const int n = LS_SNPUTS(t_ls_fox_utc_s_lat_lon, out, cap, "utc_s,lat,lon,bearing_deg,spread_deg,level,source\n");
    return n < 0 ? 0 : (size_t)n >= cap ? (int)cap - 1 : n;
}

int ls_fox_csv_line(const ls_df_bearing_t *b, uint32_t utc_s, char *out, size_t cap)
{
    if (!b || !out || !cap) return 0;
    char src[sizeof(b->source)];
    snprintf(src, sizeof(src), "%s", b->source);
    for (char *c = src; *c; c++) if (*c == ',' || *c == '\n' || *c == '\r') *c = ' ';
    const int n = LS_SNPRINTF(t_ls_fox_lu_7f_7f_1f, out, cap, "%lu,%.7f,%.7f,%.1f,%.1f,%.1f,%s\n", (unsigned long)utc_s, b->lat, b->lon,
                           b->bearing, b->spread, isfinite(b->level) ? b->level : 0.0f, src);
    return n < 0 ? 0 : (size_t)n >= cap ? (int)cap - 1 : n;
}

bool ls_fox_csv_parse(const char *line, ls_df_bearing_t *b, uint32_t *utc_s)
{
    if (!line || !b || *line == '#' || !strncmp(line, "utc_s", 5)) return false;
    memset(b, 0, sizeof(*b));
    unsigned long utc = 0;
    double lat, lon; float brg, spread, level;
    int used = 0;
    if (sscanf(line, "%lu,%lf,%lf,%f,%f,%f,%n", &utc, &lat, &lon, &brg, &spread, &level, &used) != 6 || !used) return false;
    if (fabs(lat) > 90 || fabs(lon) > 180 || !isfinite(brg) || brg < 0 || brg >= 360.5f || !(spread >= 0)) return false;
    b->lat = lat; b->lon = lon; b->bearing = brg; b->spread = spread; b->level = level;
    snprintf(b->source, sizeof(b->source), "%s", line + used);
    for (char *c = b->source; *c; c++) if (*c == '\n' || *c == '\r') { *c = 0; break; }
    if (utc_s) *utc_s = (uint32_t)utc;
    return true;
}

/* ----------------------------------------------------------------- hunt -- */

void ls_fox_hunt_clear(ls_fox_hunt_t *h) { if (h) { const uint32_t s = h->serial; memset(h, 0, sizeof(*h)); h->serial = s + 1; } }

void ls_fox_hunt_add(ls_fox_hunt_t *h, const ls_df_bearing_t *b, uint32_t utc_s)
{
    if (!h || !b) return;
    if (h->n == LS_FOX_MAX) {
        memmove(h->b, h->b + 1, sizeof(h->b[0]) * (LS_FOX_MAX - 1));
        memmove(h->utc, h->utc + 1, sizeof(h->utc[0]) * (LS_FOX_MAX - 1));
        h->n--;
    }
    h->b[h->n] = *b; h->utc[h->n] = utc_s; h->n++;
    h->serial++;
}

bool ls_fox_hunt_drop_last(ls_fox_hunt_t *h)
{
    if (!h || !h->n) return false;
    h->n--; h->serial++;
    return true;
}
