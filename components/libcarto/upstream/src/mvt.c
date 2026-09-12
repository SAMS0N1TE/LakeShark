#include "mvt.h"
#include <string.h>
#include <math.h>

#define MVT_MAX_VALUES 4096

/*LS-1050  The value tables are the caller's, not this frame's.

   They were two 4096 entry arrays inside mvt_ctx, and mvt_ctx is a local of
   carto_mvt_render_category - a thirty-two kilobyte stack frame, built once
   per layer category and five times per tile. On a host that is invisible.
   On a 6 KB embedded task stack it is a stack protection fault in the
   function prologue, every time, which is exactly what the board did.

   The caller allocates them from its arena, which is PSRAM and already sized
   for a frame, and passes them in. The frame is about a hundred bytes now. */
typedef struct {
    carto_framebuffer *fb;
    const carto_style *style;
    /*LS-1073  Single precision, because this board's FPU is single precision.

       These four are the per-POINT transform: every coordinate of every
       geometry in every tile goes through `ox + cx * per_ext`. As doubles
       that is a software-emulated multiply and add per point on an ESP32-P4,
       whose FPU does float in hardware and double not at all. Thousands of
       points a tile, twelve tiles a pan.

       Precision is not the constraint here and it is worth saying why. `ox`
       is the tile's top left in FRAME coordinates - a few hundred pixels -
       not the world origin, which is hundreds of thousands. The large
       subtraction that produces it stays in double in carto_render_tile,
       where cancellation would matter; what arrives here is small. A float
       holds about seven significant digits, so a coordinate in the hundreds
       carries roughly a thousandth of a pixel, and the result is cast to int
       anyway. */
    float ox, oy, tile_px, per_ext;
    carto_layer_kind category;
    carto_ipt *scratch;
    int scratch_cap;
    double road_scale;
    int min_road_prio;

    const uint8_t **val_ptr;
    int            *val_len;
    int            *val_num;      /* the numeric form, 0 when it was a string */
    int             val_cap;
    int             nvals;
    /*LS-1055  Place names are handed out, not drawn. See mvt.h. */
    carto_label_sink *labels;
    int               name_key_idx;
    int               minzoom_key_idx;
    int               rank_key_idx;
    int            class_key_idx;
} mvt_ctx;

static uint64_t rvarint(const uint8_t *b, size_t len, size_t *pos) {
    uint64_t r = 0;
    int s = 0;
    while (*pos < len) {
        uint8_t c = b[(*pos)++];
        r |= (uint64_t)(c & 0x7f) << s;
        if (!(c & 0x80)) break;
        s += 7;
        if (s > 63) break;
    }
    return r;
}

static int32_t zigzag(uint32_t n) {
    return (int32_t)((n >> 1) ^ (0u - (n & 1)));
}

/* The plain name, not one of the forty localisations beside it. A tile
   carries name, name:en, name:ja and so on; matching a prefix would take
   whichever came first in the key table, which is arbitrary. */
static int key_is_name(const uint8_t *p, int len) {
    return len == 4 && memcmp(p, "name", 4) == 0;
}

static int key_is_minzoom(const uint8_t *p, int len) {
    return len == 8 && memcmp(p, "min_zoom", 8) == 0;
}

static int key_is_rank(const uint8_t *p, int len) {
    return len == 15 && memcmp(p, "population_rank", 15) == 0;
}

static int key_is_class(const uint8_t *p, int len) {
    return (len == 4 && memcmp(p, "kind", 4) == 0)
        || (len == 5 && memcmp(p, "class", 5) == 0)
        || (len == 9 && memcmp(p, "pmap:kind", 9) == 0);
}

static int is_park_kind(const char *k) {
    static const char *parks[] = {
        "park", "wood", "forest", "grass", "playground", "garden",
        "nature_reserve", "meadow", "recreation_ground", "cemetery",
        "allotments", "golf_course", "pitch", "village_green",
    };
    for (size_t i = 0; i < sizeof(parks) / sizeof(parks[0]); ++i)
        if (strcmp(k, parks[i]) == 0) return 1;
    return 0;
}

