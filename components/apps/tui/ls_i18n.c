/* The interface language. See ls_i18n.h. */

#include "ls_i18n.h"

#include <stdint.h>
#include <string.h>

#include "esp_attr.h"
#include "tuilib/tui_core.h"
#include "ls_textpack.h"

typedef struct {
    const char *en;
    const char *uk;
} ls_i18n_pair_t;

#if defined(LS_I18N_PACKED) && LS_I18N_PACKED
/* The firmware's table: ls_i18n_uk.h read by
   components/ls_textpack/gen/textpack_gen.py. The English keys stay literals
   (the linker shares them with the labels in the code); the Ukrainian, in
   the same order, is zstd-packed and decoded to PSRAM the first time anything
   asks for an entry (Ukrainian chosen, or a test walking the table). A lookup
   works as before; the Ukrainian just lives in PSRAM. */
#include "ls_i18n_pack.h"

#define N_PAIRS LS_I18N_PAIRS
EXT_RAM_BSS_ATTR static const char *s_pair_uk[N_PAIRS];
static bool s_unpacked;

/* false when the frames could not be decoded (no memory): every entry then
   reads as missing and the interface stays English. */
static bool unpack(void)
{
    if (__atomic_load_n(&s_unpacked, __ATOMIC_ACQUIRE)) return true;
    int k = 0;
    for (unsigned f = 0; f < ls_i18n_nframes; f++) {
        const char *p = ls_text_frame(&ls_i18n_frames[f], &ls_i18n_slots[f]);
        if (!p) return false;
        const char *end = p + ls_i18n_frames[f].raw_len;
        while (p < end && k < N_PAIRS) {
            s_pair_uk[k++] = p;
            p += strlen(p) + 1;
        }
    }
    if (k != N_PAIRS) return false;
    __atomic_store_n(&s_unpacked, true, __ATOMIC_RELEASE);
    return true;
}
#define PAIR_EN(i) ls_i18n_en[i]
#define PAIR_UK(i) s_pair_uk[i]
#else
#include "ls_i18n_uk.h"

#define N_PAIRS ((int)(sizeof(UK) / sizeof(UK[0])))
static bool unpack(void) { return true; }
#define PAIR_EN(i) UK[i].en
#define PAIR_UK(i) UK[i].uk
#endif
/* Open addressing at under half full, so a miss - the common case, text
   from the radio - ends at the first or second empty slot. */
#define HASH_SLOTS 2048
#define ARENA_BYTES 16384
_Static_assert(N_PAIRS < HASH_SLOTS / 2, "grow HASH_SLOTS with the table");

/* Built once, the first time Ukrainian is chosen. PSRAM, zeroed at boot,
   no initialisers (EXT_RAM_BSS_ATTR). */
EXT_RAM_BSS_ATTR static uint16_t s_hash[HASH_SLOTS];   /* entry + 1, 0 empty */
EXT_RAM_BSS_ATTR static uint16_t s_off[N_PAIRS];        /* into s_arena      */
EXT_RAM_BSS_ATTR static char     s_arena[ARENA_BYTES];

static ls_lang_t s_lang = LS_LANG_EN;
static bool s_built;
static int s_dropped;
static void (*s_save)(int lang);
static void (*s_miss)(const char *en);

static uint32_t hash(const char *s)
{
    uint32_t h = 2166136261u;
    while (*s) h = (h ^ (uint8_t)*s++) * 16777619u;
    return h;
}

static void build(void)
{
    if (s_built) return;
    s_built = true;
    memset(s_hash, 0, sizeof(s_hash));
    /* Offset 0 is an empty string: an entry that does not fit reads as
       missing and stays English. */
    size_t used = 1;
    s_arena[0] = 0;
    s_dropped = 0;
    if (!unpack()) { s_dropped = N_PAIRS; return; }   /* empty hash: all English */
    for (int i = 0; i < N_PAIRS; i++) {
        const size_t room = ARENA_BYTES - used;
        const size_t need = (size_t)tui_utf8_cells(PAIR_UK(i), strlen(PAIR_UK(i))) + 1;
        if (need > room) { s_off[i] = 0; s_dropped++; continue; }
        s_off[i] = (uint16_t)used;
        used += tui_utf8_to_cells(s_arena + used, room, PAIR_UK(i)) + 1;
        uint32_t h = hash(PAIR_EN(i)) & (HASH_SLOTS - 1);
        while (s_hash[h]) h = (h + 1) & (HASH_SLOTS - 1);
        s_hash[h] = (uint16_t)(i + 1);
    }
}

/* Read by any task (a worker may format a notice), written by the UI task
   after the table is built, so a reader that sees Ukrainian sees the table. */
static ls_lang_t lang_now(void) { return __atomic_load_n(&s_lang, __ATOMIC_ACQUIRE); }

ls_lang_t ls_i18n_lang(void) { return lang_now(); }

bool ls_i18n_lang_valid(int lang) { return lang >= 0 && lang < LS_LANG_COUNT; }

static void apply(ls_lang_t lang)
{
    if (!ls_i18n_lang_valid((int)lang)) lang = LS_LANG_EN;
    if (lang == LS_LANG_UK) build();
    __atomic_store_n(&s_lang, lang, __ATOMIC_RELEASE);
}

void ls_i18n_set_lang(ls_lang_t lang)
{
    apply(lang);
    if (s_save) s_save((int)lang_now());
}

void ls_i18n_load(int stored)
{
    apply(ls_i18n_lang_valid(stored) ? (ls_lang_t)stored : LS_LANG_EN);
}

void ls_i18n_set_saver(void (*save)(int lang)) { s_save = save; }

ls_lang_t ls_i18n_next(ls_lang_t lang)
{
    return (ls_lang_t)(((int)lang + 1) % LS_LANG_COUNT);
}

const char *ls_i18n_lang_name(ls_lang_t lang)
{
    /* "Українська" in cell bytes, converted once. */
    static char uk[24];
    if (lang != LS_LANG_UK) return "English";
    if (!uk[0]) tui_utf8_to_cells(uk, sizeof(uk), LS_TEXT(t_ls_i18n, "Українська"));
    return uk;
}

const char *ls_tr(const char *en)
{
    if (lang_now() == LS_LANG_EN || !en || !*en) return en;
    uint32_t h = hash(en) & (HASH_SLOTS - 1);
    for (uint16_t e; (e = s_hash[h]) != 0; h = (h + 1) & (HASH_SLOTS - 1)) {
        if (!strcmp(PAIR_EN(e - 1), en)) {
            const char *t = s_arena + s_off[e - 1];
            return *t ? t : en;
        }
    }
    if (s_miss) s_miss(en);
    return en;
}

const char *ls_tr_ctx(const char *key, const char *en)
{
    if (lang_now() == LS_LANG_EN || !key) return en;
    const char *t = ls_tr(key);
    return t == key ? en : t;
}

void ls_i18n_set_miss_hook(void (*miss)(const char *en)) { s_miss = miss; }

int ls_i18n_entries(void) { return N_PAIRS; }
const char *ls_i18n_entry_en(int i) { return i >= 0 && i < N_PAIRS && unpack() ? PAIR_EN(i) : NULL; }
const char *ls_i18n_entry_uk(int i) { return i >= 0 && i < N_PAIRS && unpack() ? PAIR_UK(i) : NULL; }
int ls_i18n_dropped(void) { build(); return s_dropped; }
