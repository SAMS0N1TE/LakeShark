#include "ls_wmm.h"

#include <math.h>
#include <string.h>
#include "esp_attr.h"

typedef struct { signed char n, m; float g, h, gd, hd; } wmm_coef_t;

/* WMM2025 (NOAA NCEI, public domain), epoch 2025.0, valid to 2030.0:
   n, m, g, h (nT) and their secular variation (nT/year). */
static const wmm_coef_t WMM2025[] = {
    {1, 0, -29351.8, 0.0, 12.0, 0.0},
    {1, 1, -1410.8, 4545.4, 9.7, -21.5},
    {2, 0, -2556.6, 0.0, -11.6, 0.0},
    {2, 1, 2951.1, -3133.6, -5.2, -27.7},
    {2, 2, 1649.3, -815.1, -8.0, -12.1},
    {3, 0, 1361.0, 0.0, -1.3, 0.0},
    {3, 1, -2404.1, -56.6, -4.2, 4.0},
    {3, 2, 1243.8, 237.5, 0.4, -0.3},
    {3, 3, 453.6, -549.5, -15.6, -4.1},
    {4, 0, 895.0, 0.0, -1.6, 0.0},
    {4, 1, 799.5, 278.6, -2.4, -1.1},
    {4, 2, 55.7, -133.9, -6.0, 4.1},
    {4, 3, -281.1, 212.0, 5.6, 1.6},
    {4, 4, 12.1, -375.6, -7.0, -4.4},
    {5, 0, -233.2, 0.0, 0.6, 0.0},
    {5, 1, 368.9, 45.4, 1.4, -0.5},
    {5, 2, 187.2, 220.2, 0.0, 2.2},
    {5, 3, -138.7, -122.9, 0.6, 0.4},
    {5, 4, -142.0, 43.0, 2.2, 1.7},
    {5, 5, 20.9, 106.1, 0.9, 1.9},
    {6, 0, 64.4, 0.0, -0.2, 0.0},
    {6, 1, 63.8, -18.4, -0.4, 0.3},
    {6, 2, 76.9, 16.8, 0.9, -1.6},
    {6, 3, -115.7, 48.8, 1.2, -0.4},
    {6, 4, -40.9, -59.8, -0.9, 0.9},
    {6, 5, 14.9, 10.9, 0.3, 0.7},
    {6, 6, -60.7, 72.7, 0.9, 0.9},
    {7, 0, 79.5, 0.0, -0.0, 0.0},
    {7, 1, -77.0, -48.9, -0.1, 0.6},
    {7, 2, -8.8, -14.4, -0.1, 0.5},
    {7, 3, 59.3, -1.0, 0.5, -0.8},
    {7, 4, 15.8, 23.4, -0.1, 0.0},
    {7, 5, 2.5, -7.4, -0.8, -1.0},
    {7, 6, -11.1, -25.1, -0.8, 0.6},
    {7, 7, 14.2, -2.3, 0.8, -0.2},
    {8, 0, 23.2, 0.0, -0.1, 0.0},
    {8, 1, 10.8, 7.1, 0.2, -0.2},
    {8, 2, -17.5, -12.6, 0.0, 0.5},
    {8, 3, 2.0, 11.4, 0.5, -0.4},
    {8, 4, -21.7, -9.7, -0.1, 0.4},
    {8, 5, 16.9, 12.7, 0.3, -0.5},
    {8, 6, 15.0, 0.7, 0.2, -0.6},
    {8, 7, -16.8, -5.2, -0.0, 0.3},
    {8, 8, 0.9, 3.9, 0.2, 0.2},
    {9, 0, 4.6, 0.0, -0.0, 0.0},
    {9, 1, 7.8, -24.8, -0.1, -0.3},
    {9, 2, 3.0, 12.2, 0.1, 0.3},
    {9, 3, -0.2, 8.3, 0.3, -0.3},
    {9, 4, -2.5, -3.3, -0.3, 0.3},
    {9, 5, -13.1, -5.2, 0.0, 0.2},
    {9, 6, 2.4, 7.2, 0.3, -0.1},
    {9, 7, 8.6, -0.6, -0.1, -0.2},
    {9, 8, -8.7, 0.8, 0.1, 0.4},
    {9, 9, -12.9, 10.0, -0.1, 0.1},
    {10, 0, -1.3, 0.0, 0.1, 0.0},
    {10, 1, -6.4, 3.3, 0.0, 0.0},
    {10, 2, 0.2, 0.0, 0.1, -0.0},
    {10, 3, 2.0, 2.4, 0.1, -0.2},
    {10, 4, -1.0, 5.3, -0.0, 0.1},
    {10, 5, -0.6, -9.1, -0.3, -0.1},
    {10, 6, -0.9, 0.4, 0.0, 0.1},
    {10, 7, 1.5, -4.2, -0.1, 0.0},
    {10, 8, 0.9, -3.8, -0.1, -0.1},
    {10, 9, -2.7, 0.9, -0.0, 0.2},
    {10, 10, -3.9, -9.1, -0.0, -0.0},
    {11, 0, 2.9, 0.0, 0.0, 0.0},
    {11, 1, -1.5, 0.0, -0.0, -0.0},
    {11, 2, -2.5, 2.9, 0.0, 0.1},
    {11, 3, 2.4, -0.6, 0.0, -0.0},
    {11, 4, -0.6, 0.2, 0.0, 0.1},
    {11, 5, -0.1, 0.5, -0.1, -0.0},
    {11, 6, -0.6, -0.3, 0.0, -0.0},
    {11, 7, -0.1, -1.2, -0.0, 0.1},
    {11, 8, 1.1, -1.7, -0.1, -0.0},
    {11, 9, -1.0, -2.9, -0.1, 0.0},
    {11, 10, -0.2, -1.8, -0.1, 0.0},
    {11, 11, 2.6, -2.3, -0.1, 0.0},
    {12, 0, -2.0, 0.0, 0.0, 0.0},
    {12, 1, -0.2, -1.3, 0.0, -0.0},
    {12, 2, 0.3, 0.7, -0.0, 0.0},
    {12, 3, 1.2, 1.0, -0.0, -0.1},
    {12, 4, -1.3, -1.4, -0.0, 0.1},
    {12, 5, 0.6, -0.0, -0.0, -0.0},
    {12, 6, 0.6, 0.6, 0.1, -0.0},
    {12, 7, 0.5, -0.1, -0.0, -0.0},
    {12, 8, -0.1, 0.8, 0.0, 0.0},
    {12, 9, -0.4, 0.1, 0.0, -0.0},
    {12, 10, -0.2, -1.0, -0.1, -0.0},
    {12, 11, -1.3, 0.1, -0.0, 0.0},
    {12, 12, -0.7, 0.2, -0.1, -0.1},
};