/*LS-1056  A tag value is a string OR a number, and both were needed.

   This kept only the string and dropped every numeric field on the floor,
   which was invisible while the only tag anybody read was the class - that
   is always a string. min_zoom and population_rank are numbers, so the
   de-clutter that decides which place names a view is big enough for was
   reading zero for all of them and drawing every name at every zoom. A view
   of one county had "United States" written across it.

   The MVT Value message is a union: field 1 string, 2 float, 3 double,
   4 int, 5 uint, 6 sint, 7 bool. Numbers come back as an int because that is
   what these tags are; a float that is really 2.5 rounds, which is fine for
   a zoom threshold and would not be for a coordinate. */
static void parse_value(const uint8_t *b, size_t len,
                        const uint8_t **sptr, int *slen, int *num) {
    *sptr = NULL;
    *slen = 0;
    if (num) *num = 0;
    size_t p = 0;
    while (p < len) {
        uint64_t key = rvarint(b, len, &p);
        uint32_t fn = (uint32_t)(key >> 3), wt = (uint32_t)(key & 7);
        if (wt == 2) {
            uint64_t l = rvarint(b, len, &p);
            if (fn == 1) { *sptr = b + p; *slen = (int)l; }
            p += (size_t)l;
        } else if (wt == 0) {
            uint64_t v = rvarint(b, len, &p);
            if (!num) continue;
            if (fn == 4 || fn == 5 || fn == 7) *num = (int)v;
            else if (fn == 6) *num = (int)zigzag((uint32_t)v);   /* sint */
        } else if (wt == 5) {
            if (num && fn == 2) {
                float f;
                memcpy(&f, b + p, sizeof f);
                *num = (int)(f + 0.5f);
            }
            p += 4;
        } else if (wt == 1) {
            if (num && fn == 3) {
                double d;
                memcpy(&d, b + p, sizeof d);
                *num = (int)(d + 0.5);
            }
            p += 8;
        } else break;
    }
}

static void feature_colors(const mvt_ctx *m, int prio, carto_rgb *fill,
                           carto_rgb *line, carto_rgb *pt, int *lw) {
    const carto_style *s = m->style;
    *lw = 1;
    switch (m->category) {
        case CARTO_LAYER_WATER:    *fill = *line = *pt = s->water; break;
        case CARTO_LAYER_LANDUSE:  *fill = *line = *pt = s->park; break;
        case CARTO_LAYER_BUILDING: *fill = *line = *pt = s->building; break;
        case CARTO_LAYER_ROAD: {
            if (prio < CARTO_ROAD_PRIO_MIN) prio = CARTO_ROAD_PRIO_MIN;
            if (prio > CARTO_ROAD_PRIO_MAX) prio = CARTO_ROAD_PRIO_MAX;
            *fill = *line = *pt = s->road_color_by_prio[prio];
            int w = (int)(s->road_width[prio] * m->road_scale + 0.5);
            *lw = w < 1 ? 1 : w;
            break;
        }
        default: *fill = *line = *pt = s->label_color; break;
    }
}

static void geom_bbox_ext(const uint8_t *g, size_t glen, int *w, int *h) {
    size_t p = 0;
    int cx = 0, cy = 0, minx = 0, miny = 0, maxx = 0, maxy = 0, have = 0;
    while (p < glen) {
        uint64_t cmd = rvarint(g, glen, &p);
        uint32_t id = (uint32_t)(cmd & 7), count = (uint32_t)(cmd >> 3);
        if (id == 1 || id == 2) {
            for (uint32_t k = 0; k < count; ++k) {
                cx += zigzag((uint32_t)rvarint(g, glen, &p));
                cy += zigzag((uint32_t)rvarint(g, glen, &p));
                if (!have) { minx = maxx = cx; miny = maxy = cy; have = 1; }
                else {
                    if (cx < minx) minx = cx;
                    if (cx > maxx) maxx = cx;
                    if (cy < miny) miny = cy;
                    if (cy > maxy) maxy = cy;
                }
            }
        }

    }
    *w = have ? (maxx - minx) : 0;
    *h = have ? (maxy - miny) : 0;
}

int g_mvt_scratch_peak;

