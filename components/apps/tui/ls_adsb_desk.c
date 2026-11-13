/* See ls_adsb_desk.h. */
#include "ls_adsb_desk.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

const char LS_ADSBD_CSV_HEAD[] =
    "icao,callsign,altitude_ft,vertical_fpm,speed_kt,heading_deg,lat,lon,range_nm,bearing_deg,squawk,alarm,source,age_s,messages";

/* Has the row anything to sort by in this column? */
static bool has(const ls_adsbd_row_t *r, int col)
{
    switch (col) {
    case LS_AC_CALL: return r->call[0] != 0;
    case LS_AC_ALT: return r->have & LS_AF_ALT;
    case LS_AC_VS: return r->have & LS_AF_VS;
    case LS_AC_SPD: return r->have & LS_AF_SPD;
    case LS_AC_HDG: return r->have & LS_AF_HDG;
    case LS_AC_DIST: return r->nm >= 0;
    case LS_AC_BRG: return r->brg >= 0;
    case LS_AC_SQUAWK: return r->squawk != 0;
    default: return true;
    }
}

static int cmp(const ls_adsbd_row_t *a, const ls_adsbd_row_t *b, int col)
{
#define NUM(x, y) ((x) < (y) ? -1 : (x) > (y))
    switch (col) {
    case LS_AC_CALL: return strcasecmp(a->call, b->call);
    case LS_AC_ICAO: return NUM(a->icao, b->icao);
    case LS_AC_ALT: return NUM(a->alt, b->alt);
    case LS_AC_VS: return NUM(a->vs, b->vs);
    case LS_AC_SPD: return NUM(a->spd, b->spd);
    case LS_AC_HDG: return NUM(a->hdg, b->hdg);
    case LS_AC_DIST: return NUM(a->nm, b->nm);
    case LS_AC_BRG: return NUM(a->brg, b->brg);
    case LS_AC_SQUAWK: return NUM(a->squawk, b->squawk);
    case LS_AC_SRC: return strcmp(a->src, b->src);
    case LS_AC_AGE: return NUM(a->age_s, b->age_s);
    case LS_AC_MSGS: return NUM(b->msgs, a->msgs);          /* ascending is the busiest first */
    default: return 0;
    }
#undef NUM
}

void ls_adsbd_sort(uint8_t *order, const ls_adsbd_row_t *rows, int n, int col, bool desc, bool alarms_first)
{
    for (int i = 0; i < n; i++) order[i] = (uint8_t)i;
    if (col == LS_AC_ALARM) alarms_first = true;
    for (int i = 1; i < n; i++) {
        const uint8_t v = order[i];
        const ls_adsbd_row_t *b = &rows[v];
        int k = i;
        while (k > 0) {
            const ls_adsbd_row_t *a = &rows[order[k - 1]];
            int c;
            if (alarms_first && !a->alarm != !b->alarm) c = a->alarm ? -1 : 1;
            else if (has(a, col) != has(b, col)) c = has(a, col) ? -1 : 1;
            else { c = cmp(a, b, col); if (desc) c = -c; }
            if (c <= 0) break;
            order[k] = order[k - 1];
            k--;
        }
        order[k] = v;
    }
}