#define N_MAX 12
#define EPOCH 2025.0

double ls_wmm_year(int year, int month, int day)
{
    static const int before[] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
    if (month < 1 || month > 12) month = 1;
    const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    const int days = leap ? 366 : 365;
    int doy = before[month - 1] + day - 1 + (leap && month > 2);
    return year + (double)doy / days;
}

/* Synthesis as in the WMM technical report (NOAA NCEI, 2025), section 1.2:
   geodetic to geocentric, Schmidt semi-normalised Legendre functions by the
   usual recursion, the field in geocentric north/east/down, then rotated
   back to the ellipsoid. */
bool ls_wmm_field(double lat, double lon, double height_m, double year, ls_wmm_field_t *out)
{
    if (!out || !isfinite(lat) || !isfinite(lon) || year < EPOCH || year > EPOCH + 5.0) return false;
    const double DEG = M_PI / 180.0;
    const double a = 6378.137, f = 1 / 298.257223563, e2 = f * (2 - f), re = 6371.2;
    const double h = height_m / 1000.0, dt = year - EPOCH;
    const double phi = lat * DEG, lam = lon * DEG;
    const double sp = sin(phi), cp = cos(phi);
    const double rc = a / sqrt(1 - e2 * sp * sp);
    const double p = (rc + h) * cp, z = (rc * (1 - e2) + h) * sp;
    const double r = sqrt(p * p + z * z);
    const double phic = asin(z / r);
    /* Colatitude terms of the geocentric position. */
    const double ct = sin(phic), st = cos(phic);

    /* 6.7 KB of tables: more than the whole 6 KB TUI stack this runs on,
       which it overflowed the first time COMPASS had a position. Static in
       PSRAM instead; the one caller is ls_compass_live on the TUI task, so
       nothing runs this twice at once. */
    EXT_RAM_BSS_ATTR static double g[N_MAX + 1][N_MAX + 1], hh[N_MAX + 1][N_MAX + 1];
    memset(g, 0, sizeof(g)); memset(hh, 0, sizeof(hh));
    for (unsigned i = 0; i < sizeof(WMM2025) / sizeof(WMM2025[0]); i++) {
        const wmm_coef_t *c = &WMM2025[i];
        g[c->n][c->m] = c->g + dt * c->gd;
        hh[c->n][c->m] = c->h + dt * c->hd;
    }

    /* Gauss-normalised P and dP/dtheta, then the Schmidt factors. */
    EXT_RAM_BSS_ATTR static double P[N_MAX + 1][N_MAX + 1], dP[N_MAX + 1][N_MAX + 1], S[N_MAX + 1][N_MAX + 1];
    memset(P, 0, sizeof(P)); memset(dP, 0, sizeof(dP));
    P[0][0] = 1; dP[0][0] = 0; S[0][0] = 1;
    for (int n = 1; n <= N_MAX; n++) {
        S[n][0] = S[n - 1][0] * (2.0 * n - 1) / n;
        for (int m = 1; m <= n; m++)
            S[n][m] = S[n][m - 1] * sqrt((double)(n - m + 1) * (m == 1 ? 2 : 1) / (n + m));
        for (int m = 0; m <= n; m++) {
            if (n == m) {
                P[n][m] = st * P[n - 1][m - 1];
                dP[n][m] = st * dP[n - 1][m - 1] + ct * P[n - 1][m - 1];
            } else if (n == 1) {
                P[n][m] = ct * P[n - 1][m];
                dP[n][m] = ct * dP[n - 1][m] - st * P[n - 1][m];
            } else {
                const double k = ((double)(n - 1) * (n - 1) - (double)m * m) / ((2.0 * n - 1) * (2.0 * n - 3));
                P[n][m] = ct * P[n - 1][m] - k * P[n - 2][m];
                dP[n][m] = ct * dP[n - 1][m] - st * P[n - 1][m] - k * dP[n - 2][m];
            }
        }
    }

    double bt = 0, bp = 0, br = 0;
    for (int n = 1; n <= N_MAX; n++) {
        const double ar = pow(re / r, n + 2);
        for (int m = 0; m <= n; m++) {
            const double gs = g[n][m] * S[n][m], hs = hh[n][m] * S[n][m];
            const double cm = cos(m * lam), sm = sin(m * lam);
            const double t = gs * cm + hs * sm;
            br += ar * (n + 1) * t * P[n][m];
            bt -= ar * t * dP[n][m];
            if (st > 1e-10) bp += ar * m * (gs * sm - hs * cm) * P[n][m] / st;
        }
    }
    /* Geocentric north, east, down. */
    const double xg = -bt, yg = bp, zg = -br;
    const double psi = phic - phi;
    const double x = xg * cos(psi) - zg * sin(psi);
    const double zz = xg * sin(psi) + zg * cos(psi);
    out->north_nt = x; out->east_nt = yg; out->down_nt = zz;
    out->horizontal_nt = sqrt(x * x + yg * yg);
    out->total_nt = sqrt(x * x + yg * yg + zz * zz);
    out->declination = atan2(yg, x) / DEG;
    out->inclination = atan2(zz, out->horizontal_nt) / DEG;
    return true;
}