static void render_feature(mvt_ctx *m, const uint8_t *g, size_t glen,
                           int geomtype, int prio) {
    carto_rgb fillc, linec, ptc;
    int lw;
    feature_colors(m, prio, &fillc, &linec, &ptc, &lw);

    if ((geomtype == 2 || geomtype == 3) && m->per_ext > 0.0) {
        int bw, bh;
        geom_bbox_ext(g, glen, &bw, &bh);
        float thresh = (geomtype == 2 ? CARTO_MIN_LINE_PX : CARTO_MIN_POLY_PX)
                        / m->per_ext;
        if ((float)bw < thresh && (float)bh < thresh) return;
    }

    size_t p = 0;
    int cx = 0, cy = 0, n = 0;
    carto_ipt *pts = m->scratch;
    /*LS-1067  The largest ring this scratch has ever had to hold.

       LS DEVIATION 3 sizes the scratch at 65536 points - 512 KB - and says
       so rather than justifying it, with the honest note that trimming it
       blind would show up as missing roads rather than as an error. This is
       what turns the guess into a measurement: recorded where a ring is
       finished, which is the only moment n is final, so it costs one compare
       per ring rather than one per point. */
    #define SCRATCH_MARK() do { if (n > g_mvt_scratch_peak) \
                                    g_mvt_scratch_peak = n; } while (0)

    while (p < glen) {
        uint64_t cmdint = rvarint(g, glen, &p);
        uint32_t id = (uint32_t)(cmdint & 7);
        uint32_t count = (uint32_t)(cmdint >> 3);

        if (id == 1) {
            SCRATCH_MARK();
            if (geomtype == 2 && n >= 2) carto_polyline(m->fb, pts, n, lw, linec);
            n = 0;
            for (uint32_t k = 0; k < count; ++k) {
                cx += zigzag((uint32_t)rvarint(g, glen, &p));
                cy += zigzag((uint32_t)rvarint(g, glen, &p));
                int fx = (int)(m->ox + cx * m->per_ext);
                int fy = (int)(m->oy + cy * m->per_ext);
                if (geomtype == 1) {
                    carto_fill_rect(m->fb, fx - 1, fy - 1, 3, 3, ptc);
                } else if (n < m->scratch_cap) {
                    pts[n].x = fx; pts[n].y = fy; ++n;
                }
            }
        } else if (id == 2) {
            for (uint32_t k = 0; k < count; ++k) {
                cx += zigzag((uint32_t)rvarint(g, glen, &p));
                cy += zigzag((uint32_t)rvarint(g, glen, &p));
                if (n < m->scratch_cap) {
                    pts[n].x = (int)(m->ox + cx * m->per_ext);
                    pts[n].y = (int)(m->oy + cy * m->per_ext);
                    ++n;
                }
            }
        } else if (id == 7) {
            SCRATCH_MARK();
            if (geomtype == 3 && n >= 3) carto_fill_polygon(m->fb, pts, n, fillc);
            n = 0;
        }
    }
    SCRATCH_MARK();
    if (geomtype == 2 && n >= 2) carto_polyline(m->fb, pts, n, lw, linec);
    #undef SCRATCH_MARK
}

static void feature_class(mvt_ctx *m, const uint8_t *tags, size_t taglen,
                          char *out, int outcap) {
    out[0] = 0;
    if (m->class_key_idx < 0 || !tags) return;
    size_t tp = 0;
    while (tp < taglen) {
        uint32_t ki = (uint32_t)rvarint(tags, taglen, &tp);
        if (tp >= taglen) break;
        uint32_t vi = (uint32_t)rvarint(tags, taglen, &tp);
        if ((int)ki == m->class_key_idx && (int)vi < m->nvals
                && m->val_len[vi] > 0) {
            int cl = m->val_len[vi];
            if (cl > outcap - 1) cl = outcap - 1;
            memcpy(out, m->val_ptr[vi], (size_t)cl);
            out[cl] = 0;
            return;
        }
    }
}

/*LS-1055  A tag's value by key index: text into `out`, or a number.

   feature_class already did this for one key. Places need three more - the
   name, the zoom it becomes worth showing at, and how big the place is - and
   walking the tag list once for all of them beats walking it four times. */