void ls_adsbd_cell(const ls_adsbd_row_t *r, int col, char *out, size_t n)
{
    if (!n) return;
    out[0] = 0;
    switch (col) {
    case LS_AC_ALARM: snprintf(out, n, "%s", r->alarm ? "!" : ""); break;
    case LS_AC_CALL: snprintf(out, n, "%s", r->call[0] ? r->call : "-"); break;
    case LS_AC_ICAO: snprintf(out, n, "%06lX", (unsigned long)r->icao); break;
    case LS_AC_ALT: if (r->have & LS_AF_ALT) snprintf(out, n, "%ld%s", (long)r->alt, r->stale & LS_AF_ALT ? "*" : ""); break;
    case LS_AC_VS: if (r->have & LS_AF_VS) snprintf(out, n, "%+ld%s", (long)r->vs, r->stale & LS_AF_VS ? "*" : ""); break;
    case LS_AC_SPD: if (r->have & LS_AF_SPD) snprintf(out, n, "%d%s", r->spd, r->stale & LS_AF_SPD ? "*" : ""); break;
    case LS_AC_HDG: if (r->have & LS_AF_HDG) snprintf(out, n, "%03d%s", r->hdg, r->stale & LS_AF_HDG ? "*" : ""); break;
    case LS_AC_DIST: if (r->nm >= 0) snprintf(out, n, r->nm < 100 ? "%.1f%s" : "%.0f%s", (double)r->nm, r->stale & LS_AF_POS ? "*" : ""); break;
    case LS_AC_BRG: if (r->brg >= 0) snprintf(out, n, "%03d", (int)(r->brg + 0.5f) % 360); break;
    case LS_AC_SQUAWK: if (r->squawk) snprintf(out, n, "%04d", r->squawk); break;
    case LS_AC_SRC: snprintf(out, n, "%s", r->src); break;
    case LS_AC_AGE:
        if (r->age_s < 100) snprintf(out, n, "%lds", (long)(r->age_s < 0 ? 0 : r->age_s));
        else snprintf(out, n, "%ldm", (long)(r->age_s / 60));
        break;
    case LS_AC_MSGS: snprintf(out, n, "%lu", (unsigned long)r->msgs); break;
    default: break;
    }
}

static bool holds(const char *hay, const char *needle)
{
    const size_t n = strlen(needle);
    for (; *hay; hay++)
        if (!strncasecmp(hay, needle, n)) return true;
    return false;
}

bool ls_adsbd_match(const ls_adsbd_row_t *r, const char *needle)
{
    char text[16];
    if (!needle || !*needle) return true;
    if (holds(r->call, needle)) return true;
    snprintf(text, sizeof(text), "%06lX", (unsigned long)r->icao);
    if (holds(text, needle)) return true;
    if (r->squawk) { snprintf(text, sizeof(text), "%04d", r->squawk); if (holds(text, needle)) return true; }
    return r->alarm && holds(r->alarm, needle);
}

int ls_adsbd_csv(const ls_adsbd_row_t *r, char *out, size_t n)
{
    char alt[12] = "", vs[12] = "", spd[8] = "", hdg[8] = "", pos[32] = ",", nm[12] = "", brg[8] = "", sq[8] = "";
    if (r->have & LS_AF_ALT) snprintf(alt, sizeof(alt), "%ld", (long)r->alt);
    if (r->have & LS_AF_VS) snprintf(vs, sizeof(vs), "%ld", (long)r->vs);
    if (r->have & LS_AF_SPD) snprintf(spd, sizeof(spd), "%d", r->spd);
    if (r->have & LS_AF_HDG) snprintf(hdg, sizeof(hdg), "%d", r->hdg);
    if (r->have & LS_AF_POS) snprintf(pos, sizeof(pos), "%.5f,%.5f", (double)r->lat, (double)r->lon);
    if (r->nm >= 0) snprintf(nm, sizeof(nm), "%.1f", (double)r->nm);
    if (r->brg >= 0) snprintf(brg, sizeof(brg), "%d", (int)(r->brg + 0.5f) % 360);
    if (r->squawk) snprintf(sq, sizeof(sq), "%04d", r->squawk);
    return snprintf(out, n, "%06lX,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%ld,%lu", (unsigned long)r->icao, r->call, alt, vs, spd, hdg,
                    pos, nm, brg, sq, r->alarm ? r->alarm : "", r->src, (long)r->age_s, (unsigned long)r->msgs);
}

char ls_adsbd_trend(const ls_adsbd_row_t *r)
{
    if (!(r->have & LS_AF_VS)) return ' ';
    return r->vs > 250 ? '^' : r->vs < -250 ? 'v' : '=';
}
