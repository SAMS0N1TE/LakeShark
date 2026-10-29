/* See ls_lap_manifest.h. Decode bytes explicitly: never cast untrusted ELF pointers. */
#include "ls_lap_manifest.h"
#include <limits.h>
#include "ls_textpack.h"
_Static_assert(offsetof(lap_app_t, icon_art) + sizeof(((lap_app_t *)0)->icon_art) ==
                   LS_LAP_MANIFEST_BYTES,
               "LAP wire prefix changed");
#include <stdio.h>
#include <string.h>

static bool fail(char *why, size_t cap, const char *s) {
    if (why && cap)
        snprintf(why, cap, "%s", s);
    return false;
}
static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t u32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static bool range(size_t off, size_t n, size_t size) { return off <= size && n <= size - off; }
static bool overlap(size_t a, size_t n, size_t b, size_t m) {
    return n && m && a < b + m && b < a + n;
}
static bool get(const ls_lap_reader_t *r, size_t off, void *p, size_t n) {
    return range(off, n, r->size) && r->read(r->ctx, off, p, n);
}
typedef struct {
    uint32_t name, type, flags, addr, off, size, link, info, align, ent;
} section;
typedef struct {
    uint32_t type, off, addr, filesz, memsz, flags, align;
} segment;
typedef struct {
    uint32_t ph, sh, entry;
    unsigned np, ns, strings;
} header;
static bool shget(const ls_lap_reader_t *r, const header *h, unsigned i, section *s) {
    uint8_t b[40];
    if (i >= h->ns || !get(r, h->sh + i * 40u, b, sizeof(b)))
        return false;
    *s = (section){u32(b),      u32(b + 4),  u32(b + 8),  u32(b + 12), u32(b + 16),
                   u32(b + 20), u32(b + 24), u32(b + 28), u32(b + 32), u32(b + 36)};
    return true;
}
static bool phget(const ls_lap_reader_t *r, const header *h, unsigned i, segment *s) {
    uint8_t b[32];
    if (i >= h->np || !get(r, h->ph + i * 32u, b, sizeof(b)))
        return false;
    *s = (segment){u32(b),      u32(b + 4),  u32(b + 8), u32(b + 16),
                   u32(b + 20), u32(b + 24), u32(b + 28)};
    return true;
}
/* Bound each name as well as the table. The loader uses strcmp on these. */
static bool nameget(const ls_lap_reader_t *r, section s, uint32_t index, char out[256]) {
    if (s.type != 3 || index >= s.size)
        return false;
    size_t n = s.size - index;
    if (n > 256)
        n = 256;
    if (!get(r, s.off + index, out, n))
        return false;
    return memchr(out, 0, n) != NULL;
}
static bool head(const ls_lap_reader_t *r, header *h, char *why, size_t cap) {
    uint8_t b[52];
    if (!r || !r->read || r->size > LS_LAP_MAX_BYTES || !get(r, 0, b, sizeof(b)))
        return fail(why, cap, LS_TEXT(t_ls_lap_manifest_elf_truncated_or_larger, "ELF truncated or larger than 512 KB"));
    if (memcmp(b, "\177ELF\1\1\1", 7) || u16(b + 16) != 3 || u16(b + 18) != 243 ||
        u32(b + 20) != 1 || u16(b + 40) != 52)
        return fail(why, cap, LS_TEXT(t_ls_lap_manifest_needs_little_endian_elf32, "needs little-endian ELF32 RISC-V ET_DYN"));
    *h = (header){u32(b + 28), u32(b + 32), u32(b + 24), u16(b + 44), u16(b + 48), u16(b + 50)};
    if (!h->ns || h->ns > 128 || h->np > 16 || !h->strings || h->strings >= h->ns ||
        u16(b + 46) != 40 || (h->np && u16(b + 42) != 32) || h->sh % 4 || h->ph % 4 ||
        !range(h->sh, h->ns * 40u, r->size) || h->sh < 52 ||
        (h->np && (!range(h->ph, h->np * 32u, r->size) || h->ph < 52 ||
                   overlap(h->ph, h->np * 32u, h->sh, h->ns * 40u))))
        return fail(why, cap, LS_TEXT(t_ls_lap_manifest_invalid_elf_header_tables, "invalid ELF header tables"));
    return true;
}
bool ls_lap_plain_name(const char *name, size_t max) {
    if (!name || !*name || *name == '.' || !max || max > 48)
        return false;
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++, n++)
        if (n >= max || !((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                          (*p >= '0' && *p <= '9') || *p == '_' || *p == '.' || *p == '-'))
            return false;
    return !strstr(name, "..");
}
bool ls_lap_name_equal(const char *a, const char *b) {
    if (!a || !b)
        return false;
    for (;; a++, b++) {
        unsigned x = (unsigned char)*a, y = (unsigned char)*b;
        if (x >= 'A' && x <= 'Z')
            x += 32;
        if (y >= 'A' && y <= 'Z')
            y += 32;
        if (x != y)
            return false;
        if (!x)
            return true;
    }
}
bool ls_lap_fields_valid(const lap_app_t *m, char *why, size_t cap) {
    if (!m || m->magic != LAP_MAGIC)
        return fail(why, cap, "bad LAP magic");
    /* API 2.2 firmware runs every 1.x app (1.0-1.3), and 2.0 to 2.2 apps. */
    if (!((m->api_major == 1 && m->api_minor <= LS_LAP_API1_MINOR) ||
          (m->api_major == 2 && m->api_minor <= LS_LAP_API2_MINOR)))
        return fail(why, cap, "unsupported app API");
    if (!memchr(m->id, 0, sizeof(m->id)) || !m->id[0])
        return fail(why, cap, "invalid app id");
    for (const char *p = m->id; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_' || *p == '.' ||
              *p == '-'))
            return fail(why, cap, "invalid app id");
    /* Dot-only ids would escape apps_data even without a slash. */
    if (!strcmp(m->id, ".") || !strcmp(m->id, ".."))
        return fail(why, cap, "unsafe app id");
    if (!m->name[0] || !memchr(m->name, 0, sizeof(m->name)) || !memchr(m->sub, 0, sizeof(m->sub)) ||
        !memchr(m->version, 0, sizeof(m->version)) || !memchr(m->author, 0, sizeof(m->author)))
        return fail(why, cap, LS_TEXT(t_ls_lap_manifest_unterminated_or_empty_manifest, "unterminated or empty manifest field"));
    for (unsigned y = 0; y < LAP_ICON_H; y++) {
        if (!memchr(m->icon_art[y], 0, LAP_ICON_W + 1))
            return fail(why, cap, LS_TEXT(t_ls_lap_manifest_unterminated_icon_art, "unterminated icon art"));
        for (unsigned x = 0; x < LAP_ICON_W + 1; x++) {
            unsigned c = (unsigned char)m->icon_art[y][x];
            if (c && (c < 32 || c > 126))
                return fail(why, cap, LS_TEXT(t_ls_lap_manifest_icon_art_must_be, "icon art must be printable ASCII"));
        }
    }
    /* A capability cannot hide in a manifest older than the API that defined it. */
    const unsigned known = m->api_major >= 2 ? (m->api_minor >= 1 ? LS_LAP_CAPS_API21 : LS_LAP_CAPS_API2)
                                             : LAP_CAP_NETWORK;
    if ((m->flags & ~known) || (m->flags && m->api_major == 1 && m->api_minor < 3) ||
        m->reserved || m->color > 15 || m->icon > 12)
        return fail(why, cap, LS_TEXT(t_ls_lap_manifest_invalid_reserved_icon_color, "invalid reserved/icon/color field"));
    if (why && cap)
        why[0] = 0;
    return true;
}
bool ls_lap_manifest_read(const ls_lap_reader_t *r, lap_app_t *out, char *why, size_t cap) {
    header h;
    section strings, s, t;
    char name[256];
    uint32_t manifest = 0;
    size_t file_end = 52;
    if (!out)
        return fail(why, cap, "no output");
    if (!head(r, &h, why, cap))
        return false;
    file_end = h.sh + h.ns * 40u;
    if (h.np && h.ph + h.np * 32u > file_end)
        file_end = h.ph + h.np * 32u;
    if (!shget(r, &h, h.strings, &strings) || strings.type != 3 ||
        !range(strings.off, strings.size, r->size))
        return fail(why, cap, LS_TEXT(t_ls_lap_manifest_bad_section_string_table, "bad section string table"));
    for (unsigned i = 0; i < h.ns; i++) {
        if (!shget(r, &h, i, &s))
            return fail(why, cap, "cannot read section");
        if (i == 0 && (s.name || s.type || s.flags || s.addr || s.off || s.size || s.link ||
                       s.info || s.align || s.ent))
            return fail(why, cap, LS_TEXT(t_ls_lap_manifest_nonzero_null_section, "nonzero null section"));
        if (s.off > r->size || s.size > LS_LAP_MAX_MEMORY || s.link >= h.ns ||
            (s.align && (s.align > 65536 || (s.align & (s.align - 1)))) ||
            (s.type != 8 && !range(s.off, s.size, r->size)) || !nameget(r, strings, s.name, name))
            return fail(why, cap, LS_TEXT(t_ls_lap_manifest_invalid_section_bounds_name, "invalid section bounds/name/link"));
        if (s.type != 8 && (size_t)s.off + s.size > file_end)
            file_end = (size_t)s.off + s.size;
        if (s.type != 8 && s.size) {
            if (overlap(s.off, s.size, 0, 52) || overlap(s.off, s.size, h.sh, h.ns * 40u) ||
                overlap(s.off, s.size, h.ph, h.np * 32u))
                return fail(why, cap, LS_TEXT(t_ls_lap_manifest_section_overlaps_elf_tables, "section overlaps ELF tables"));
            for (unsigned j = 1; j < i; j++) {
                if (!shget(r, &h, j, &t))
                    return fail(why, cap, "cannot read section");
                if (t.type != 8 && overlap(s.off, s.size, t.off, t.size))
                    return fail(why, cap, LS_TEXT(t_ls_lap_manifest_overlapping_sections, "overlapping sections"));
            }
        }
        if (!strcmp(name, ".lap_app")) {
            if (manifest || s.type != 1 || s.size != LS_LAP_DESCRIPTOR_BYTES || !(s.flags & 2) || s.off % 4)
                return fail(why, cap, LS_TEXT(t_ls_lap_manifest_invalid_or_duplicate_lap, "invalid or duplicate .lap_app"));
            manifest = s.off;
        }
    }
    uint32_t end = 0;
    for (unsigned i = 0; i < h.np; i++) {
        segment p;
        if (!phget(r, &h, i, &p) || !range(p.off, p.filesz, r->size) || p.memsz < p.filesz ||
            p.memsz > LS_LAP_MAX_MEMORY || p.addr > LS_LAP_MAX_MEMORY - p.memsz ||
            (p.align && (p.align > 65536 || (p.align & (p.align - 1)))))
            return fail(why, cap, LS_TEXT(t_ls_lap_manifest_invalid_segment_bounds, "invalid segment bounds"));
        if (p.type == 1 && p.memsz) {
            if (p.addr < end)
                return fail(why, cap, LS_TEXT(t_ls_lap_manifest_overlapping_or_unordered_segments, "overlapping or unordered segments"));
            end = p.addr + p.memsz;
        }
    }
    if (file_end != r->size)
        return fail(why, cap, LS_TEXT(t_ls_lap_manifest_unrecognized_trailing_elf_data, "unrecognized trailing ELF data"));
    if (!manifest)
        return fail(why, cap, "missing .lap_app");
    uint8_t b[LS_LAP_MANIFEST_BYTES];
    lap_app_t m;
    if (!get(r, manifest, b, sizeof(b)))
        return fail(why, cap, "truncated manifest");
    memset(&m, 0, sizeof(m));
    m.magic = u32(b);
    m.api_major = u16(b + 4);
    m.api_minor = u16(b + 6);
    /* Fixed-width wire prefix is identical on host and board; tail padding differs. */
    memcpy(m.id, b + 8, LS_LAP_MANIFEST_BYTES - 8);
    if (!ls_lap_fields_valid(&m, why, cap))
        return false;
    *out = m;
    return true;
}
static bool memread(void *ctx, size_t off, void *p, size_t n) {
    memcpy(p, (const uint8_t *)ctx + off, n);
    return true;
}
bool ls_lap_icon_read(const ls_lap_reader_t *r, uint8_t out[LAP_PX_H][LAP_PX_W]) {
    memset(out, 0xFF, LAP_PX_H * LAP_PX_W);
    header h;
    section s, strings;
    char name[256];
    char why[8];
    if (!r || !head(r, &h, why, sizeof(why)) || !shget(r, &h, h.strings, &strings))
        return false;
    for (unsigned i = 1; i < h.ns; i++) {
        if (!shget(r, &h, i, &s) || !nameget(r, strings, s.name, name) || strcmp(name, ".lap_icon"))
            continue;
        lap_icon_px_t icon;
        if (s.type != 1 || s.size != sizeof(icon) || !range(s.off, s.size, r->size) ||
            !get(r, s.off, &icon, sizeof(icon)) || memcmp(icon.magic, "LIC1", 4))
            return false;
        for (int y = 0; y < LAP_PX_H; y++)
            for (int x = 0; x < LAP_PX_W; x++) {
                const char c = icon.rows[y][x];
                const int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
                            : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
                if (v < 0 && c != '.' && c != ' ' && c) {
                    memset(out, 0xFF, LAP_PX_H * LAP_PX_W);
                    return false;
                }
                out[y][x] = v < 0 ? 0xFF : (uint8_t)v;
            }
        return true;
    }
    return false;
}
bool ls_lap_manifest(const void *bytes, size_t size, lap_app_t *out, char *why, size_t cap) {
    if (!bytes)
        return fail(why, cap, "no input");
    ls_lap_reader_t r = {memread, (void *)bytes, size};
    return ls_lap_manifest_read(&r, out, why, cap);
}
static bool mapped(const ls_lap_reader_t *r, const header *h, uint32_t addr, size_t n,
                   unsigned flags) {
    for (unsigned i = 0; i < h->np; i++) {
        segment p;
        if (!phget(r, h, i, &p))
            return false;
        if (p.type == 1 && (p.flags & flags) == flags && addr >= p.addr &&
            range(addr - p.addr, n, p.memsz))
            return true;
    }
    return false;
}
bool ls_lap_preflight_resolved(const void *bytes, size_t size, ls_lap_resolve_fn resolve, char *why,
                               size_t cap) {
    lap_app_t m;
    header h;
    section s, t, strings;
    char name[256];
    uint8_t b[16];
    if (!ls_lap_manifest(bytes, size, &m, why, cap))
        return false;
    ls_lap_reader_t r = {memread, (void *)bytes, size};
    if (!head(&r, &h, why, cap) || (h.entry & 1u) || !mapped(&r, &h, h.entry, 2, 1))
        return fail(why, cap, LS_TEXT(t_ls_lap_manifest_entry_outside_executable_segment, "entry outside executable segment"));
    const uint8_t *raw = bytes;
    if ((u32(raw + 36) & ~1u) != 2u)
        return fail(why, cap, LS_TEXT(t_ls_lap_manifest_needs_ilp32f_risc_v, "needs ilp32f RISC-V ABI"));
    for (unsigned i = 0; i < h.np; i++) {
        segment p;
        if (!phget(&r, &h, i, &p))
            return fail(why, cap, "bad segment");
        if (p.type == 1 && (p.flags & 3) == 3)
            return fail(why, cap, LS_TEXT(t_ls_lap_manifest_writable_executable_segment, "writable executable segment"));
    }
    for (unsigned i = 1; i < h.ns; i++) {
        if (!shget(&r, &h, i, &s))
            return fail(why, cap, "bad section");
        if ((s.flags & 2) && !mapped(&r, &h, s.addr, s.size, 0))
            return fail(why, cap, LS_TEXT(t_ls_lap_manifest_unmapped_allocated_section, "unmapped allocated section"));
        if (s.type == 9 || (s.flags & 0x400))
            return fail(why, cap, LS_TEXT(t_ls_lap_manifest_rel_tls_not_supported, "REL/TLS not supported"));
        if (s.type == 2 || s.type == 11) {
            if (s.off % 4 || s.ent != 16 || s.size % 16 || !shget(&r, &h, s.link, &strings) ||
                strings.type != 3)
                return fail(why, cap, LS_TEXT(t_ls_lap_manifest_invalid_symbol_table, "invalid symbol table"));
            for (uint32_t j = 0; j < s.size; j += 16) {
                if (!get(&r, s.off + j, b, 16) || !nameget(&r, strings, u32(b), name))
                    return fail(why, cap, "invalid symbol name");
                unsigned ndx = u16(b + 14);
                if (!ndx && u32(b + 4))
                    return fail(why, cap, LS_TEXT(t_ls_lap_manifest_nonzero_undefined_symbol_value, "nonzero undefined symbol value"));
                if (!ndx && name[0] && resolve && !resolve(name)) {
                    if (why && cap)
                        snprintf(why, cap, "needs %.88s", name);
                    return false;
                }
                if (ndx && ndx != 0xfff1 &&
                    (ndx >= h.ns || !mapped(&r, &h, u32(b + 4), u32(b + 8), 0)))
                    return fail(why, cap, LS_TEXT(t_ls_lap_manifest_invalid_symbol_range, "invalid symbol range"));
            }
        }
        if (s.type == 4) {
            if (s.off % 4 || s.ent != 12 || s.size % 12 || s.info >= h.ns ||
                !shget(&r, &h, s.link, &t) || (t.type != 2 && t.type != 11) || t.ent != 16 ||
                t.size % 16)
                return fail(why, cap, LS_TEXT(t_ls_lap_manifest_invalid_relocation_table, "invalid relocation table"));
            for (uint32_t j = 0; j < s.size; j += 12) {
                if (!get(&r, s.off + j, b, 12))
                    return fail(why, cap, "bad relocation");
                uint32_t info = u32(b + 4), type = info & 255;
                if ((info >> 8) >= t.size / 16 ||
                    (type != 0 && type != 1 && type != 3 && type != 5) ||
                    (type && (u32(b) % 4 || !mapped(&r, &h, u32(b), 4, 2))))
                    return fail(why, cap, LS_TEXT(t_ls_lap_manifest_unsupported_or_text_relocation, "unsupported or text relocation"));
                if (type == 3 && !mapped(&r, &h, u32(b + 8), 0, 0))
                    return fail(why, cap, LS_TEXT(t_ls_lap_manifest_relative_addend_outside_app, "relative addend outside app"));
            }
        }
    }
    return true;
}

bool ls_lap_preflight(const void *bytes, size_t size, char *why, size_t cap) {
    return ls_lap_preflight_resolved(bytes, size, NULL, why, cap);
}