static void feature_tags(mvt_ctx *m, const uint8_t *tags, size_t taglen,
                         char *name, int namecap, int *minzoom, int *rank)
{
    name[0] = 0;
    *minzoom = 0;
    *rank = 0;
    if (!tags) return;

    size_t tp = 0;
    while (tp < taglen) {
        uint32_t ki = (uint32_t)rvarint(tags, taglen, &tp);
        if (tp >= taglen) break;
        uint32_t vi = (uint32_t)rvarint(tags, taglen, &tp);
        if ((int)vi >= m->nvals) continue;

        if ((int)ki == m->name_key_idx && m->val_len[vi] > 0 && !name[0]) {
            int cl = m->val_len[vi];
            if (cl > namecap - 1) cl = namecap - 1;
            memcpy(name, m->val_ptr[vi], (size_t)cl);
            name[cl] = 0;
        } else if ((int)ki == m->minzoom_key_idx) {
            *minzoom = m->val_num[vi];
        } else if ((int)ki == m->rank_key_idx) {
            *rank = m->val_num[vi];
        }
    }
}

/* The first point of a feature's geometry, in frame pixels. A place is a
   point in these tiles; anything else is skipped rather than guessed at. */
static int feature_point(mvt_ctx *m, const uint8_t *g, size_t glen,
                         int *out_x, int *out_y)
{
    size_t p = 0;
    int32_t cx = 0, cy = 0;
    while (p < glen) {
        uint32_t cmd = (uint32_t)rvarint(g, glen, &p);
        const uint32_t id = cmd & 7, count = cmd >> 3;
        if (id != 1) return 0;                    /* not a MoveTo: not a point */
        if (count < 1) return 0;
        cx += zigzag((uint32_t)rvarint(g, glen, &p));
        cy += zigzag((uint32_t)rvarint(g, glen, &p));
        *out_x = (int)(m->ox + cx * m->per_ext);
        *out_y = (int)(m->oy + cy * m->per_ext);
        return 1;
    }
    return 0;
}

static void collect_label(mvt_ctx *m, const uint8_t *tags, size_t taglen,
                          const uint8_t *geom, size_t geomlen)
{
    carto_label_sink *sk = m->labels;
    if (!sk || sk->n >= sk->cap || m->name_key_idx < 0) return;

    char name[CARTO_LABEL_MAX_TEXT];
    int mz = 0, rank = 0;
    feature_tags(m, tags, taglen, name, (int)sizeof name, &mz, &rank);
    if (!name[0]) return;

    int x = 0, y = 0;
    if (!feature_point(m, geom, geomlen, &x, &y)) return;

    /* Off the frame is not worth a slot: the caller has a fixed number of
       them and a name nobody can see would take one from a name they can. */
    if (x < 0 || y < 0 || x >= m->fb->width || y >= m->fb->height) return;

    /*LS-1056  If it did not fit, end it at a word.

       feature_tags fills the buffer and stops, so a long name arrives with
       its last word cut in half - "Franklin Falls Historic Distric", which
       reads as a fault rather than as an abbreviation. Backing up to the
       last space costs a word and buys a name that looks deliberate.

       LS-1060  This runs BEFORE the de-duplication below, and the order is
       the whole point.

       It used to run after, on the copy already committed to the sink, so
       the de-duplication compared names that had NOT been trimmed. Around
       Franklin there are four places whose names differ only past the
       thirty-second character - "Franklin Falls Historic District" and its
       neighbours - so four entries that trim to the identical string
       "Franklin Falls Historic" each compared unequal, each took a slot, and
       the list showed the same name four times.

       On the map this was invisible: the label collision test drops names
       whose boxes touch, so three of the four were quietly discarded and
       only one was ever drawn. It surfaced the moment a list showed all of
       them. De-duplicating on the text that will actually be SHOWN is the
       invariant worth holding, because that is the one an operator can
       check. */
    if ((int)strlen(name) >= (int)sizeof name - 1) {
        int t = (int)strlen(name);
        while (t > 4 && name[t - 1] != ' ') t--;
        while (t > 1 && name[t - 1] == ' ') t--;
        if (t > 4) name[t] = 0;
    }

    /*LS-1056  One entry per place, not one per tile that mentions it.

       A place near a tile boundary is in both tiles, and both are rendered,
       so the same name arrived twice a few cells apart - which reads as two
       towns with the same name. Comparing the text is enough: two genuinely
       different places with identical names close enough to collide on one
       screen is not a case worth carrying code for. */
    for (int i = 0; i < sk->n; i++)
        if (strcmp(sk->at[i].text, name) == 0) return;

    carto_label *L = &sk->at[sk->n++];
    memcpy(L->text, name, sizeof L->text);
    L->text[sizeof L->text - 1] = 0;
    L->x = x;
    L->y = y;
    L->min_zoom = (uint8_t)(mz < 0 ? 0 : (mz > 255 ? 255 : mz));
    L->rank = (uint8_t)(rank < 0 ? 0 : (rank > 255 ? 255 : rank));
}


int  mvt_scratch_peak(void)       { return g_mvt_scratch_peak; }
void mvt_scratch_peak_reset(void) { g_mvt_scratch_peak = 0; }

static void render_feature_msg(mvt_ctx *m, const uint8_t *b, size_t len) {
    int geomtype = 0;
    const uint8_t *geom = NULL, *tags = NULL;
    size_t geomlen = 0, taglen = 0, p = 0;
    while (p < len) {
        uint64_t key = rvarint(b, len, &p);
        uint32_t fn = (uint32_t)(key >> 3), wt = (uint32_t)(key & 7);
        if (wt == 0) {
            uint64_t v = rvarint(b, len, &p);
            if (fn == 3) geomtype = (int)v;
        } else if (wt == 2) {
            uint64_t l = rvarint(b, len, &p);
            if (fn == 4) { geom = b + p; geomlen = (size_t)l; }
            else if (fn == 2) { tags = b + p; taglen = (size_t)l; }
            p += (size_t)l;
        } else if (wt == 5) { p += 4; }
        else if (wt == 1) { p += 8; }
        else break;
    }
    if (!geom) return;

    int prio = CARTO_ROAD_PRIO_MIN;
    if (m->category == CARTO_LAYER_ROAD || m->category == CARTO_LAYER_LANDUSE) {
        char cls[40];
        feature_class(m, tags, taglen, cls, (int)sizeof cls);
        if (m->category == CARTO_LAYER_ROAD) {
            prio = carto_road_priority(cls);
            if (prio < m->min_road_prio)
                return;
        } else if (!is_park_kind(cls)) {
            return;
        }
    }
    /*LS-1055  A place is a name first and a dot second. The dot is still
       drawn - it is what says exactly where - and the name goes to the
       caller to put in cells. */
    if (m->category == CARTO_LAYER_PLACE)
        collect_label(m, tags, taglen, geom, geomlen);

    render_feature(m, geom, geomlen, geomtype, prio);
}

static void render_layer(mvt_ctx *m, const uint8_t *b, size_t len) {
    char name[80];
    int extent = 4096;
    size_t p = 0;
    int key_idx = 0;
    name[0] = 0;
    m->nvals = 0;
    m->class_key_idx = -1;
    m->name_key_idx = -1;
    m->minzoom_key_idx = -1;
    m->rank_key_idx = -1;

    while (p < len) {
        uint64_t key = rvarint(b, len, &p);
        uint32_t fn = (uint32_t)(key >> 3), wt = (uint32_t)(key & 7);
        if (wt == 2) {
            uint64_t l = rvarint(b, len, &p);
            if (fn == 1) {
                int cn = (int)l; if (cn > 79) cn = 79;
                for (int i = 0; i < cn; ++i) name[i] = (char)b[p + i];
                name[cn] = 0;
            } else if (fn == 3) {
                if (m->class_key_idx < 0 && key_is_class(b + p, (int)l))
                    m->class_key_idx = key_idx;
                if (m->name_key_idx < 0 && key_is_name(b + p, (int)l))
                    m->name_key_idx = key_idx;
                if (m->minzoom_key_idx < 0 && key_is_minzoom(b + p, (int)l))
                    m->minzoom_key_idx = key_idx;
                if (m->rank_key_idx < 0 && key_is_rank(b + p, (int)l))
                    m->rank_key_idx = key_idx;
                ++key_idx;
            } else if (fn == 4) {
                if (m->nvals < m->val_cap) {
                    const uint8_t *sp; int sl; int nv;
                    parse_value(b + p, (size_t)l, &sp, &sl, &nv);
                    m->val_ptr[m->nvals] = sp;
                    m->val_len[m->nvals] = (sp ? sl : 0);
                    m->val_num[m->nvals] = nv;
                    ++m->nvals;
                }
            }
            p += (size_t)l;
        } else if (wt == 0) {
            uint64_t v = rvarint(b, len, &p);
            if (fn == 5) extent = (int)v;
        } else if (wt == 5) { p += 4; }
        else if (wt == 1) { p += 8; }
        else break;
    }

    if (carto_classify_layer(name) != m->category) return;
    m->per_ext = m->tile_px / (float)(extent > 0 ? extent : 4096);

    p = 0;
    while (p < len) {
        uint64_t key = rvarint(b, len, &p);
        uint32_t fn = (uint32_t)(key >> 3), wt = (uint32_t)(key & 7);
        if (wt == 2) {
            uint64_t l = rvarint(b, len, &p);
            if (fn == 2) render_feature_msg(m, b + p, (size_t)l);
            p += (size_t)l;
        } else if (wt == 0) { rvarint(b, len, &p); }
        else if (wt == 5) { p += 4; }
        else if (wt == 1) { p += 8; }
        else break;
    }
}

void carto_mvt_render_category(carto_framebuffer *fb, const carto_style *style,
                              const uint8_t *tile, size_t len,
                              carto_layer_kind category,
                              double ox, double oy, double tile_px,
                              int zoom,
                              carto_ipt *scratch, int scratch_cap,
                              const uint8_t **val_ptr, int *val_len,
                              int *val_num, int val_cap,
                              carto_label_sink *labels) {
    /* Without the tables there is nothing to look a feature's class up in,
       and every feature would draw in the default style. Refusing is the
       honest answer; drawing a plausible wrong map is not. */
    if (!val_ptr || !val_len || !val_num || val_cap <= 0) return;

    mvt_ctx m;
    m.val_ptr = val_ptr;
    m.val_len = val_len;
    m.val_num = val_num;
    m.val_cap = val_cap;
    m.labels = labels;
    m.fb = fb;
    m.style = style;
    /*LS-1073  Narrowed once, here, rather than on every point. */
    m.ox = (float)ox;
    m.oy = (float)oy;
    m.tile_px = tile_px;
    m.per_ext = tile_px / 4096.0;
    m.category = category;
    m.scratch = scratch;
    m.scratch_cap = scratch_cap;

    /*LS-1053  Thin by how big the tile is DRAWN, not by its zoom number.

       Both of these were tuned against a tile drawn at its natural 256
       pixels, and they read the zoom to decide how much road to keep. That
       held while the map rendered at the panel's own resolution. It stopped
       holding the moment the map started rendering into a few pixels per
       character cell: a tile is drawn at about 77 pixels now, so everything
       is three and a third times more crowded than the number says, and
       every residential street in a town arrived on top of every other one.
       Reported from the board as roads being a mess when zoomed out.

       A tile at half its natural size is worth one zoom level of thinning,
       which is exactly what log2 of the ratio says. Expressed that way the
       tuning is about what the eye gets rather than about a coordinate, so
       it stays right if the sub-cell resolution changes again - and it is
       still the old numbers at tile_px 256, so nothing that was tuned
       against a full-resolution render moves. */
    const double zeff = (double)zoom + log2(tile_px / 256.0);

    double rs = 0.125 + (zeff - 9.0) * 0.0625;
    m.road_scale = rs < 0.125 ? 0.125 : (rs > 0.55 ? 0.55 : rs);
    int mp = (int)(18.5 - zeff);
    m.min_road_prio = mp < 1 ? 1 : (mp > 10 ? 10 : mp);

    m.nvals = 0;
    m.class_key_idx = -1;

    size_t p = 0;
    while (p < len) {
        uint64_t key = rvarint(tile, len, &p);
        uint32_t fn = (uint32_t)(key >> 3), wt = (uint32_t)(key & 7);
        if (wt == 2) {
            uint64_t l = rvarint(tile, len, &p);
            if (fn == 3) render_layer(&m, tile + p, (size_t)l);
            p += (size_t)l;
        } else if (wt == 0) { rvarint(tile, len, &p); }
        else if (wt == 5) { p += 4; }
        else if (wt == 1) { p += 8; }
        else break;
    }
}
