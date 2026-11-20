/* Portuguese, French, German, Italian and Dutch -> English gists, and which
   language a chat line is in. Written for this app.

   One reader for the five, driven by each language's data file
   (lang/<code>.bin from tools/gen_xlate_lang.py), in passes over a batch:
   1. tokens: words folded (lower case, accents off, German umlauts as ae oe
      ue), elisions split (l', j', dell'), hyphens split (es-tu, chama-se),
      chat spellings expanded (vc, slt, cmq), laughter, long vowels;
   2. the longest phrase of the hand table first, then each word read: the
      forms the rules cannot make, the lexicon, then the inflection rules
      learned from Wiktionary's tables (suffixes and, for German and Dutch,
      the ge- prefix and separable particles); German and Dutch compounds
      split in two;
   3. English written back: a subject for a dropped one (pt, it), object
      pronouns after the verb, do-support for not and questions, the perfect
      as a past, progressive, going to, modals, the adjective before its
      noun, German and Dutch verbs at the end of a clause brought forward,
      separable verbs joined again.
   Detection: built-in word profiles of seven languages (mt_xlate_langs.h),
   with the 1.3 Spanish/English detector deciding between those two.
   No malloc; static buffers, so one caller at a time. */
#include "mt_xlate.h"
#include "mt_xlate_int.h"
#include "mt_xlate_langs.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define LK      32      /* longest folded word kept; longer words pass through */
#define LMAXT   96      /* tokens per batch */
#define LBATCH  1200    /* source bytes per batch */
#define LCH     224     /* English pieces per batch */
#define LARENA  4096    /* English text per batch */
#define MAGIC   0x314C544Du
#define HEADW   23      /* header words: magic, code, date, pool size, 9 sections, credit */

/* ------------------------------------------------------------ languages */

static const char *const CODE[MT_LANG_COUNT] = { "", "en", "es", "pt", "fr", "de", "it", "nl", "pl", "ru", "uk" };
static const char *const NAME[MT_LANG_COUNT] = { "", "ENGLISH", "SPANISH", "PORTUGUESE", "FRENCH", "GERMAN",
                                                  "ITALIAN", "DUTCH", "POLISH", "RUSSIAN", "UKRAINIAN" };
/* how a language's clause is built: German and Dutch put verbs at the end;
   Romance puts adjectives after the noun; the rest before it */
#define GERMANIC(l) ((l) == MT_LANG_DE || (l) == MT_LANG_NL)
#define ROMANCE(l)  ((l) == MT_LANG_PT || (l) == MT_LANG_FR || (l) == MT_LANG_IT)
#define SLAVIC(l)   ((l) == MT_LANG_PL || (l) == MT_LANG_RU || (l) == MT_LANG_UK)
#define CYRL(l)     ((l) == MT_LANG_RU || (l) == MT_LANG_UK)

const char *mt_lang_code(mt_lang_t l) { return (l > 0 && l < MT_LANG_COUNT) ? CODE[l] : ""; }
const char *mt_lang_name(mt_lang_t l) { return (l > 0 && l < MT_LANG_COUNT) ? NAME[l] : ""; }
mt_lang_t mt_lang_from_code(const char *c)
{
    int l;
    if (!c) return MT_LANG_UNKNOWN;
    for (l = 1; l < MT_LANG_COUNT; l++)
        if (!strcmp(c, CODE[l])) return (mt_lang_t)l;
    return MT_LANG_UNKNOWN;
}

/* ----------------------------------------------------------- card data */

enum { LP_N = 1, LP_A, LP_V, LP_R, LP_I };
enum { T_NONE, T_INF, T_PRES, T_PRET, T_IMPF, T_FUT, T_COND, T_SUBJ, T_SUBJI, T_IMP, T_GER, T_PART, T_PLUP };
enum { C_PH = 1, C_CL, C_INTJ, C_ADV, C_PREP, C_CONJ, C_Q, C_DET, C_PRON, C_CLIT, C_PRONX, C_NEG, C_NEG2, C_NUM,
       C_SKIP, C_EXIST };
#define F_DEF   (1u << 8)
#define F_PL    (1u << 9)
#define F_ACC   (1u << 10)
#define F_SUB   (1u << 11)
#define F_PRE   (1u << 12)
#define F_AMB   (1u << 13)
#define F_REFL  (1u << 14)
#define F_Q     (1u << 15)
#define F_POSS  (1u << 16)
#define F_THE   (1u << 17)
#define F_NEG   (1u << 18)
#define F_A     (1u << 19)
#define F_S     (1u << 20)
#define F_HYPH  (1u << 21)
#define F_OBJ   (1u << 22)
#define F_NEVER (1u << 23)
#define F_E     (1u << 24)
#define F_PAST  (1u << 25)
#define F_YOU   (1u << 26)
#define F_INT   (1u << 27)
#define F_QUE   (1u << 28)
enum { R_BE = 1, R_HAVE = 2, R_PROG = 4, R_GOING = 8, R_CAN = 16, R_WANT = 32, R_MUST = 64, R_SHOULD = 128,
       R_WILL = 256, R_MAY = 512, R_PERFBE = 1024, R_IT = 2048, R_NEED = 4096, R_DE = 8192, R_JUST = 16384 };
#define LEX_UNCOUNT 1

typedef struct {
    const char *pool;
    uint32_t npool;
    const uint32_t *lex, *exc, *rule, *rinfo, *func, *funca, *abbr, *verb;
    const uint16_t *excl;
    int nlex, nexc, nrule, nfunc, nabbr, nverb;
} xd_t;

static xd_t D[MT_LANG_COUNT];
#define MAXRULE 1536
static uint8_t RLAST[MT_LANG_COUNT - MT_LANG_FIRST_CARD][MAXRULE];   /* each rule's last letter, 0 for none: a quick skip */
static const xd_t *X;           /* the language being read */
static int XL;

static const char *XP(uint32_t w) { return X->pool + (w & 0x3FFFFFu); }
static const char *NX(const char *s) { return s + strlen(s) + 1; }
static int streq(const char *a, const char *b) { return strcmp(a, b) == 0; }

/* first entry whose key is `key`, -1 if none */
static int xfind(const uint32_t *t, int n, const char *key)
{
    int lo = 0, hi = n;
    while (lo < hi) {
        int mid = (lo + hi) >> 1;
        if (strcmp(XP(t[mid]), key) < 0) lo = mid + 1; else hi = mid;
    }
    return (lo < n && streq(XP(t[lo]), key)) ? lo : -1;
}

/* every one of `n` entries names `k` strings inside the pool */
static int strings_ok(const xd_t *d, const uint32_t *t, int n, int k)
{
    int i, j;
    for (i = 0; i < n; i++) {
        uint32_t o = t[i] & 0x3FFFFFu;
        for (j = 0; j < k; j++) {
            if (o >= d->npool) return 0;
            o += (uint32_t)strlen(d->pool + o) + 1;
        }
    }
    return 1;
}

int mt_xlate_lang_attach(mt_lang_t lang, const void *data, size_t len)
{
    const uint8_t *b = (const uint8_t *)data;
    uint32_t h[HEADW], off[9], cnt[9];
    static const uint8_t ELEM[9] = { 4, 4, 2, 4, 4, 4, 4, 4, 4 };
    xd_t d;
    int s, i;
    if (lang < MT_LANG_FIRST_CARD || lang >= MT_LANG_COUNT || !b || len < sizeof h || ((uintptr_t)b & 3)) return -1;
    memcpy(h, b, sizeof h);
    if (h[0] != MAGIC) return -2;
    {
        char code[5];
        memcpy(code, &h[1], 4);
        code[4] = 0;
        if (strcmp(code, CODE[lang])) return -3;
    }
    memset(&d, 0, sizeof d);
    d.npool = h[3];
    if (!d.npool || d.npool > len - sizeof h || b[sizeof h + d.npool - 1] != 0) return -4;
    d.pool = (const char *)b + sizeof h;
    for (s = 0; s < 9; s++) {
        off[s] = h[4 + 2 * s];
        cnt[s] = h[5 + 2 * s];
        if ((off[s] & 3) || off[s] > len || cnt[s] > (len - off[s]) / ELEM[s] || cnt[s] > 0xFFFF0u) return -5;
    }
    d.lex = (const uint32_t *)(b + off[0]); d.nlex = (int)cnt[0];
    d.exc = (const uint32_t *)(b + off[1]); d.nexc = (int)cnt[1];
    d.excl = (const uint16_t *)(b + off[2]);
    d.rule = (const uint32_t *)(b + off[3]); d.nrule = (int)cnt[3];
    d.rinfo = (const uint32_t *)(b + off[4]);
    d.func = (const uint32_t *)(b + off[5]); d.nfunc = (int)cnt[5];
    d.funca = (const uint32_t *)(b + off[6]);
    d.abbr = (const uint32_t *)(b + off[7]); d.nabbr = (int)cnt[7];
    d.verb = (const uint32_t *)(b + off[8]); d.nverb = (int)cnt[8];
    if (cnt[2] != cnt[1] || cnt[4] != cnt[3] || cnt[6] != cnt[5] || !d.nlex || d.nlex > 65535) return -6;
    if (!strings_ok(&d, d.lex, d.nlex, 2) || !strings_ok(&d, d.exc, d.nexc, 1) || !strings_ok(&d, d.rule, d.nrule, 4) ||
        !strings_ok(&d, d.func, d.nfunc, 3) || !strings_ok(&d, d.abbr, d.nabbr, 2) || h[22] >= d.npool) return -7;
    for (i = 0; i < d.nexc; i++) if (d.excl[i] >= d.nlex) return -8;
    for (i = 0; i < d.nverb; i++) if ((int)(d.verb[i] >> 16) >= d.nlex) return -8;
    for (i = 0; i < d.nrule; i++) if ((d.rinfo[i] & 15) < LP_N || (d.rinfo[i] & 15) > LP_I) return -9;
    if (d.nrule > MAXRULE) return -10;
    for (i = 0; i < d.nrule; i++) {
        const char *fpre = d.pool + (d.rule[i] & 0x3FFFFFu), *lpre = fpre + strlen(fpre) + 1, *fsuf = lpre + strlen(lpre) + 1;
        const size_t fs = strlen(fsuf);
        RLAST[lang - MT_LANG_FIRST_CARD][i] = (uint8_t)(fs ? fsuf[fs - 1] : 0);
    }
    D[lang] = d;
    return 0;
}

void mt_xlate_lang_detach(mt_lang_t lang)
{
    if (lang >= MT_LANG_FIRST_CARD && lang < MT_LANG_COUNT) memset(&D[lang], 0, sizeof D[lang]);
}

int mt_xlate_lang_ready(mt_lang_t lang)
{
    if (lang == MT_LANG_ES) return 1;
    return lang >= MT_LANG_FIRST_CARD && lang < MT_LANG_COUNT && D[lang].nlex > 0;
}

/* lexicon */
#define LX_POS(i)   ((int)((X->lex[i] >> 22) & 15))
#define LX_FLAGS(i) ((int)((X->lex[i] >> 28) & 15))
static const char *lx_key(int i) { return XP(X->lex[i]); }
static const char *lx_gloss(int i) { return i >= 0 ? NX(XP(X->lex[i])) : ""; }
static int lex_pos(const char *key, int pos)
{
    int i = xfind(X->lex, X->nlex, key);
    if (i < 0) return -1;
    for (; i < X->nlex && streq(lx_key(i), key); i++)
        if (LX_POS(i) == pos) return i;
    return -1;
}
static int verb_role(int lex)
{
    int lo = 0, hi = X->nverb;
    if (lex < 0) return 0;
    while (lo < hi) {
        int mid = (lo + hi) >> 1;
        if ((int)(X->verb[mid] >> 16) < lex) lo = mid + 1; else hi = mid;
    }
    return (lo < X->nverb && (int)(X->verb[lo] >> 16) == lex) ? (int)(X->verb[lo] & 0xFFFF) : 0;
}

/* function words: the entry for key, the one written with an accent when acc
   says so. *mismatch: the only entry differs in accent from what was typed. */
#define FA(i)       ((i) >= 0 ? X->funca[i] : 0u)
#define FCLASS(i)   ((int)(FA(i) & 31))
#define FPERSON(i)  ((int)((FA(i) >> 5) & 7))
#define FHAS(i, f)  ((FA(i) & (f)) != 0)
static const char *fn_en(int i) { return NX(XP(X->func[i])); }
static const char *fn_alt(int i) { const char *a = NX(fn_en(i)); return *a ? a : fn_en(i); }
static int func_find(const char *key, int acc, int *mismatch)
{
    int i = xfind(X->func, X->nfunc, key), j;
    if (mismatch) *mismatch = 0;
    if (i < 0) return -1;
    j = (i + 1 < X->nfunc && streq(XP(X->func[i + 1]), key)) ? i + 1 : -1;
    if (j >= 0) return (acc ? FHAS(j, F_ACC) : !FHAS(j, F_ACC)) ? j : i;
    if (mismatch) *mismatch = acc && !FHAS(i, F_ACC);
    return i;
}

/* ---------------------------------------------------------------- letters */

static int upc(int c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }
static int isdig(int c) { return c >= '0' && c <= '9'; }

/* One letter of UTF-8 Latin text at s, folded into o[0..*no): bytes used, 0
   when it is not a letter. *acc: it carried a mark; *up: a capital. de:
   umlauts as ae oe ue (German), else the plain vowel. As fold() in the
   generator for every letter it can meet. */
/* Cyrillic, as Latin (BGN/PCGN without marks): а..я, then ё є і ї ґ. The
   key and the screen differ only in ё (e to look up, yo to show). */
static const char *const CYR[32] = { "a", "b", "v", "g", "d", "e", "zh", "z", "i", "y", "k", "l", "m", "n", "o", "p",
                                     "r", "s", "t", "u", "f", "kh", "ts", "ch", "sh", "shch", "", "y", "", "e", "yu", "ya" };
/* The Latin of one Cyrillic letter (code point, lower case), or NULL when it is not one. */
static const char *cyr_latin(unsigned cp, int uk, int screen)
{
    if (cp >= 0x430 && cp <= 0x44F) {
        if (uk && cp == 0x433) return "h";              /* г */
        if (uk && cp == 0x438) return "y";              /* и */
        return CYR[cp - 0x430];
    }
    switch (cp) {
    case 0x451: return screen ? "yo" : "e";             /* ё */
    case 0x454: return "ye";                            /* є */
    case 0x456: return "i";                             /* і */
    case 0x457: return "yi";                            /* ї */
    case 0x491: return "g";                             /* ґ */
    case 0x45E: return "u";                             /* ў */
    }
    return NULL;
}
/* A Cyrillic letter at s: its code point in lower case and *up, or 0. */
static unsigned cyr_at(const unsigned char *s, int *up)
{
    unsigned cp;
    *up = 0;
    if (s[0] < 0xD0 || s[0] > 0xD3 || (s[1] & 0xC0) != 0x80) return 0;
    cp = ((unsigned)(s[0] & 0x1F) << 6) | (s[1] & 0x3F);
    if (cp >= 0x410 && cp <= 0x42F) { *up = 1; cp += 0x20; }
    else if (cp >= 0x400 && cp <= 0x40F) { *up = 1; cp += 0x50; }
    else if (cp == 0x490) { *up = 1; cp = 0x491; }
    return cyr_latin(cp, 0, 0) ? cp : 0;
}
/* Latin Extended-A (U+0100..U+017F): its plain letter (Polish, Czech, Baltic ...). */
static const char LATA[129] = "aaaaaacccccccc" "ddddeeeeeeeeeegggggggghhhhiiiiiiiiiiiijjkkk" "llllllllllnnnnnnnnnoooooo"
                              "oorrrrrrsssssssstttttt" "uuuuuuuuuuuuwwyyyzzzzzzs";
static int lata_upper(unsigned cp)
{
    if (cp < 0x138) return !(cp & 1);
    if (cp == 0x138 || cp == 0x149 || cp == 0x17F) return 0;
    if (cp < 0x149) return cp & 1;
    if (cp < 0x178) return !(cp & 1);
    if (cp == 0x178) return 1;
    return cp & 1;
}

int mt_xlate_needs_screen(const char *in)
{
    const unsigned char *s = (const unsigned char *)in;
    int up;
    for (; s && *s; s++)
        if (cyr_at(s, &up) || ((s[0] == 0xC4 || s[0] == 0xC5) && (s[1] & 0xC0) == 0x80)) return 1;
    return 0;
}

int mt_xlate_screen(const char *in, mt_lang_t lang, char *out, size_t cap)
{
    const unsigned char *s = (const unsigned char *)in;
    size_t n = 0;
    int uk = lang == MT_LANG_UK, up;
    if (!out || !cap) return 0;
    out[0] = 0;
    if (!in) return 0;
    /* a Ukrainian letter says which scheme (і ї є ґ) */
    for (const unsigned char *p = s; !uk && *p; p++) {
        const unsigned cp = cyr_at(p, &up);
        if (cp == 0x454 || cp == 0x456 || cp == 0x457 || cp == 0x491) uk = 1;
    }
    int prev_up = 0;
    while (*s && n + 1 < cap) {
        const unsigned cp = cyr_at(s, &up);
        if (cp) {
            const char *l = cyr_latin(cp, uk, 1);
            int nx;
            /* a word in capitals stays in capitals (ВСЁ: VSYO) */
            const int all = up && (prev_up || (cyr_at(s + 2, &nx) && nx));
            for (int k = 0; l[k] && n + 1 < cap; k++) out[n++] = (char)((up && (k == 0 || all)) ? l[k] - 32 : l[k]);
            prev_up = up;
            s += 2;
            continue;
        }
        prev_up = 0;
        if ((s[0] == 0xC4 || s[0] == 0xC5) && (s[1] & 0xC0) == 0x80 && !(s[0] == 0xC5 && (s[1] == 0x92 || s[1] == 0x93))) {
            const unsigned c = ((s[0] & 0x1F) << 6) | (s[1] & 0x3F);
            out[n++] = (char)(lata_upper(c) ? LATA[c - 0x100] - 32 : LATA[c - 0x100]);
            s += 2;
            continue;
        }
        if (s[0] == 0xCC && (s[1] == 0x81 || s[1] == 0x80)) { s += 2; continue; }   /* a stress mark */
        if (s[0] == 0xCA && s[1] == 0xBC) { out[n++] = '\''; s += 2; continue; }   /* the Ukrainian apostrophe */
        /* anything else as it is, a whole UTF-8 sequence or nothing */
        {
            const int len = s[0] < 0x80 ? 1 : s[0] >= 0xF0 ? 4 : s[0] >= 0xE0 ? 3 : s[0] >= 0xC0 ? 2 : 1;
            int k;
            for (k = 1; k < len && s[k]; k++) {}
            if (k < len) break;                                      /* cut short at the end */
            if (n + (size_t)len >= cap) break;
            memcpy(out + n, s, (size_t)len);
            n += (size_t)len;
            s += len;
        }
    }
    out[n] = 0;
    return (int)n;
}

/* A letter of Latin Extended-A at s as its plain ASCII letter (upper case kept), or 0. */
char mtx_lata(const unsigned char *s)
{
    unsigned c;
    if ((s[0] != 0xC4 && s[0] != 0xC5) || (s[1] & 0xC0) != 0x80) return 0;
    c = ((s[0] & 0x1F) << 6) | (s[1] & 0x3F);
    return (char)(lata_upper(c) ? LATA[c - 0x100] - 32 : LATA[c - 0x100]);
}

/* The Latin of one Cyrillic letter for the in-place screen fold (at most two letters). */
const char *mtx_cyr2(const unsigned char *s, int *up)
{
    const unsigned cp = cyr_at(s, up);
    const char *l = cp ? cyr_latin(cp, 0, 1) : NULL;
    return l && !strcmp(l, "shch") ? "sh" : l;
}

/* fold modes: plain Latin, German umlauts, and the two Cyrillic readings */
enum { FM_PLAIN, FM_DE, FM_RU, FM_UK };
static int fold_mode(int lang)
{
    return lang == MT_LANG_DE ? FM_DE : lang == MT_LANG_RU ? FM_RU : lang == MT_LANG_UK ? FM_UK : FM_PLAIN;
}

static int lfold(const unsigned char *s, char *o, int *no, int *acc, int *up, int mode)
{
    unsigned c = s[0], d;
    const int de = mode == FM_DE;
    *acc = 0; *up = 0; *no = 1;
    o[0] = 0;
    if (c < 0x80) {
        if (c >= 'A' && c <= 'Z') { *up = 1; o[0] = (char)(c + 32); return 1; }
        if (c >= 'a' && c <= 'z') { o[0] = (char)c; return 1; }
        return 0;
    }
    if (c == 0xC5 && (s[1] == 0x92 || s[1] == 0x93)) { *up = s[1] == 0x92; o[0] = 'o'; o[1] = 'e'; *no = 2; return 2; }
    if ((c == 0xC4 || c == 0xC5) && (s[1] & 0xC0) == 0x80) {
        const unsigned cp = ((c & 0x1F) << 6) | (s[1] & 0x3F);
        o[0] = LATA[cp - 0x100];
        *up = lata_upper(cp);
        *acc = cp != 0x141 && cp != 0x142;               /* l with stroke folds as a plain letter */
        return 2;
    }
    if (c >= 0xD0 && c <= 0xD3) {
        /* Cyrillic: a letter only to the readers of Russian and Ukrainian */
        int u;
        const unsigned cp = cyr_at(s, &u);
        const char *lat;
        if (!cp || (mode != FM_RU && mode != FM_UK)) return 0;
        lat = cyr_latin(cp, mode == FM_UK, 0);
        *no = (int)strlen(lat);
        memcpy(o, lat, (size_t)*no);
        *up = u;
        return 2;
    }
    if (c == 0xCC && (s[1] == 0x81 || s[1] == 0x80) && (mode == FM_RU || mode == FM_UK)) { *no = 0; return 2; }  /* stress */
    if (c != 0xC3 || s[1] < 0x80 || s[1] > 0xBF) return 0;
    d = s[1];
    if (d == 0x9F) { o[0] = 's'; o[1] = 's'; *no = 2; return 2; }
    if (d < 0xA0) { *up = 1; d += 0x20; }
    *acc = 1;
    switch (d) {
    case 0xA0: case 0xA1: case 0xA2: case 0xA3: case 0xA5: o[0] = 'a'; return 2;
    case 0xA4: o[0] = 'a'; if (de) { o[1] = 'e'; *no = 2; } return 2;
    case 0xA6: o[0] = 'a'; o[1] = 'e'; *no = 2; *acc = 0; return 2;
    case 0xA7: o[0] = 'c'; return 2;
    case 0xA8: case 0xA9: case 0xAA: case 0xAB: o[0] = 'e'; return 2;
    case 0xAC: case 0xAD: case 0xAE: case 0xAF: o[0] = 'i'; return 2;
    case 0xB1: o[0] = 'n'; return 2;
    case 0xB2: case 0xB3: case 0xB4: case 0xB5: o[0] = 'o'; return 2;
    case 0xB6: o[0] = 'o'; if (de) { o[1] = 'e'; *no = 2; } return 2;
    case 0xB9: case 0xBA: case 0xBB: o[0] = 'u'; return 2;
    case 0xBC: o[0] = 'u'; if (de) { o[1] = 'e'; *no = 2; } return 2;
    case 0xBD: case 0xBF: o[0] = 'y'; return 2;
    }
    *acc = 0; *up = 0;
    return 0;
}

/* ---------------------------------------------------------------- tokens */

enum { TK_WORD = 1, TK_NUM, TK_PUNCT, TK_OTHER };
enum { TF_CAP = 1, TF_UPPER = 2, TF_SPACE = 4, TF_ACC = 8, TF_HYPH = 16, TF_SYN = 32, TF_LONG = 64, TF_SSTART = 128 };

typedef struct {
    uint16_t at, len;           /* source bytes */
    uint8_t kind, fl, q;        /* q: in the question part of its sentence */
    char key[LK];
} lt_t;

static lt_t tk[LMAXT];
static int ntk;
static const char *src;
static int all_upper;

static int is_laugh(const char *k)
{
    int n = (int)strlen(k), i, ks = 0, hs = 0;
    if (streq(k, "mdr") || streq(k, "lol") || streq(k, "ptdr") || streq(k, "rsrs") || streq(k, "rsrsrs") ||
        streq(k, "kk") || streq(k, "ahah") || streq(k, "ahahah")) return 1;
    if (n < 3) return 0;
    for (i = 0; i < n; i++) {
        if (k[i] == 'k') ks++;
        else if (k[i] == 'h') hs++;
        else if (!(k[i] == 'a' || k[i] == 'e' || k[i] == 'i' || k[i] == 'u')) return 0;
    }
    return ks == n || (hs * 2 >= n - 1 && n >= 4 && k[0] == 'h');
}

/* a word with an apostrophe or hyphen the tables have whole (aujourd'hui, peut-etre, c'e) */
static int any_known(const char *k)
{
    return xfind(X->func, X->nfunc, k) >= 0 || xfind(X->lex, X->nlex, k) >= 0 ||
           (!strchr(k, '\'') && xfind(X->exc, X->nexc, k) >= 0) || xfind(X->abbr, X->nabbr, k) >= 0;
}

static lt_t *new_tok(int at, int len, int fl)
{
    lt_t *t;
    if (ntk >= LMAXT) return 0;
    t = &tk[ntk++];
    memset(t, 0, sizeof *t);
    t->at = (uint16_t)at;
    t->len = (uint16_t)len;
    t->kind = TK_WORD;
    t->fl = (uint8_t)fl;
    return t;
}

/* A chat spelling (vc, slt, cmq) becomes the words it stands for. */
static void expand_abbr(lt_t *t)
{
    int i, at = t->at, len = t->len, first = 1;
    const unsigned char *e;
    if ((t->fl & TF_UPPER) && strlen(t->key) == 1 && !all_upper) return;    /* C, D, X as letters */
    if (is_laugh(t->key)) { strcpy(t->key, "haha"); return; }
    i = xfind(X->abbr, X->nabbr, t->key);
    if (i < 0) return;
    e = (const unsigned char *)NX(XP(X->abbr[i]));
    while (*e) {
        char k[LK];
        int n = 0, acc = 0;
        while (*e == ' ') e++;
        while (*e && *e != ' ') {
            char o[4];
            int no, a, up, u = lfold(e, o, &no, &a, &up, fold_mode(XL));
            if (!u) { if (*e == '\'' && n < LK - 1) k[n++] = '\''; e++; continue; }
            if (n + no < LK) { memcpy(k + n, o, (size_t)no); n += no; }
            acc |= a;
            e += u;
        }
        k[n] = 0;
        if (!n) break;
        if (first) {
            snprintf(t->key, LK, "%s", k);
            t->fl = (uint8_t)((t->fl & ~TF_ACC) | (acc ? TF_ACC : 0));
            first = 0;
        } else {
            lt_t *s = new_tok(at, len, TF_SPACE | TF_SYN | (acc ? TF_ACC : 0));
            if (!s) break;
            snprintf(s->key, LK, "%s", k);
        }
    }
}

/* One word (letters, apostrophes, hyphens) from src[start..end), folded into
   kb with where each key byte came from. Split at hyphens and after
   elisions unless the whole word is known. */
static void push_word(int start, int end, const char *kb, const uint16_t *kpos, int kn, int fl)
{
    int i, p0 = 0;
    char key[LK];
    if (kn < LK && (strchr(kb, '-') || strchr(kb, '\''))) {
        if (any_known(kb)) {
            lt_t *t = new_tok(start, end - start, fl);
            if (t) { snprintf(t->key, LK, "%.*s", LK - 1, kb); expand_abbr(t); }
            return;
        }
    } else if (!strchr(kb, '-') && !strchr(kb, '\'')) {
        lt_t *t = new_tok(start, end - start, fl | (kn >= LK ? TF_LONG : 0));
        if (t) {
            snprintf(t->key, LK, "%.*s", LK - 1, kb);
            if (!(t->fl & TF_LONG)) expand_abbr(t);
        }
        return;
    }
    /* pieces: "l'" + "antenne", "es" + "-tu", "dell'" + "acqua" */
    for (i = 0; i <= kn; i++) {
        int cut = i == kn || kb[i] == '-' || (kb[i] == '\'' && i + 1 < kn);
        if (!cut) continue;
        {
            int pe = (i < kn && kb[i] == '\'') ? i + 1 : i;     /* the apostrophe stays with its piece */
            int n = pe - p0;
            if (n > 0 && n < LK) {
                int s0 = kpos[p0], s1 = pe < kn ? kpos[pe] : end;
                memcpy(key, kb + p0, (size_t)n);
                key[n] = 0;
                if (!(streq(key, "t") && (fl & TF_HYPH))) {     /* a-t-il */
                    lt_t *t = new_tok(s0, s1 - s0, fl);
                    if (t) {
                        snprintf(t->key, LK, "%s", key);
                        expand_abbr(t);
                    }
                }
            }
            fl = (fl & (TF_ACC | TF_UPPER)) | ((i < kn && kb[i] == '-') ? TF_HYPH : 0);
            p0 = (i < kn && kb[i] == '-') ? i + 1 : pe;
        }
    }
}

/* Tokens from src + pos until the batch is full. Returns where the next batch starts. */
static int tokenize(int pos, int de)   /* de: the fold mode */
{
    const unsigned char *s = (const unsigned char *)src;
    int space = 1, sstart = 1;
    ntk = 0;
    while (s[pos]) {
        unsigned c = s[pos];
        char o[4];
        int no, acc, up, l;
        if (ntk >= LMAXT - 8 || pos >= LBATCH) break;
        if (ntk >= LMAXT - 24 && ntk && tk[ntk - 1].kind == TK_PUNCT && strchr(".!?\n", tk[ntk - 1].key[0])) break;
        if (c == ' ' || c == '\t' || c == '\r') { space = 1; pos++; continue; }
        l = lfold(s + pos, o, &no, &acc, &up, de);
        if (l || isdig((int)c)) {
            char kb[LK * 2];
            uint16_t kpos[LK * 2];
            int k = 0, start = pos, letters = 0, uppers = 0, digits = 0, fl = space ? TF_SPACE : 0, first = 1;
            while (s[pos] && pos - start < 600) {
                int u = lfold(s + pos, o, &no, &acc, &up, de);
                if (!u && isdig(s[pos])) { o[0] = (char)s[pos]; no = 1; u = 1; digits++; }
                else if (!u && (s[pos] == '\'' || (s[pos] == 0xE2 && s[pos + 1] == 0x80 && s[pos + 2] == 0x99) ||
                                (s[pos] == 0xCA && s[pos + 1] == 0xBC))) {
                    /* an apostrophe inside a word, or closing one (po', e'); in Ukrainian part of the word (p'yat) */
                    int w = s[pos] == '\'' ? 1 : s[pos] == 0xCA ? 2 : 3, n2, a2, u2;
                    char o2[4];
                    if (!k || !(lfold(s + pos + w, o2, &n2, &a2, &u2, de) || (s[pos + w] <= ' ' && k <= 3))) break;
                    if (de == FM_RU || de == FM_UK) { o[0] = 0; no = 0; }
                    else { o[0] = '\''; no = 1; }
                    u = w;
                } else if (!u && s[pos] == '-' && k && lfold(s + pos + 1, o, &no, &acc, &up, de)) {
                    o[0] = '-'; no = 1; u = 1; acc = up = 0;
                } else if (!u && (s[pos] == '.' || s[pos] == ',' || s[pos] == ':') && digits && !letters &&
                           isdig(s[pos + 1])) {
                    o[0] = (char)s[pos]; no = 1; u = 1;
                }
                if (!u) break;
                if (o[0] >= 'a' && o[0] <= 'z') {
                    letters++;
                    if (up) uppers++;
                    if (first && up) fl |= TF_CAP;
                    if (acc) fl |= TF_ACC;
                }
                first = 0;
                /* three or more of one letter: two (oiii, simmm) */
                if (!(no == 1 && k >= 2 && kb[k - 1] == o[0] && kb[k - 2] == o[0] && o[0] >= 'a')) {
                    int j;
                    for (j = 0; j < no && k < LK * 2 - 1; j++) { kpos[k] = (uint16_t)pos; kb[k++] = o[j]; }
                }
                pos += u;
            }
            kb[k] = 0;
            if (letters && uppers == letters && letters > 1) fl |= TF_UPPER;
            if (letters == 1 && uppers == 1) fl |= TF_UPPER;
            if (sstart) fl |= TF_SSTART;
            sstart = 0;
            if (digits && !letters) {
                lt_t *t = new_tok(start, pos - start, fl);
                if (t) { t->kind = TK_NUM; snprintf(t->key, LK, "%.*s", LK - 1, kb); }
            } else if (digits || pos - start > 40) {
                lt_t *t = new_tok(start, pos - start, fl | TF_LONG);     /* LS01, V3: names */
                if (t) snprintf(t->key, LK, "%.*s", LK - 1, kb);
            } else {
                push_word(start, pos, kb, kpos, k, fl);
            }
        } else if (c < 0x80) {
            lt_t *t = new_tok(pos, 1, space ? TF_SPACE : 0);
            if (!t) break;
            t->kind = TK_PUNCT;
            t->key[0] = (char)c;
            pos++;
            if (c == '.' || c == '!' || c == '?') {
                while (s[pos] == '.' || s[pos] == '!' || s[pos] == '?') {
                    if (s[pos] == '?') t->key[0] = '?';
                    else if (s[pos] == '!' && t->key[0] != '?') t->key[0] = '!';
                    pos++;
                }
                t->len = (uint16_t)(pos - t->at);
                sstart = 1;
            }
            if (c == '\n') { space = 1; sstart = 1; continue; }
        } else {
            int start = pos;
            lt_t *t;
            pos++;
            while (s[pos] >= 0x80 && pos - start < 600 && !lfold(s + pos, o, &no, &acc, &up, de)) pos++;
            t = new_tok(start, pos - start, space ? TF_SPACE : 0);
            if (!t) break;
            t->kind = TK_OTHER;
        }
        space = 0;
    }
    /* the question is the last clause of a sentence that ends in ? */
    {
        int i = 0;
        while (i < ntk) {
            int j = i, from = i, k;
            while (j < ntk && !(tk[j].kind == TK_PUNCT && strchr(".!?\n", tk[j].key[0]))) j++;
            if (j < ntk && tk[j].key[0] == '?') {
                for (k = j - 1; k > i; k--)
                    if (tk[k].kind == TK_PUNCT && strchr(",;:", tk[k].key[0])) { from = k + 1; break; }
                for (k = from; k <= j && k < ntk; k++) tk[k].q = 1;
            }
            i = j + 1;
        }
    }
    return pos;
}

/* ----------------------------------------------------------------- words */

typedef struct { int16_t lex, mod; uint8_t pos, tense, person, pl, encl; } rd_t;   /* encl: ENCL[encl - 1] on its end */

/* readings of a folded word: the exceptions, the lexicon, then the rules
   (as the generator's Reader, which decides what goes in the exceptions) */
static int analyze_core(const char *k, rd_t *o)
{
    int n = 0, i, r, kl = (int)strlen(k);
    i = xfind(X->exc, X->nexc, k);
    for (; i >= 0 && i < X->nexc && n < 2 && streq(XP(X->exc[i]), k); i++) {
        const uint32_t w = X->exc[i];
        o[n].lex = (int16_t)X->excl[i];
        o[n].mod = -1;
        o[n].encl = 0;
        o[n].pos = (uint8_t)LX_POS(X->excl[i]);
        o[n].tense = (uint8_t)((w >> 22) & 15);
        o[n].person = (uint8_t)((w >> 26) & 7);
        o[n].pl = (uint8_t)((w >> 29) & 1);
        n++;
    }
    if (n) return n;
    i = xfind(X->lex, X->nlex, k);
    for (; i >= 0 && i < X->nlex && n < 3 && streq(lx_key(i), k); i++) {
        o[n].lex = (int16_t)i;
        o[n].mod = -1;
        o[n].encl = 0;
        o[n].pos = (uint8_t)LX_POS(i);
        o[n].tense = (uint8_t)(o[n].pos == LP_V ? T_INF : T_NONE);
        o[n].person = o[n].pl = 0;
        n++;
    }
    if (n) return n;
    for (r = 0; r < X->nrule && n < 3; r++) {
        const uint8_t last = RLAST[XL - MT_LANG_FIRST_CARD][r];
        if (last && (!kl || (uint8_t)k[kl - 1] != last)) continue;
        const char *fpre = XP(X->rule[r]), *lpre = NX(fpre), *fsuf = NX(lpre), *lsuf = NX(fsuf);
        const int fp = (int)strlen(fpre), fs = (int)strlen(fsuf), info = (int)X->rinfo[r], pos = info & 15;
        char cand[LK * 2];
        int stem = kl - fp - fs, j, seen = 0, li;
        if (stem < 2 || memcmp(k + kl - fs, fsuf, (size_t)fs) || memcmp(k, fpre, (size_t)fp)) continue;
        for (j = 0; j < n; j++) if (o[j].pos == pos) seen = 1;
        if (seen) continue;
        snprintf(cand, sizeof cand, "%s%.*s%s", lpre, stem, k + fp, lsuf);
        li = lex_pos(cand, pos);
        if (li < 0) continue;
        o[n].lex = (int16_t)li;
        o[n].mod = -1;
        o[n].encl = 0;
        o[n].pos = (uint8_t)pos;
        o[n].tense = (uint8_t)((info >> 4) & 15);
        o[n].person = (uint8_t)((info >> 8) & 7);
        o[n].pl = (uint8_t)((info >> 11) & 1);
        n++;
    }
    return n;
}

/* Italian object pronouns on the end of a command or an infinitive (dimmi,
   mandami, sentirti, aspettami): the verb, with the object to say after it. */
static const char *const ENCL[][2] = {
    { "glielo", "it to him" }, { "gliela", "it to him" }, { "melo", "it to me" }, { "mela", "it to me" },
    { "telo", "it to you" }, { "gli", "him" }, { "mi", "me" }, { "ti", "you" }, { "ci", "us" }, { "vi", "you" },
    { "lo", "it" }, { "la", "it" }, { "li", "them" }, { "le", "them" }, { "ne", "some" }, { "si", "" } };
#define NENCL ((int)(sizeof ENCL / sizeof ENCL[0]))

static int enclitic(const char *k, int kl, rd_t *o)
{
    int e;
    for (e = 0; e < NENCL; e++) {
        const int el = (int)strlen(ENCL[e][0]);
        char st[LK + 2];
        int sl = kl - el, c;
        if (sl < 2 || strcmp(k + sl, ENCL[e][0])) continue;
        /* the stem as a command (manda), an infinitive (sentir + e), or a short command (dim + mi: di) */
        for (c = 0; c < 3; c++) {
            rd_t r[3];
            int n, j;
            if (c == 0) snprintf(st, sizeof st, "%.*s", sl, k);
            else if (c == 1) snprintf(st, sizeof st, "%.*se", sl, k);
            else if (sl >= 2 && k[sl - 1] == k[sl] && k[sl - 1] == ENCL[e][0][0]) snprintf(st, sizeof st, "%.*s", sl - 1, k);
            else continue;
            n = analyze_core(st, r);
            for (j = 0; j < n; j++) {
                if (r[j].pos != LP_V) continue;
                o[0] = r[j];
                o[0].tense = (uint8_t)(c == 1 ? T_INF : T_IMP);
                o[0].person = 2;
                o[0].encl = (uint8_t)(e + 1);
                return 1;
            }
        }
    }
    return 0;
}

static int analyze(const char *k, rd_t *o)
{
    int n = analyze_core(k, o), kl = (int)strlen(k), s;
    if (n) return n;
    if (XL == MT_LANG_IT && kl >= 5 && kl < LK && enclitic(k, kl, o)) return 1;
    /* a doubled last letter left from a long one (simm, oii) */
    if (kl > 2 && k[kl - 1] == k[kl - 2]) {
        char b[LK];
        snprintf(b, sizeof b, "%.*s", kl - 1, k);
        n = analyze_core(b, o);
        if (n) return n;
    }
    /* German and Dutch compounds: a known word, then a known noun (Funk|geraet, Akku|ladung) */
    if (GERMANIC(XL) && kl >= 7 && kl < LK) {
        for (s = 3; s <= kl - 3; s++) {
            rd_t h[3];
            int nh = analyze_core(k + s, h), j, m = -1, cut;
            for (j = 0; j < nh; j++) if (h[j].pos == LP_N) break;
            if (j == nh) continue;
            /* the first part, with or without its joint (s, n, en, es, e) */
            for (cut = 0; cut <= 2 && m < 0; cut++) {
                char a[LK];
                int al = s - cut;
                if (al < 3) break;
                if (cut == 1 && !(k[s - 1] == 's' || k[s - 1] == 'n' || k[s - 1] == 'e')) continue;
                if (cut == 2 && !((k[s - 2] == 'e' && (k[s - 1] == 'n' || k[s - 1] == 's')))) continue;
                snprintf(a, sizeof a, "%.*s", al, k);
                if ((m = lex_pos(a, LP_N)) < 0 && (m = lex_pos(a, LP_A)) < 0) {
                    char v[LK + 2];
                    snprintf(v, sizeof v, "%sen", a);
                    m = lex_pos(v, LP_V);
                }
            }
            if (m < 0) continue;
            o[0] = h[j];
            o[0].mod = (int16_t)m;
            return 1;
        }
    }
    return 0;
}

/* ----------------------------------------------------------------- items */

enum { IT_WORD = 1, IT_FUNC, IT_NUM, IT_PUNCT, IT_OTHER };
typedef struct {
    uint8_t kind, t0, nt, nrd, sel, done, negd, q;
    int16_t fn;
    rd_t rd[3];
} li_t;

static li_t it[LMAXT];
static int nit;

static const rd_t *RD(int i) { return (i >= 0 && i < nit && it[i].kind == IT_WORD && it[i].nrd) ? &it[i].rd[it[i].sel] : 0; }
static int is_pos(int i, int pos) { const rd_t *r = RD(i); return r && r->pos == pos; }
static int has_pos(int i, int pos)
{
    int k;
    if (i < 0 || i >= nit || it[i].kind != IT_WORD) return 0;
    for (k = 0; k < it[i].nrd; k++) if (it[i].rd[k].pos == pos) return 1;
    return 0;
}
static void prefer(int i, int pos)
{
    int k;
    for (k = 0; k < it[i].nrd; k++) if (it[i].rd[k].pos == pos) { it[i].sel = (uint8_t)k; return; }
}
static int fclass(int i) { return (i >= 0 && i < nit && it[i].kind == IT_FUNC) ? FCLASS(it[i].fn) : 0; }
static int punct(int i) { return i < 0 || i >= nit || it[i].kind == IT_PUNCT; }
static int clause_end_at(int i)
{
    return punct(i) || fclass(i) == C_CONJ || (fclass(i) == C_Q && FHAS(it[i].fn, F_SUB) && !it[i].q);
}
static int is_fin(int t) { return t != T_INF && t != T_GER && t != T_PART && t != T_NONE; }

/* the clause's tokens t0..t1 are words: is t..t+n-1 all of it? */
static int whole_clause(int t, int n)
{
    int a = t - 1, b = t + n;
    while (a >= 0 && tk[a].kind == TK_OTHER) a--;
    while (b < ntk && tk[b].kind == TK_OTHER) b++;
    return (a < 0 || tk[a].kind == TK_PUNCT) && (b >= ntk || tk[b].kind == TK_PUNCT);
}
static int clause_start_tok(int t)
{
    int a = t - 1;
    while (a >= 0 && tk[a].kind == TK_OTHER) a--;
    return a < 0 || tk[a].kind == TK_PUNCT;
}

/* Polish: the first person ends in ę (jadę, słyszę), the third in e (pisze, potrzebuje). Typed with
   no Polish letters at all, a verb in -e that opens its clause (after nie, an adverb) is the first. */
static void pl_first_person(li_t *I, int t, int marks)
{
    const unsigned char *e = (const unsigned char *)src + tk[t].at + tk[t].len;
    const int kl = (int)strlen(tk[t].key), ogonek = tk[t].len >= 2 && e[-2] == 0xC4 && (e[-1] | 1) == 0x99;
    int k;
    if (kl < 3 || tk[t].key[kl - 1] != 'e') return;
    if (!ogonek) {
        if (marks) return;
        for (k = nit - 1; k >= 0 && it[k].kind != IT_PUNCT && fclass(k) != C_CONJ; k--)
            if (it[k].kind != IT_FUNC || (fclass(k) != C_NEG && fclass(k) != C_ADV && fclass(k) != C_CLIT)) return;
    }
    for (k = 0; k < I->nrd; k++)
        if (I->rd[k].pos == LP_V && I->rd[k].person == 3 && (I->rd[k].tense == T_PRES || I->rd[k].tense == T_FUT))
            I->rd[k].person = 1;
}
static int pl_marks(void)
{
    const unsigned char *s = (const unsigned char *)src;
    for (; *s; s++)
        if ((s[0] == 0xC4 && (s[1] >= 0x84 && s[1] <= 0x87)) || (s[0] == 0xC4 && (s[1] == 0x98 || s[1] == 0x99)) ||
            (s[0] == 0xC5 && ((s[1] >= 0x81 && s[1] <= 0x84) || s[1] == 0x9A || s[1] == 0x9B || (s[1] >= 0xB9 && s[1] <= 0xBC))) ||
            (s[0] == 0xC3 && (s[1] == 0xB3 || s[1] == 0x93)))
            return 1;
    return 0;
}

static void build_items(void)
{
    int t = 0;
    const int marks = XL == MT_LANG_PL && pl_marks();
    nit = 0;
    while (t < ntk && nit < LMAXT) {
        li_t *I = &it[nit];
        memset(I, 0, sizeof *I);
        I->t0 = (uint8_t)t;
        I->nt = 1;
        I->fn = -1;
        I->q = tk[t].q;
        if (tk[t].kind == TK_PUNCT) { I->kind = IT_PUNCT; nit++; t++; continue; }
        if (tk[t].kind == TK_NUM) { I->kind = IT_NUM; nit++; t++; continue; }
        if (tk[t].kind == TK_OTHER || (tk[t].fl & TF_LONG)) { I->kind = IT_OTHER; nit++; t++; continue; }
        /* the longest phrase of the table */
        {
            int n, best = -1, bestn = 0;
            for (n = 7; n >= 1 && best < 0; n--) {
                char key[LK * 7 + 8];
                int k, len = 0, acc = 0, ok = 1, mis = 0, f;
                if (t + n > ntk) continue;
                for (k = 0; k < n; k++) {
                    const lt_t *T = &tk[t + k];
                    const char *sep = !k ? "" : (T->fl & TF_HYPH) ? "-" : (len && key[len - 1] == '\'') ? "" : " ";
                    if (T->kind != TK_WORD || (T->fl & TF_LONG)) { ok = 0; break; }
                    len += snprintf(key + len, sizeof key - (size_t)len, "%s%s", sep, T->key);
                    if (len >= (int)sizeof key) { ok = 0; break; }
                    if (T->fl & TF_ACC) acc = 1;
                }
                if (!ok) continue;
                f = func_find(key, acc, &mis);
                if (f < 0) continue;
                if (FHAS(f, F_A) && !whole_clause(t, n)) continue;
                if (FHAS(f, F_S) && !clause_start_tok(t)) continue;
                if (FHAS(f, F_HYPH) && !(tk[t].fl & TF_HYPH)) continue;
                /* nó: the node, not "in the"; está: is, not "esta" (this) */
                if (n == 1 && mis) {
                    rd_t r[3];
                    if (analyze_core(key, r)) continue;
                }
                /* after a determiner or a preposition, a word of the lexicon is a noun (per l'aiuto), not "help!" */
                if (n == 1 && FCLASS(f) == C_INTJ && nit > 0 && it[nit - 1].kind == IT_FUNC &&
                    (FCLASS(it[nit - 1].fn) == C_DET || FCLASS(it[nit - 1].fn) == C_PREP) && lex_pos(key, LP_N) >= 0) continue;
                /* German and Dutch: a capital inside the sentence is a noun (fuer deine Hilfe), not "help!" */
                if (n == 1 && FCLASS(f) == C_INTJ && GERMANIC(XL) && (tk[t].fl & TF_CAP) &&
                    !(tk[t].fl & TF_SSTART) && !whole_clause(t, 1) && lex_pos(key, LP_N) >= 0) continue;
                if (n == 1 && FCLASS(f) == C_PRON && FHAS(f, F_HYPH) && !(tk[t].fl & TF_HYPH)) continue;
                best = f;
                bestn = n;
            }
            if (best >= 0) {
                I->kind = IT_FUNC;
                I->fn = (int16_t)best;
                I->nt = (uint8_t)bestn;
                nit++;
                t += bestn;
                continue;
            }
        }
        I->kind = IT_WORD;
        I->nrd = (uint8_t)analyze(tk[t].key, I->rd);
        if (XL == MT_LANG_PL) pl_first_person(I, t, marks);
        nit++;
        t++;
    }
}

static int nominal(int i);

/* Which reading each word takes, from its neighbours. */
static void choose(void)
{
    int i;
    /* a determiner with no noun after it that is also a verb (esta: está, sein: be) */
    for (i = nit - 1; i >= 0; i--) {
        rd_t r[3];
        int n, v, kept = 0;
        const int after_subj = fclass(i - 1) == C_PRON;      /* we zijn, sie sind */
        if (fclass(i) != C_DET || FHAS(it[i].fn, F_OBJ) || FHAS(it[i].fn, F_DEF) || it[i].nt != 1) continue;
        if (nominal(i + 1) && !after_subj) continue;
        if (RD(i + 1) && RD(i + 1)->pos == LP_V && !after_subj) continue;   /* das ist: that is */
        /* the verb as the exceptions or the lexicon have it, not a guess of the rules (dat: not "date") */
        {
            const char *k = tk[it[i].t0].key;
            kept = xfind(X->exc, X->nexc, k) >= 0 || lex_pos(k, LP_V) >= 0;
        }
        n = analyze(tk[it[i].t0].key, r);
        for (v = 0; v < n; v++) if (r[v].pos == LP_V && (kept || verb_role(r[v].lex))) break;   /* esta: está */
        if (v == n) continue;
        it[i].kind = IT_WORD;
        it[i].nrd = (uint8_t)n;
        memcpy(it[i].rd, r, sizeof r);
        it[i].sel = (uint8_t)v;
    }
    for (i = 0; i < nit; i++) {
        li_t *I = &it[i];
        int pc = fclass(i - 1), cap;
        if (I->kind != IT_WORD || I->nrd < 2) continue;
        cap = (tk[I->t0].fl & TF_CAP) && !(tk[I->t0].fl & TF_SSTART);
        if (XL == MT_LANG_DE && cap && has_pos(i, LP_N)) { prefer(i, LP_N); continue; }
        if (pc == C_DET || pc == C_NUM || (pc == C_PREP && !has_pos(i, LP_V)) ||
            (i > 0 && (is_pos(i - 1, LP_A) || it[i - 1].kind == IT_NUM) && fclass(i - 2) == C_DET)) {
            if (has_pos(i, LP_N)) prefer(i, LP_N); else if (has_pos(i, LP_A)) prefer(i, LP_A);
            continue;
        }
        if (pc == C_PH || pc == C_CL) {
            /* besoin d' aide: after a phrase that ends in "of", a noun */
            const char *pk = tk[it[i - 1].t0 + it[i - 1].nt - 1].key;
            const size_t pn = strlen(pk);
            if ((streq(pk, "de") || streq(pk, "di") || (pn && pk[pn - 1] == '\'')) && has_pos(i, LP_N)) { prefer(i, LP_N); continue; }
        }
        if (pc == C_PREP) {
            /* para ir, pour aller, per andare: an infinitive; else a noun */
            int k;
            for (k = 0; k < I->nrd; k++) if (I->rd[k].pos == LP_V && I->rd[k].tense == T_INF) { I->sel = (uint8_t)k; break; }
            if (k == I->nrd && has_pos(i, LP_N)) prefer(i, LP_N);
            continue;
        }
        if (pc == C_PRON || pc == C_CLIT || pc == C_NEG || is_pos(i - 1, LP_V)) {
            if (has_pos(i, LP_V) && !(is_pos(i - 1, LP_V) && has_pos(i, LP_A) && !(verb_role(RD(i - 1)->lex) & (R_CAN | R_WANT | R_MUST | R_NEED | R_GOING | R_HAVE | R_PROG | R_WILL | R_SHOULD | R_MAY)))) {
                prefer(i, LP_V);
                continue;
            }
        }
        /* a casa velha: after a noun the adjective, unless the word is first of all a verb (est, é) */
        if (is_pos(i - 1, LP_N) && has_pos(i, LP_A) && ROMANCE(XL) &&
            !(I->rd[0].pos == LP_V && is_fin(I->rd[0].tense))) {
            prefer(i, LP_A);
            continue;
        }
    }
    /* une nouvelle antenne: between a determiner and a noun, the adjective */
    for (i = 1; i + 1 < nit; i++) {
        const int pc = fclass(i - 1);
        if (it[i].kind == IT_WORD && it[i].nrd > 1 && has_pos(i, LP_A) && (pc == C_DET || pc == C_NUM) &&
            has_pos(i + 1, LP_N) && !has_pos(i + 1, LP_A))
            prefer(i, LP_A);
    }
    /* French: avoir or être and a past participle typed without its accent (j'ai achete, ils sont arrives) */
    if (XL == MT_LANG_FR)
        for (i = 1; i < nit; i++) {
            const rd_t *r = RD(i), *b = RD(i - 1);
            if (b && b->pos == LP_V && (verb_role(b->lex) & (R_BE | R_HAVE)) && r && r->pos == LP_V && r->tense == T_PRES) {
                const char *k = tk[it[i].t0].key, *l = lx_key(r->lex);
                const size_t kl = strlen(k), ll = strlen(l);
                if (ll > 2 && l[ll - 2] == 'e' && l[ll - 1] == 'r' && kl >= ll - 1 && !strncmp(k, l, ll - 1) &&
                    (kl == ll - 1 || streq(k + ll - 1, "s") || streq(k + ll - 1, "e") || streq(k + ll - 1, "es")))
                    it[i].rd[it[i].sel].tense = T_PART;
            }
        }
}

/* ------------------------------------------------------------- English out */

typedef struct { uint16_t off; uint8_t len, fl; } lc_t;
enum { CF_GLUE = 1, CF_PASS = 2 };
static char arena[LARENA];
static int narena;
static lc_t ch[LCH];
static int nch;

static int emitn(const char *s, int n, int fl)
{
    if (n > 255) {
        int first = emitn(s, 255, fl);
        while (first >= 0 && (n -= 255) > 0) {
            s += 255;
            if (emitn(s, n > 255 ? 255 : n, CF_GLUE | CF_PASS) < 0) break;
        }
        return first;
    }
    if (n <= 0 || nch >= LCH || narena + n > LARENA) return -1;
    memcpy(arena + narena, s, (size_t)n);
    ch[nch].off = (uint16_t)narena;
    ch[nch].len = (uint8_t)n;
    ch[nch].fl = (uint8_t)fl;
    narena += n;
    return nch++;
}
static int emit(const char *s) { return (s && *s) ? emitn(s, (int)strlen(s), 0) : -1; }
static void move_chunk(int from, int to)
{
    lc_t c;
    if (from < 0 || to < 0 || from >= nch || to >= nch || from == to) return;
    c = ch[from];
    if (from > to) memmove(&ch[to + 1], &ch[to], sizeof(lc_t) * (size_t)(from - to));
    else memmove(&ch[from], &ch[from + 1], sizeof(lc_t) * (size_t)(to - from));
    ch[to] = c;
}
/* the source of item i as it was written (a Cyrillic name in Latin) */
static void emit_src(int i, int fl)
{
    const lt_t *a = &tk[it[i].t0], *b = &tk[it[i].t0 + it[i].nt - 1];
    const int n = b->at + b->len - a->at;
    if (CYRL(XL) && n < 120) {
        char in[128], out[256];
        memcpy(in, src + a->at, (size_t)n);
        in[n] = 0;
        mt_xlate_screen(in, (mt_lang_t)XL, out, sizeof out);
        emitn(out, (int)strlen(out), fl | CF_PASS);
        return;
    }
    emitn(src + a->at, n, fl | CF_PASS);
}

/* "a" before a vowel sound: "an" */
static void fix_article(int c)
{
    if (c >= 0 && c + 1 < nch && ch[c].len == 1 && arena[ch[c].off] == 'a') {
        char w[40];
        int n = ch[c + 1].len < 39 ? ch[c + 1].len : 39;
        memcpy(w, arena + ch[c + 1].off, (size_t)n);
        w[n] = 0;
        if (mtx_vowel_start(w) && narena + 2 <= LARENA) {
            memcpy(arena + narena, "an", 2);
            ch[c].off = (uint16_t)narena;
            ch[c].len = 2;
            narena += 2;
        }
    }
}

/* ------------------------------------------------------------- the writer */

static struct {
    const char *subj;           /* subject pronoun waiting for its verb */
    int subj_a;                 /* its English person (1 I, 2 you, 3 he, 4 we, 5 you, 6 they) */
    int np, np_a, np_ch;        /* a noun phrase that may be the next verb's subject; its first chunk */
    int neg, never;             /* "not" waiting for a verb */
    int ncl;
    const char *cl[3];          /* object pronouns waiting for a verb */
    int cl_first;               /* the clause opened with one (me manda) */
    int ch0;                    /* the first English piece of the clause */
    int had_verb, qword, q_done, sub, after_prep;
} cs;
static int sent_q;
static int at_num;              /* "at" was said before a number: "hours" after it goes */

static void clause_reset(int sub)
{
    const int qd = cs.q_done;
    memset(&cs, 0, sizeof cs);
    cs.np_ch = -1;
    cs.sub = sub;
    cs.ch0 = nch;
    cs.q_done = sub ? qd : 0;
}

static int agree(int p) { return (p == 1 || p == 7) ? 1 : p == 3 ? 3 : 0; }
static const char *be_form(int past, int a)
{
    if (past) return (a == 1 || a == 3) ? "was" : "were";
    return a == 1 ? "am" : a == 3 ? "is" : "are";
}
static int first_is(const char *g, const char *w)
{
    size_t n = strlen(w);
    return !strncmp(g, w, n) && (g[n] == 0 || g[n] == ' ' || g[n] == '+');
}

/* verb + its object: "wait+for" says "for" only before one, "let * know" puts it at the star */
static void emit_verb(const char *g, int form, const char *obj)
{
    char w[48], buf[112];
    const char *rest = g;
    while (*rest && *rest != ' ' && *rest != '+') rest++;
    mtx_en_form(g, form, w, sizeof w);
    if (*rest == '+') {
        if (obj) snprintf(buf, sizeof buf, "%s %s %s", w, rest + 1, obj);
        else snprintf(buf, sizeof buf, "%s", w);
    } else if (strchr(rest, '*')) {
        const char *star = strchr(rest, '*');
        snprintf(buf, sizeof buf, "%s%.*s%s%s", w, (int)(star - rest), rest, obj ? obj : "us", star + 1);
    } else if (obj) {
        snprintf(buf, sizeof buf, "%s%s %s", w, rest, obj);
    } else {
        snprintf(buf, sizeof buf, "%s%s", w, rest);
    }
    emit(buf);
}

enum { TC_PRES, TC_PAST, TC_FUT, TC_COND, TC_IMP };
enum { M_SIMPLE, M_PERF, M_PASS, M_PROG, M_GOING, M_CAN, M_WANT, M_MUST, M_SHOULD, M_WILL, M_MAY, M_NEED, M_CHAIN,
       M_JUST };

static int tense_class(int t)
{
    switch (t) {
    case T_PRET: case T_IMPF: case T_SUBJI: case T_PLUP: return TC_PAST;
    case T_FUT: return TC_FUT;
    case T_COND: return TC_COND;
    case T_IMP: return TC_IMP;
    }
    return TC_PRES;
}

/* Write a verb group: subject (or the noun phrase at subj_ch), the finite
   word with tense, not and question order, then the rest. */
static const char *vg_adv;      /* an adverb inside the verb group (ho gia visto: already) */
static void vg_write(const char *subj, int subj_ch, int a, int tc, int neg, int never, int q, int mode,
                     const char *g1, const char *g2, const char *obj)
{
    const char *head = g1, *mid = 0, *mainv = 0;
    int hk = 0, mform = MTX_BASE, fin_ch = -1;   /* hk: 0 a verb, 1 be, 2 have (aux), 3 modal */
    char fin[40] = "", aux2[24] = "";
    int head_said = 0, do_sup = 0, just = 0;
    switch (mode) {
    case M_JUST: head = g2; tc = TC_PAST; just = 1; break;
    case M_PERF:
        if (tc == TC_PRES) { head = g2; tc = TC_PAST; }
        else { head = "have"; hk = 2; mainv = g2; mform = MTX_PP; }
        break;
    case M_PASS: head = "be"; mainv = g2; mform = MTX_PP; break;
    case M_PROG: head = "be"; mainv = g2; mform = MTX_ING; break;
    case M_GOING: head = "be"; mid = "going to"; mainv = g2; break;
    case M_CAN: head = "can"; hk = 3; mainv = g2; break;
    case M_SHOULD: head = "should"; hk = 3; mainv = g2; break;
    case M_WILL: head = "will"; hk = 3; mainv = g2; break;
    case M_MAY: head = "may"; hk = 3; mainv = g2; break;
    case M_MUST: head = "have"; mid = "to"; mainv = g2; break;
    case M_WANT: head = "want"; mid = "to"; mainv = g2; break;
    case M_NEED: head = "need"; mid = "to"; mainv = g2; break;
    case M_CHAIN: mid = "to"; mainv = g2; break;
    }
    if (!head || !*head) head = "do";
    if (hk == 0 && first_is(head, "be")) hk = 1;
    if (hk == 0 && (first_is(head, "can") || first_is(head, "must") || first_is(head, "should"))) hk = 3;
    if (tc == TC_IMP) {
        if (neg) emit("don't");
        if (never) emit("never");
        if (hk == 1) emit("be");
        else if (hk == 3) emit_verb(head, MTX_BASE, mainv ? 0 : obj);
        else emit_verb(head, MTX_BASE, mainv ? 0 : obj);
        if (mid) emit(mid);
        if (mainv) emit_verb(mainv, mform, obj);
        return;
    }
    if (hk == 3) {
        const char *m = head;
        if (tc == TC_PAST || tc == TC_COND)
            m = first_is(head, "can") ? "could" : first_is(head, "will") ? "would" : first_is(head, "may") ? "might" :
                first_is(head, "must") ? "had to" : head;
        if (tc == TC_FUT && first_is(head, "can")) { m = "will"; snprintf(aux2, sizeof aux2, "be able to"); }
        snprintf(fin, sizeof fin, "%s", m);
        head_said = 1;
    } else if (tc == TC_FUT || tc == TC_COND) {
        snprintf(fin, sizeof fin, "%s", tc == TC_FUT ? "will" : "would");
        if (hk == 1) { snprintf(aux2, sizeof aux2, "be"); head_said = 1; }
        else if (hk == 2) { snprintf(aux2, sizeof aux2, "have"); head_said = 1; }
    } else if (hk == 1) {
        snprintf(fin, sizeof fin, "%s", be_form(tc == TC_PAST, a));
        head_said = 1;
    } else if (hk == 2) {
        snprintf(fin, sizeof fin, "%s", tc == TC_PAST ? "had" : a == 3 ? "has" : "have");
        head_said = 1;
    } else if ((neg && !never) || (q && (subj || subj_ch >= 0))) {
        snprintf(fin, sizeof fin, "%s", tc == TC_PAST ? "did" : a == 3 ? "does" : "do");
        do_sup = 1;
    }
    if (neg && !never && fin[0]) mtx_negate(fin, sizeof fin);
    if (q && fin[0] && (subj || subj_ch >= 0)) {
        fin_ch = emit(fin);
        if (subj) emit(subj);
        else move_chunk(fin_ch, subj_ch);
    } else {
        if (subj) emit(subj);
        if (fin[0]) emit(fin);
    }
    if (never) emit("never");
    if (just) emit("just");
    if (vg_adv) { emit(vg_adv); vg_adv = 0; }
    if (aux2[0]) emit(aux2);
    if (!head_said) {
        int f = (do_sup || fin[0]) ? MTX_BASE : tc == TC_PAST ? MTX_PAST : a == 3 ? MTX_S : MTX_BASE;
        emit_verb(head, f, mainv ? 0 : obj);
    }
    if (mid) emit(mid);
    if (mainv) emit_verb(mainv, mform, obj);
    else if (head_said && obj && hk != 3) emit(obj);
    else if (head_said && obj) emit(obj);
}

static const char *pron_for(int p, const rd_t *v, int q)
{
    static const char *const IT_VERB[] = { "be", "rain", "snow", "work", "seem", "happen", "cost", "matter",
                                           "break", "fail", "charge", "start", "begin", "end", "finish", "hurt" };
    unsigned k;
    switch (p) {
    case 1: case 7: return "I";
    case 2: case 5: return "you";
    case 4: return "we";
    case 6: return "they";
    }
    if (q && XL == MT_LANG_PT) return "you";        /* você takes the third person */
    if (v && (verb_role(v->lex) & R_IT)) return "it";
    if (v)
        for (k = 0; k < sizeof IT_VERB / sizeof IT_VERB[0]; k++)
            if (first_is(lx_gloss(v->lex), IT_VERB[k])) return "it";
    return "he";
}

static int nominal(int i)
{
    return i < nit && (it[i].kind == IT_NUM || has_pos(i, LP_N) || has_pos(i, LP_A) ||
                       (it[i].kind == IT_WORD && !it[i].nrd) || it[i].kind == IT_OTHER ||
                       (fclass(i) == C_DET && FHAS(it[i].fn, F_POSS)) || (fclass(i) == C_ADV && FHAS(it[i].fn, F_INT)));
}

static void noun_en(const rd_t *r, int pl, char *b, int cap)
{
    char g[64];
    const char *gl = lx_gloss(r->lex);
    if (r->pos == LP_V && r->tense == T_PART) { mtx_en_form(gl, MTX_PP, b, cap); return; }
    if (r->pos == LP_V && r->tense == T_GER) { mtx_en_form(gl, MTX_ING, b, cap); return; }
    if (r->mod >= 0) snprintf(g, sizeof g, "%.24s %.36s", lx_gloss(r->mod), gl);
    else snprintf(g, sizeof g, "%.60s", gl);
    if (pl && r->pos == LP_N && !(LX_FLAGS(r->lex) & LEX_UNCOUNT)) mtx_en_plural(g, b, cap);
    else snprintf(b, (size_t)cap, "%s", g);
}

static int vp(int i, int end);

/* A noun phrase from item i: determiner, adjectives, the noun, adjectives
   after it (Romance), written in English order. Returns the item after it. */
/* "of" (de, di, do, du, della ...) */
static int is_of(int i)
{
    if (fclass(i) == C_DET && FHAS(it[i].fn, F_AMB)) return 1;      /* du, des after a noun */
    if (fclass(i) != C_PREP) return 0;
    {
        const char *e = fn_en(it[i].fn);
        return streq(e, "of") || streq(e, "of the");
    }
}
static int bare_noun(int i) { return is_pos(i, LP_N) && it[i].kind == IT_WORD; }
static int name_word(int i)
{
    return i < nit && it[i].kind == IT_WORD && !it[i].nrd && (tk[it[i].t0].fl & TF_CAP);
}

static int np(int i, int end)
{
    int j = i, k, npre = 0, npost = 0, head = -1, pl = 0, first = -1, det_c = -1, mod = -1, owner = -1;
    int pre[4], post[3];
    const char *det = 0;
    char b[80];
    if (fclass(j) == C_DET) {
        const int f = it[j].fn;
        if (!nominal(j + 1)) {
            /* standing alone: a pronoun (das: that, isso) */
            first = emit(fn_alt(f));
            if (!cs.had_verb && !cs.np && !cs.after_prep) {
                cs.np = 1;
                cs.np_a = (FHAS(f, F_PL) || FHAS(f, F_YOU)) ? 0 : 3;
                cs.np_ch = first;
            }
            cs.after_prep = 0;
            return j + 1;
        }
        if (FHAS(f, F_DEF) && fclass(j + 1) == C_DET && FHAS(it[j + 1].fn, F_POSS)) j++;   /* o meu: my */
        det = fn_en(it[j].fn);
        /* du, des: "some", or "of the" after a noun; "any" in a question or a negative clause */
        if (FHAS(it[j].fn, F_AMB)) {
            if (j > 0 && (is_pos(j - 1, LP_N) || (it[j - 1].kind == IT_WORD && !it[j - 1].nrd))) det = fn_alt(it[j].fn);
            else if (it[j].q || cs.neg) det = "any";
        }
        pl = FHAS(it[j].fn, F_PL);
        if (FHAS(it[j].fn, F_INT) && (is_pos(j + 1, LP_A) || is_pos(j + 1, LP_R)) && !has_pos(j + 1, LP_N)) det = "very";
        j++;
    }
    while (j < end && npre < 4) {
        if (fclass(j) == C_ADV && FHAS(it[j].fn, F_INT) && (has_pos(j + 1, LP_A))) { pre[npre++] = j++; continue; }
        if (it[j].kind == IT_NUM || fclass(j) == C_NUM) { pre[npre++] = j++; continue; }
        if (is_pos(j, LP_A) && (has_pos(j + 1, LP_N) || is_pos(j + 1, LP_A) || (it[j + 1].kind == IT_WORD && !it[j + 1].nrd))) {
            pre[npre++] = j++;
            continue;
        }
        break;
    }
    if (j < end && (is_pos(j, LP_N) || (it[j].kind == IT_WORD && !it[j].nrd) ||
                    (it[j].kind == IT_WORD && (is_pos(j, LP_A) || is_pos(j, LP_V))))) {
        head = j++;
        if (RD(head) && RD(head)->pl) pl = 1;
    } else if (j < end && it[j].kind == IT_OTHER && (det || npre)) {
        head = j++;
    }
    if (ROMANCE(XL) && head >= 0 && is_pos(head, LP_N)) {
        while (j < end && npost < 3 && (is_pos(j, LP_A) || (RD(j) && RD(j)->pos == LP_V && RD(j)->tense == T_PART)))
            post[npost++] = j++;
        /* o repetidor do morro: the hill repeater; a casa do João: João's house */
        if (j + 1 < end && is_of(j)) {
            int m = j + 1;
            if (fclass(m) == C_DET && FHAS(it[m].fn, F_DEF) && m + 1 < end) m++;
            if (bare_noun(m) && !is_pos(m + 1, LP_A) && !is_pos(m + 1, LP_N) && !is_of(m + 1)) { mod = m; j = m + 1; }
            else if (name_word(m) && fclass(j + 1) != C_DET && !nominal(m + 1)) { owner = m; j = m + 1; }
        }
    }
    /* Polish, Russian and Ukrainian have no articles: "the" after a preposition, or on a noun that opens
       a clause with more to say (на холме: on the hill; сигнал хороший: the signal is good) */
    if (SLAVIC(XL) && !det && owner < 0 && mod < 0 && !npost && head >= 0 && is_pos(head, LP_N) && !at_num &&
        !(npre && (it[pre[0]].kind == IT_NUM || fclass(pre[0]) == C_NUM)) &&
        (cs.after_prep || (nch == cs.ch0 && !cs.had_verb && j < end && !punct(j))))
        det = "the";
    if (owner >= 0) {
        char nm[48];
        const lt_t *t = &tk[it[owner].t0];
        snprintf(nm, sizeof nm, "%.*s's", t->len < 40 ? t->len : 40, src + t->at);
        first = emitn(nm, (int)strlen(nm), CF_PASS);
        det = 0;
    }
    if (det) first = det_c = emit(det);
    for (k = 0; k < npre; k++) {
        int c;
        if (it[pre[k]].kind == IT_NUM) c = emitn(src + tk[it[pre[k]].t0].at, tk[it[pre[k]].t0].len, CF_PASS);
        else if (it[pre[k]].kind == IT_FUNC) c = emit(fn_en(it[pre[k]].fn));
        else { noun_en(RD(pre[k]), 0, b, sizeof b); c = emit(b); }
        if (first < 0) first = c;
        if (it[pre[k]].kind == IT_NUM || fclass(pre[k]) == C_NUM) {
            const char *nk = tk[it[pre[k]].t0].key;
            if (!(streq(nk, "1") || streq(nk, "um") || streq(nk, "un") || streq(nk, "une") || streq(nk, "ein") ||
                  streq(nk, "uno") || streq(nk, "een"))) pl = 1;
        }
    }
    for (k = 0; k < npost; k++) {
        int c;
        noun_en(RD(post[k]), 0, b, sizeof b);
        c = emit(b);
        if (first < 0) first = c;
    }
    if (mod >= 0) {
        int c;
        noun_en(RD(mod), 0, b, sizeof b);
        c = emit(b);
        if (first < 0) first = c;
    }
    if (head >= 0 && at_num && npre && RD(head) && first_is(lx_gloss(RD(head)->lex), "hour")) {
        /* à 9 heures: at 9 */
    } else if (head >= 0) {
        int c;
        if (it[head].kind != IT_WORD || !it[head].nrd) c = (emit_src(head, 0), nch - 1);
        else { noun_en(RD(head), pl, b, sizeof b); c = emit(b); }
        if (first < 0) first = c;
    } else if (det && npost + npre && RD(pre[npre ? npre - 1 : 0])) {
        emit("one");
    }
    if (det_c >= 0) fix_article(det_c);
    if (first >= 0 && !cs.had_verb && !cs.after_prep && !cs.np && !cs.subj) {
        cs.np = 1;
        cs.np_a = pl ? 0 : 3;
        cs.np_ch = first;
    }
    cs.after_prep = 0;
    at_num = 0;
    return j > i ? j : i + 1;
}

/* the verb that finishes a chain (vou chegar, ho visto, habe ... gesehen):
   Romance languages right after the finite verb (adverbs, not, pronouns
   between); German and Dutch the last verb of the clause */
static int partner_of(int i, int end, int *via_que)
{
    int k;
    *via_que = 0;
    if (GERMANIC(XL)) {
        for (k = end - 1; k > i; k--) {
            const rd_t *r = RD(k);
            if (it[k].done) continue;
            if (r && r->pos == LP_V && (r->tense == T_INF || r->tense == T_PART)) return k;
            if (r && r->pos == LP_V) break;
        }
        return -1;
    }
    /* "tenho que ir", "voltou a funcionar", "ho da fare": past a clause word to the infinitive */
    for (k = end; k < nit && k <= i + 4 && !punct(k); k++) {}
    end = k;
    for (k = i + 1; k < end && k <= i + 4; k++) {
        const rd_t *r = RD(k);
        const int c = fclass(k);
        const char *key = tk[it[k].t0].key;
        if (it[k].done) continue;
        if ((streq(key, "que") || streq(key, "de") || streq(key, "a") || streq(key, "da") || streq(key, "di") ||
             streq(key, "d'") || streq(key, "te") || streq(key, "zu")) && it[k].nt == 1 && RD(k + 1) &&
            RD(k + 1)->pos == LP_V && RD(k + 1)->tense == T_INF) {
            *via_que = k;
            continue;
        }
        if (r && r->pos == LP_V && !is_fin(r->tense)) return k;
        if (r) {
            /* a second reading as an infinitive or participle (arrivati: arrived, not "successful") */
            int m;
            const int aux = verb_role(RD(i)->lex) & (R_BE | R_HAVE | R_PROG | R_CAN | R_WANT | R_MUST | R_GOING | R_NEED);
            for (m = 0; m < it[k].nrd; m++)
                if (it[k].rd[m].pos == LP_V && !is_fin(it[k].rd[m].tense) && (aux || r->pos == LP_V)) {
                    it[k].sel = (uint8_t)m;
                    return k;
                }
            if (r->pos == LP_V) return -1;
        }
        if (c == C_SKIP && RD(k + 1) && RD(k + 1)->pos == LP_V) { *via_que = k; continue; }   /* en train de */
        if (c == C_ADV || c == C_NEG2 || c == C_CLIT || (r && r->pos == LP_R)) continue;
        break;
    }
    return -1;
}

/* A verb and everything that goes with it. Returns the item after it. */
static int vp(int i, int end)
{
    const rd_t *f = RD(i), *p = 0;
    const char *subj = 0, *obj = 0;
    int a = 3, subj_ch = -1, role, mode = M_SIMPLE, tc, k, via = 0, pi, neg = cs.neg, never = cs.never, q, f_imp = 0;
    char objbuf[40];
    if (!f) return i + 1;
    role = verb_role(f->lex);
    q = it[i].q && !cs.q_done;
    /* the second verb */
    pi = partner_of(i, end, &via);
    if (pi >= 0) {
        int pr;
        p = RD(pi);
        pr = verb_role(p->lex);
        if ((role & R_HAVE) && p->tense == T_PART) mode = M_PERF;
        else if ((role & R_BE) && p->tense == T_PART) mode = (pr & R_PERFBE) ? M_PERF : M_PASS;
        else if ((role & R_PROG) && p->tense == T_GER) mode = M_PROG;
        else if ((role & R_BE) && via && fclass(via) == C_SKIP) mode = M_PROG;          /* je suis en train de */
        else if (p->tense == T_INF) {
            mode = (role & R_GOING) ? M_GOING : (role & R_CAN) ? M_CAN : (role & R_WANT) ? M_WANT :
                   (role & R_MUST) ? M_MUST : (role & R_SHOULD) ? M_SHOULD : (role & R_WILL) ? M_WILL :
                   (role & R_MAY) ? M_MAY : (role & R_NEED) ? M_NEED : (role & R_JUST) && via ? M_JUST :
                   (role & R_HAVE) && via ? M_MUST : M_CHAIN;
            if (mode == M_CHAIN && (role & (R_BE | R_HAVE))) { mode = M_SIMPLE; pi = -1; p = 0; }
        } else if (p->tense == T_GER) mode = M_PROG;
        else { mode = M_SIMPLE; pi = -1; p = 0; }
        if (pi >= 0) {
            it[pi].done = 1;
            if (via) it[via].done = 1;
        } else via = 0;
    }
    /* an object after a hyphen with no subject before: a command (dis-moi, chama-me, envoie-moi) */
    if (!cs.subj && !cs.np && i + 1 < end && (tk[it[i + 1].t0].fl & TF_HYPH) && !q && XL != MT_LANG_IT &&
        (fclass(i + 1) == C_CLIT || fclass(i + 1) == C_PRONX || (fclass(i + 1) == C_DET && FHAS(it[i + 1].fn, F_OBJ))))
        f_imp = 1;
    /* the subject */
    if (f_imp) {
        subj = 0;
    } else if (cs.subj) {
        subj = cs.subj; a = cs.subj_a; cs.subj = 0;
        /* il pleut, il faut: "it" */
        if (streq(subj, "he") && ((role & R_IT) || (p && (verb_role(p->lex) & R_IT))) && XL == MT_LANG_FR) subj = "it";
    } else if (cs.np) {
        subj_ch = cs.np_ch; a = cs.np_a; cs.np = 0;
    } else {
        /* after the verb: es-tu, gehe ich, hoer ich (inverted) */
        for (k = i + 1; k < end && k <= i + 2; k++) {
            const int c = fclass(k);
            if (c == C_PRON && !it[k].done) {
                const int fn = it[k].fn, pl = f->person >= 4 || (f->tense == T_INF && GERMANIC(XL));
                subj = (FHAS(fn, F_AMB) && pl) ? fn_alt(fn) : fn_en(fn);
                a = agree(FPERSON(fn));
                if (FHAS(fn, F_AMB) && pl) a = 0;
                it[k].done = 1;
                break;
            }
            /* hoert mich jemand?: someone after the verb (and its object) is the subject */
            if (c == C_PRONX && !it[k].done && GERMANIC(XL) && streq(fn_en(it[k].fn), "someone")) {
                subj = q ? "anyone" : "someone";
                a = 3;
                it[k].done = 1;
                break;
            }
            if (c == C_PRONX && GERMANIC(XL) && k == i + 1) continue;
            /* hoor je, gaat het, ist das: a determiner standing alone after the verb is its subject */
            if (c == C_DET && !it[k].done && !nominal(k + 1) && GERMANIC(XL)) {
                const int fn = it[k].fn;
                subj = fn_alt(fn);
                a = FHAS(fn, F_YOU) ? 0 : FHAS(fn, F_PL) ? 0 : 3;
                it[k].done = 1;
                break;
            }
            if (c != C_CLIT) break;
        }
        if (!subj) {
            if (f->tense == T_IMP || (f->tense == T_INF && !cs.had_verb && !GERMANIC(XL))) {
                subj = 0;
            } else if (f->tense == T_INF && GERMANIC(XL)) {
                subj = 0;               /* bitte warten: a request */
            } else if (XL == MT_LANG_PT && f->tense == T_PRES && f->person == 3 && !q && cs.cl_first) {
                f_imp = 1;              /* me manda, me avisa: a request */
            } else if (is_fin(f->tense) && (XL == MT_LANG_FR || XL == MT_LANG_DE || XL == MT_LANG_NL) && !q &&
                       nch == cs.ch0 && !(role & (R_BE | R_HAVE)) &&
                       ((f->person != 1 && f->person != 7) ||
                        (fclass(i + 1) == C_PRONX && (streq(fn_en(it[i + 1].fn), "me") || streq(fn_en(it[i + 1].fn), "us"))))) {
                f_imp = 1;              /* attends, komm her, wacht: no subject is a command */
            } else if (is_fin(f->tense)) {
                /* sono arrivati: "sono" is they too, and the participle says which */
                const int pers = (XL == MT_LANG_IT && f->person == 1 && p && p->tense == T_PART && p->pl) ? 6 :
                                 f->person ? f->person : 3;
                subj = pron_for(pers, p ? p : f, q);
                a = (q && XL == MT_LANG_PT && (pers == 3)) ? 0 : agree(pers);
            }
        }
    }
    if (subj && streq(subj, "you")) a = 0;
    /* not after the verb (pas, nicht, mai) */
    for (k = i + 1; k < end; k++) {
        const int c = fclass(k);
        if (c == C_NEG2 && !it[k].done) {
            neg = 1;
            if (FHAS(it[k].fn, F_NEVER)) never = 1, it[k].done = 1;
            it[k].negd = 1;
            break;
        }
        if (!GERMANIC(XL) && k > i + 3) break;
    }
    if (never) neg = 0;
    cs.neg = cs.never = 0;
    /* precisar de, gostar de, avoir besoin de: the "of" goes */
    if ((role & R_DE) && !p && i + 1 < end && is_of(i + 1) && it[i + 1].nt == 1 && fclass(i + 2) != C_DET) it[i + 1].done = 1;
    /* separable verbs: German and Dutch, the particle at the end of the clause (ruf ... an) */
    if (GERMANIC(XL) && end - 1 > i && !p) {
        int last = end - 1;
        while (last > i && (it[last].kind == IT_OTHER)) last--;
        if (last > i && !it[last].done && (it[last].kind == IT_FUNC || it[last].kind == IT_WORD)) {
            char v[LK * 2];
            int li;
            snprintf(v, sizeof v, "%s%s", tk[it[last].t0].key, lx_key(f->lex));
            li = lex_pos(v, LP_V);
            if (li >= 0) {
                static rd_t sep;
                sep = *f;
                sep.lex = (int16_t)li;
                f = &sep;
                it[last].done = 1;
            }
        }
    }
    /* object pronouns: before the verb (te ouço), after a hyphen (chama-me), or enclitic (dimmi) */
    if (cs.ncl) {
        snprintf(objbuf, sizeof objbuf, "%s%s%s", cs.cl[0], cs.ncl > 1 ? " " : "", cs.ncl > 1 ? cs.cl[1] : "");
        obj = objbuf;
        cs.ncl = 0;
    }
    if (i + 1 < end && (tk[it[i + 1].t0].fl & TF_HYPH) && !it[i + 1].done &&
        (fclass(i + 1) == C_CLIT || fclass(i + 1) == C_PRONX || (fclass(i + 1) == C_DET && FHAS(it[i + 1].fn, F_OBJ)))) {
        const int fn = it[i + 1].fn;
        if (!FHAS(fn, F_REFL)) {
            snprintf(objbuf, sizeof objbuf, "%s", fclass(i + 1) == C_DET ? fn_alt(fn) : fn_en(fn));
            obj = objbuf;
        }
        it[i + 1].done = 1;
    }
    tc = tense_class(f->tense);
    if (f_imp) { tc = TC_IMP; subj = 0; }
    if (subj && f->tense == T_INF) tc = TC_PRES;     /* wir gehen, nous ... */
    if (!is_fin(f->tense) && !subj && subj_ch < 0 && f->tense != T_INF) {
        /* a participle or gerund on its own: copiado, arrivando */
        char b[48];
        mtx_en_form(lx_gloss(f->lex), f->tense == T_GER ? MTX_ING : MTX_PP, b, sizeof b);
        emit(b);
        if (obj) emit(obj);
        cs.had_verb = 1;
        return i + 1;
    }
    if (f->tense == T_INF && !subj && subj_ch < 0) tc = TC_IMP;
    if (q && !subj && subj_ch < 0 && tc != TC_IMP) subj = pron_for(f->person ? f->person : 3, f, 1);
    /* an adverb between the two verbs is said before the second (sono appena arrivato: just arrived) */
    vg_adv = 0;
    if (pi > i + 1 && !GERMANIC(XL))
        for (k = i + 1; k < pi; k++)
            if (!it[k].done && (fclass(k) == C_ADV || is_pos(k, LP_R))) {
                vg_adv = fclass(k) == C_ADV ? fn_en(it[k].fn) : lx_gloss(RD(k)->lex);
                it[k].done = 1;
                break;
            }
    if (f->encl && ENCL[f->encl - 1][1][0] && !obj) obj = ENCL[f->encl - 1][1];      /* dimmi: tell me */
    if (p && p->encl && ENCL[p->encl - 1][1][0] && !obj) obj = ENCL[p->encl - 1][1];  /* riesco a sentirti */
    vg_write(subj, subj_ch, a, tc, neg, never, q, mode, lx_gloss(f->lex), p ? lx_gloss(p->lex) : 0, obj);
    if (q) cs.q_done = 1;
    cs.had_verb = 1;
    return i + 1;
}

/* a verb later in the clause, for a subject waiting for it */
static int verb_ahead(int i, int end)
{
    int k;
    for (k = i + 1; k < end; k++) {
        const rd_t *r = RD(k);
        if (it[k].done) continue;
        if (r && r->pos == LP_V && (is_fin(r->tense) || r->tense == T_INF)) return k;
        if (fclass(k) == C_EXIST) return k;
        if (!GERMANIC(XL) && !(fclass(k) == C_CLIT || fclass(k) == C_NEG || fclass(k) == C_ADV ||
                                                       is_pos(k, LP_R) || fclass(k) == C_PRONX || fclass(k) == C_PRON ||
                                                       (fclass(k) == C_DET && FHAS(it[k].fn, F_OBJ)))) return -1;
    }
    return -1;
}

static int clause_end(int i, int e)
{
    int k;
    for (k = i + 1; k < e; k++) if (clause_end_at(k)) return k;
    return e;
}

/* Russian and Ukrainian leave "be" out in the present (я дома, где ты?, сигнал хороший): a clause with
   no verb, something after i, and nothing said of it yet */
static int verbless(int i, int end)
{
    int k;
    /* where are you: after a question word the subject may end the clause */
    if (!CYRL(XL) || cs.had_verb || ((i + 1 >= end || punct(i + 1)) && !cs.qword)) return 0;
    for (k = i + 1; k < end; k++) {
        const rd_t *r = RD(k);
        if ((r && r->pos == LP_V) || fclass(k) == C_EXIST || fclass(k) == C_CL || fclass(k) == C_CONJ) return 0;
    }
    return 1;
}
static const char *be_for(int a, int pl) { return a == 1 ? "am" : (a == 3 && !pl) ? "is" : "are"; }
/* на / na between a verb with a particle (чекати: wait+for) and a pronoun: чекаю на тебе, czekam na ciebie */
static int slavic_na(int i)
{
    const char *g;
    if (!SLAVIC(XL) || fclass(i) != C_PREP || !streq(tk[it[i].t0].key, "na") || fclass(i + 1) != C_CLIT ||
        !RD(i - 1) || RD(i - 1)->pos != LP_V)
        return 0;
    for (g = lx_gloss(RD(i - 1)->lex); *g && *g != ' '; g++)
        if (*g == '+') return 1;
    return 0;
}

static int item_go(int i, int e)
{
    const int end = clause_end(i, e);
    li_t *I = &it[i];
    if (I->done) {
        if (I->kind == IT_FUNC && I->negd && FCLASS(I->fn) == C_NEG2) emit(fn_en(I->fn));
        return i + 1;
    }
    switch (I->kind) {
    case IT_PUNCT: {
        const char c = tk[I->t0].key[0];
        emitn(src + tk[I->t0].at, tk[I->t0].len, CF_GLUE | CF_PASS);
        if (strchr(",;:", c)) clause_reset(0);
        return i + 1;
    }
    case IT_NUM:
        if (nominal(i + 1) && !punct(i + 1)) return np(i, end);
        emit_src(i, 0);
        return i + 1;
    case IT_OTHER:
        emit_src(i, 0);
        return i + 1;
    case IT_WORD: {
        const rd_t *r = RD(i);
        if (!r) {
            /* unknown: a name or a word the lexicon lacks, as written */
            if (verb_ahead(i, end) >= 0 && !cs.had_verb && !cs.np) { cs.np = 1; cs.np_a = 3; cs.np_ch = nch; }
            emit_src(i, 0);
            return i + 1;
        }
        if (r->pos == LP_V && (is_fin(r->tense) || r->tense == T_INF || !(is_pos(i - 1, LP_N)))) {
            /* German and Dutch: a subject waits for a verb at the end */
            return vp(i, end);
        }
        if (r->pos == LP_N || r->pos == LP_A || r->pos == LP_V) {
            const int first = r->pos == LP_N && nch == cs.ch0 && !cs.after_prep, pl = r->pl;
            const int j = np(i, end);
            /* сигнал хороший, батарея на нуле: the signal is good */
            if (first && j < end && verbless(j - 1, end) &&
                (is_pos(j, LP_A) || is_pos(j, LP_R) || fclass(j) == C_ADV || fclass(j) == C_PREP)) {
                emit(be_for(3, pl));
                cs.had_verb = 1;
                return j;
            }
            if (GERMANIC(XL) && cs.np && j < end && !is_pos(j, LP_V)) {
                const int v = verb_ahead(j - 1, end);
                if (v >= 0 && fclass(v) != C_EXIST) vp(v, end), it[v].done = 1;
            }
            return j;
        }
        {
            char b[48];
            snprintf(b, sizeof b, "%s", lx_gloss(r->lex));
            emit(b);
        }
        return i + 1;
    }
    }
    /* function words */
    {
        const int f = I->fn, c = FCLASS(f);
        switch (c) {
        case C_DET:
            /* le, la, les, lo before a verb: its object (je le vois, lo sa) */
            if (FHAS(f, F_OBJ) && !nominal(i + 1)) {
                int k;
                for (k = i + 1; k < end && k <= i + 3; k++) {
                    if (RD(k) && RD(k)->pos == LP_V) {
                        if (cs.ncl < 2) cs.cl[cs.ncl++] = fn_alt(f);
                        return i + 1;
                    }
                    if (!(fclass(k) == C_CLIT || fclass(k) == C_NEG || (fclass(k) == C_DET && FHAS(it[k].fn, F_OBJ)))) break;
                }
            }
            return np(i, end);
        case C_PRON: {
            const int v = verb_ahead(i, end);
            /* je t'entends, il nous voit: a second pronoun before the verb is its object */
            if ((cs.subj || cs.np) && v >= 0 && !cs.had_verb && !GERMANIC(XL)) {
                if (cs.ncl < 2) cs.cl[cs.ncl++] = fn_alt(f);
                return i + 1;
            }
            /* я дома: I am at home; где ты?: where are you */
            if (v < 0 && !cs.after_prep && verbless(i, end)) {
                char b[48];
                const char *be = be_for(agree(FPERSON(f)), FPERSON(f) >= 4);
                if (I->q && !cs.q_done) snprintf(b, sizeof b, "%s %s", be, fn_en(f)), cs.q_done = 1;
                else snprintf(b, sizeof b, "%s %s", fn_en(f), be);
                emit(b);
                cs.had_verb = 1;
                return i + 1;
            }
            if (cs.after_prep || cs.had_verb || v < 0) {
                emit(cs.after_prep || cs.had_verb ? fn_alt(f) : fn_en(f));
                cs.after_prep = 0;
                return i + 1;
            }
            {
                const rd_t *vr = RD(v);
                const int pl = vr && (vr->person >= 4 || (vr->tense == T_INF && GERMANIC(XL)));
                cs.subj = (FHAS(f, F_AMB) && pl) ? fn_alt(f) : fn_en(f);
                cs.subj_a = (FHAS(f, F_AMB) && pl) ? 0 : agree(FPERSON(f));
                if ((tk[I->t0].fl & TF_CAP) && !(tk[I->t0].fl & TF_SSTART) && FHAS(f, F_YOU)) cs.subj = "you", cs.subj_a = 0;
            }
            /* German and Dutch: the verb at the end of the clause comes here */
            if (GERMANIC(XL) && v > i + 1 && fclass(v) != C_EXIST) {
                vp(v, end);
                it[v].done = 1;
            }
            return i + 1;
        }
        case C_CLIT: {
            int v = -1, k;
            for (k = i + 1; k < end && k <= i + 3; k++) {
                if (RD(k) && RD(k)->pos == LP_V) { v = k; break; }
                if (!(fclass(k) == C_CLIT || fclass(k) == C_NEG || fclass(k) == C_ADV)) break;
            }
            if (v >= 0) {
                if (nch == cs.ch0 && (FPERSON(f) == 1 || FPERSON(f) == 4)) cs.cl_first = 1;
                if (!FHAS(f, F_REFL) && cs.ncl < 2) cs.cl[cs.ncl++] = fn_en(f);
            } else if (!FHAS(f, F_REFL)) {
                /* after its verb (жду тебя, czekam na ciebie): wait for you */
                const int pv = slavic_na(i - 1) ? i - 2 : i - 1;
                const char *g = RD(pv) && RD(pv)->pos == LP_V ? lx_gloss(RD(pv)->lex) : "", *p = g;
                char b[40];
                while (*p && *p != ' ' && *p != '+') p++;
                if (*p == '+') {
                    const char *q = p + 1;
                    while (*q && *q != ' ') q++;
                    snprintf(b, sizeof b, "%.*s %s", (int)(q - p - 1), p + 1, fn_en(f));
                    emit(b);
                } else emit(fn_en(f));
            }
            return i + 1;
        }
        case C_NEG: {
            int v = -1, k;
            for (k = i + 1; k < end && k <= i + 4; k++)
                if ((RD(k) && RD(k)->pos == LP_V) || fclass(k) == C_EXIST) { v = k; break; }
            if (v >= 0 && fclass(v) != C_EXIST) {
                if (FHAS(f, F_NEVER)) cs.never = 1; else cs.neg = 1;
            } else if (v >= 0) {
                cs.neg = 1;
            } else emit(fn_alt(f));
            return i + 1;
        }
        case C_NEG2:
            emit(I->negd ? fn_en(f) : fn_alt(f));
            return i + 1;
        case C_PREP: {
            if (slavic_na(i)) return i + 1;                 /* czekam na ciebie: the verb's "for" says it */
            /* à 9 heures, alle 9: "at" before a number */
            const int num = it[i + 1 < nit ? i + 1 : i].kind == IT_NUM || fclass(i + 1) == C_NUM ||
                            (RD(i + 1) && strstr(lx_gloss(RD(i + 1)->lex), "o'clock"));       /* o ósmej: at eight */
            const char *e = num && i + 1 < nit ? fn_alt(f) : fn_en(f);
            char b[40];
            at_num = num;
            if (FHAS(f, F_THE) && fclass(i + 1) == C_DET && FHAS(it[i + 1].fn, F_POSS)) {
                const size_t n = strlen(e);
                if (n > 4 && streq(e + n - 4, " the")) { snprintf(b, sizeof b, "%.*s", (int)n - 4, e); e = b; }
            }
            emit(e);
            cs.after_prep = 1;
            return i + 1;
        }
        case C_CONJ:
            if (FHAS(f, F_REFL) && (cs.subj || cs.np) && RD(i + 1) && RD(i + 1)->pos == LP_V) return i + 1;  /* ele se chama */
            if (FHAS(f, F_QUE) && I->q && nch == cs.ch0 && !cs.q_done) {   /* che fai?, que fazes?: what */
                emit("what");
                cs.qword = 1;
                return i + 1;
            }
            emit(fn_en(f));
            clause_reset(FHAS(f, F_SUB) ? 1 : 0);
            return i + 1;
        case C_Q:
            if (I->q || !FHAS(f, F_SUB)) {
                const int c = emit(fn_en(f));
                /* vc ta onde?, tu vas où ?: the question word goes first */
                if (I->q && cs.had_verb && punct(i + 1) && c > cs.ch0) move_chunk(c, cs.ch0);
                cs.qword = 1;
            } else {
                emit(fn_alt(f));
                clause_reset(1);
            }
            return i + 1;
        case C_EXIST: {
            int pl = FHAS(f, F_PL), k, past = FHAS(f, F_PAST), neg = FHAS(f, F_NEG) || cs.neg;
            char b[40];
            for (k = i + 1; k < end; k++) if (RD(k) && RD(k)->pos == LP_N) { pl = RD(k)->pl; break; }
            if (FHAS(f, F_NEG)) {
                emit(fn_en(f));
            } else if (I->q && !cs.q_done) {
                snprintf(b, sizeof b, "%s there", past ? (pl ? "were" : "was") : (pl ? "are" : "is"));
                emit(b);
                cs.q_done = 1;
            } else {
                snprintf(b, sizeof b, "there %s%s", past ? (pl ? "were" : "was") : (pl ? "are" : "is"), neg ? "n't" : "");
                emit(b);
            }
            cs.neg = 0;
            cs.had_verb = 1;
            return i + 1;
        }
        case C_SKIP:
            return i + 1;
        case C_CL:
            /* у тебя есть: do you have, or in a statement you have */
            emit(SLAVIC(XL) && !I->q ? fn_alt(f) : fn_en(f));
            cs.had_verb = 1;
            return i + 1;
        case C_PRONX: {
            const int v = verb_ahead(i, end);
            /* это мы, це я: this is us */
            if (v < 0 && !cs.after_prep && nch == cs.ch0 && verbless(i, end)) {
                char b[40];
                if (I->q && !cs.q_done) snprintf(b, sizeof b, "is %s", fn_en(f)), cs.q_done = 1;
                else snprintf(b, sizeof b, "%s is", fn_en(f));
                emit(b);
                cs.had_verb = 1;
                return i + 1;
            }
            const int ch0 = emit(cs.neg || I->q ? fn_alt(f) : fn_en(f));     /* anyone, in a question */
            if (v >= 0 && !cs.had_verb && !cs.np && !cs.subj && !cs.after_prep) { cs.np = 1; cs.np_a = 3; cs.np_ch = ch0; }
            cs.after_prep = 0;
            return i + 1;
        }
        case C_ADV:
            emit(fn_en(f));
            return i + 1;
        default:
            emit(fn_en(f));
            return i + 1;
        }
    }
}

/* ------------------------------------------------------------- assemble */

static int put(char *out, size_t cap, size_t *n, const char *s, int len)
{
    int k;
    for (k = 0; k < len; k++) {
        if (*n + 1 >= cap) return 0;
        out[(*n)++] = s[k];
    }
    return 1;
}

static void assemble(char *out, size_t cap, size_t *n, int first_cap)
{
    int c, sent = 1;
    const size_t start = *n;
    for (c = 0; c < nch; c++) {
        char tmp[256];
        int len = ch[c].len, k;
        if (!len) continue;
        if (*n > start && !(ch[c].fl & CF_GLUE) && out[*n - 1] != '\n' && out[*n - 1] != '\'') {
            if (!put(out, cap, n, " ", 1)) return;
        }
        memcpy(tmp, arena + ch[c].off, (size_t)len);
        if (all_upper && !(ch[c].fl & CF_PASS)) {
            for (k = 0; k < len; k++) tmp[k] = (char)upc(tmp[k]);
        } else if (sent && first_cap && !(ch[c].fl & CF_PASS) && tmp[0] >= 'a' && tmp[0] <= 'z') {
            tmp[0] = (char)upc(tmp[0]);
        }
        if (!(ch[c].fl & CF_GLUE)) sent = 0;
        if (len >= 1 && (tmp[len - 1] == '.' || tmp[len - 1] == '!' || tmp[len - 1] == '?' || tmp[len - 1] == '\n') &&
            (ch[c].fl & CF_GLUE)) sent = 1;
        if (!put(out, cap, n, tmp, len)) return;
    }
}

static void detect_upper(const char *s)
{
    int lo = 0, up = 0, words = 0, in = 0;
    for (; *s; s++) {
        const int l = (*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z');
        if (*s >= 'a' && *s <= 'z') lo++;
        else if (*s >= 'A' && *s <= 'Z') up++;
        if (l && !in) words++;
        in = l;
    }
    all_upper = up >= 6 && lo == 0 && words >= 2;
}

/* Russian and Ukrainian typed in chat Latin spell some letters their own way (horosho, ja, privjet,
   xorosho): a word neither the tables nor the rules know is tried as its key would spell it */
static int cyr_known(const char *k)
{
    rd_t r[3];
    return func_find(k, 0, 0) >= 0 || xfind(X->abbr, X->nabbr, k) >= 0 || analyze(k, r);
}
static void cyr_chat_keys(void)
{
    int t, k, o, pass;
    for (t = 0; t < ntk; t++) {
        char v[LK];
        if (tk[t].kind != TK_WORD || (unsigned char)src[tk[t].at] >= 0x80 || cyr_known(tk[t].key)) continue;
        for (pass = 0; pass < 2; pass++) {
            const char *s = tk[t].key;
            for (k = o = 0; s[k] && o < LK - 3; k++) {
                const char c = s[k], p = k ? s[k - 1] : ' ';
                /* h for х (in Ukrainian h is г, so only on the second try) */
                if ((c == 'h' && !strchr("kzcs", p) && (pass || XL == MT_LANG_RU)) || c == 'x') v[o++] = 'k', v[o++] = 'h';
                else v[o++] = c == 'j' ? 'y' : c == 'w' ? 'v' : c;
            }
            v[o] = 0;
            if (!streq(v, tk[t].key) && cyr_known(v)) { snprintf(tk[t].key, sizeof tk[t].key, "%s", v); break; }
        }
    }
}

static void translate_batch(char *out, size_t cap, size_t *n)
{
    int i = 0, caps = 0, t;
    for (t = 0; t < ntk; t++) if (tk[t].kind == TK_WORD) { caps = (tk[t].fl & TF_CAP) != 0; break; }
    nch = 0;
    narena = 0;
    if (CYRL(XL)) cyr_chat_keys();
    build_items();
    choose();
#ifdef MTX_DEBUG
    for (i = 0; i < nit; i++) {
        const rd_t *r = RD(i);
        printf("  [%d] k%d '%s' n%d", i, it[i].kind, tk[it[i].t0].key, it[i].nt);
        if (it[i].kind == IT_FUNC) printf(" fn c%d '%s'", FCLASS(it[i].fn), fn_en(it[i].fn));
        if (r) printf(" %s p%d t%d pr%d pl%d (of %d)", lx_key(r->lex), r->pos, r->tense, r->person, r->pl, it[i].nrd);
        printf("\n");
    }
    i = 0;
#endif
    while (i < nit) {
        int e = i;
        while (e < nit && !(it[e].kind == IT_PUNCT && strchr(".!?\n", tk[it[e].t0].key[0]))) e++;
        if (e < nit) e++;
        sent_q = it[e - 1].q;
        clause_reset(0);
        while (i < e) {
            int nx = item_go(i, e);
            if (nx <= i) nx = i + 1;
            i = nx;
        }
    }
    assemble(out, cap, n, caps);
}

int mt_xlate_to_en(mt_lang_t from, const char *in, char *out, size_t cap)
{
    size_t n = 0;
    int pos = 0;
    if (from == MT_LANG_ES) return mt_xlate_es_en(in, out, cap);
    if (!out || !cap) return 0;
    out[0] = 0;
    if (!in || !*in || !mt_xlate_lang_ready(from)) return 0;
    X = &D[from];
    XL = from;
    detect_upper(in);
    while (in[pos]) {
        int used;
        src = in + pos;
        used = tokenize(0, fold_mode(from));
        if (ntk) {
            const int gap = pos > 0 && (in[pos - 1] == ' ' || in[pos] == ' ' || in[pos - 1] == '\n');
            if (n && gap && n + 1 < cap && out[n - 1] != ' ' && out[n - 1] != '\n') out[n++] = ' ';
            translate_batch(out, cap, &n);
        }
        if (used <= 0) break;
        pos += used;
        if (n + 1 >= cap) break;
    }
    out[n < cap ? n : cap - 1] = 0;
    return (int)strlen(out);
}

/* ------------------------------------------------------------- detection */

static const mt_lang_t ORDERL[XLD_NLANG] = { MT_LANG_EN, MT_LANG_ES, MT_LANG_PT, MT_LANG_FR, MT_LANG_DE, MT_LANG_IT,
                                             MT_LANG_NL, MT_LANG_PL, MT_LANG_RU, MT_LANG_UK };
enum { D_EN, D_ES, D_PT, D_FR, D_DE, D_IT, D_NL, D_PL, D_RU, D_UK };

static int dfind(const char *k)
{
    int lo = 0, hi = (int)(sizeof XLD_WORD / sizeof XLD_WORD[0]);
    const int n = hi;
    while (lo < hi) {
        const int mid = (lo + hi) >> 1;
        if (strcmp(XLD_POOL + XLD_WORD[mid], k) < 0) lo = mid + 1; else hi = mid;
    }
    return (lo < n && streq(XLD_POOL + XLD_WORD[lo], k)) ? lo : -1;
}
static void dadd(int *score, const char *k, int can)
{
    const int i = dfind(k);
    int l;
    if (i < 0) return;
    for (l = 0; l < XLD_NLANG; l++)
        if (can & (1 << l)) score[l] += (int)((XLD_W[i] >> (4 * l)) & 15);
}
/* A word written in Cyrillic, by its Russian (uk 0) or Ukrainian (uk 1) transliteration: its weight there. */
static int cweight(const char *k, int uk)
{
    int lo = 0, hi = (int)(sizeof XLC_WORD / sizeof XLC_WORD[0]);
    const int n = hi;
    while (lo < hi) {
        const int mid = (lo + hi) >> 1;
        if (strcmp(XLC_POOL + XLC_WORD[mid], k) < 0) lo = mid + 1; else hi = mid;
    }
    if (lo >= n || !streq(XLC_POOL + XLC_WORD[lo], k)) return -1;
    return uk ? XLC_W[lo] >> 4 : XLC_W[lo] & 15;
}

/* One word of a line: its keys as Latin (German umlauts), Russian and
   Ukrainian fold it, and whether it was written in Cyrillic. Returns the
   bytes read (0 at the end); *letters 0 when it was no word. */
typedef struct { char k[LK], kd[LK], kr[LK], ku[LK]; int n, cyr, acc, letters, can; } dword_t;
#define DB(l) (1 << (l))
#define D_ALL ((1 << XLD_NLANG) - 1)
/* The languages that write a letter with a mark (U+00C0..U+017F, lower case): é is not Polish, ł not
   Italian. A word with a letter its language never writes is not a word of it; one typed without
   the marks can be anyone's. */
static int letter_langs(unsigned cp)
{
    static const struct { const char *s; int m; } A[] = {
        { "\xC3\xA1", DB(D_ES) | DB(D_PT) | DB(D_IT) | DB(D_NL) },                                  /* á */
        { "\xC3\xA0", DB(D_PT) | DB(D_FR) | DB(D_IT) },                                             /* à */
        { "\xC3\xA2", DB(D_PT) | DB(D_FR) },                                                        /* â */
        { "\xC3\xA3", DB(D_PT) },                                                                   /* ã */
        { "\xC3\xA4", DB(D_DE) },                                                                   /* ä */
        { "\xC3\xA7", DB(D_PT) | DB(D_FR) },                                                        /* ç */
        { "\xC3\xA8", DB(D_FR) | DB(D_IT) | DB(D_NL) },                                             /* è */
        { "\xC3\xA9", DB(D_EN) | DB(D_ES) | DB(D_PT) | DB(D_FR) | DB(D_IT) | DB(D_NL) },            /* é */
        { "\xC3\xAA", DB(D_PT) | DB(D_FR) },                                                        /* ê */
        { "\xC3\xAB", DB(D_FR) | DB(D_NL) },                                                        /* ë */
        { "\xC3\xAC", DB(D_IT) },                                                                   /* ì */
        { "\xC3\xAD", DB(D_ES) | DB(D_PT) | DB(D_IT) },                                             /* í */
        { "\xC3\xAE", DB(D_FR) | DB(D_IT) },                                                        /* î */
        { "\xC3\xAF", DB(D_EN) | DB(D_FR) | DB(D_NL) },                                             /* ï */
        { "\xC3\xB1", DB(D_ES) },                                                                   /* ñ */
        { "\xC3\xB2", DB(D_IT) },                                                                   /* ò */
        { "\xC3\xB3", DB(D_ES) | DB(D_PT) | DB(D_IT) | DB(D_PL) | DB(D_NL) },                       /* ó */
        { "\xC3\xB4", DB(D_PT) | DB(D_FR) },                                                        /* ô */
        { "\xC3\xB5", DB(D_PT) },                                                                   /* õ */
        { "\xC3\xB6", DB(D_DE) | DB(D_NL) },                                                        /* ö */
        { "\xC3\xB9", DB(D_FR) | DB(D_IT) },                                                        /* ù */
        { "\xC3\xBA", DB(D_ES) | DB(D_PT) | DB(D_IT) },                                             /* ú */
        { "\xC3\xBB", DB(D_FR) },                                                                   /* û */
        { "\xC3\xBC", DB(D_ES) | DB(D_PT) | DB(D_FR) | DB(D_DE) | DB(D_NL) },                       /* ü */
        { "\xC3\x9F", DB(D_DE) },                                                                   /* ß */
        { "\xC5\x93", DB(D_FR) },                                                                   /* œ */
        { "\xC4\x85", DB(D_PL) }, { "\xC4\x87", DB(D_PL) }, { "\xC4\x99", DB(D_PL) },              /* ą ć ę */
        { "\xC5\x82", DB(D_PL) }, { "\xC5\x84", DB(D_PL) }, { "\xC5\x9B", DB(D_PL) },              /* ł ń ś */
        { "\xC5\xBA", DB(D_PL) }, { "\xC5\xBC", DB(D_PL) },                                         /* ź ż */
    };
    size_t i;
    for (i = 0; i < sizeof A / sizeof A[0]; i++) {
        const unsigned char *p = (const unsigned char *)A[i].s;
        if ((((unsigned)(p[0] & 0x1F) << 6) | (p[1] & 0x3F)) == cp) return A[i].m;
    }
    return D_ALL;
}
static int dword(const unsigned char *s0, dword_t *w)
{
    const unsigned char *s = s0;
    char o[4];
    int no, a, up, u, qvx = 0;
    memset(w, 0, sizeof *w);
    w->can = D_ALL;
    if (!lfold(s, o, &no, &a, &up, FM_RU)) return *s ? 1 : 0;
    while (*s && (u = lfold(s, o, &no, &a, &up, FM_RU)) != 0) {
        int up2, nk;
        const int cyr = cyr_at(s, &up2) != 0;
        w->cyr |= cyr;
        w->acc |= a;
        if (s[0] >= 0xC3 && s[0] <= 0xC5 && (s[1] & 0xC0) == 0x80) {
            unsigned cp = ((unsigned)(s[0] & 0x1F) << 6) | (s[1] & 0x3F);
            if (cp >= 0xC0 && cp <= 0xDE && cp != 0xD7) cp += 0x20;
            else if (cp >= 0x100 && cp < 0x17F && lata_upper(cp)) cp++;
            w->can &= letter_langs(cp);
        }
        if ((*s | 0x20) == 'q' || (*s | 0x20) == 'v' || (*s | 0x20) == 'x') qvx = 1;
        if (w->n + no < LK) { memcpy(w->kr + w->n, o, (size_t)no); w->n += no; }
        nk = (int)strlen(w->ku);
        if (lfold(s, o, &no, &a, &up, FM_UK) && nk + no < LK) memcpy(w->ku + nk, o, (size_t)no), w->ku[nk + no] = 0;
        if (!cyr) {
            nk = (int)strlen(w->k);
            if (lfold(s, o, &no, &a, &up, FM_PLAIN) && nk + no < LK) memcpy(w->k + nk, o, (size_t)no), w->k[nk + no] = 0;
            nk = (int)strlen(w->kd);
            if (lfold(s, o, &no, &a, &up, FM_DE) && nk + no < LK) memcpy(w->kd + nk, o, (size_t)no), w->kd[nk + no] = 0;
        }
        w->letters++;
        s += u;
        /* an elision ends the word with its apostrophe: c', l', dell' */
        if (!w->cyr && (*s == '\'' || (s[0] == 0xE2 && s[1] == 0x80 && s[2] == 0x99))) {
            const int sk = *s == '\'' ? 1 : 3;
            const int nkd = (int)strlen(w->kd);
            if ((nkd < 7 && lfold(s + sk, o, &no, &a, &up, FM_DE)) || (nkd < 3 && s[sk] <= ' ')) {
                if (nkd < LK - 1) { w->kd[nkd] = '\''; w->kd[nkd + 1] = 0; }
                s += sk;
                break;
            }
        }
    }
    w->kr[w->n] = 0;
    if (qvx && w->letters >= 3) w->can &= ~DB(D_PL);          /* q v x: not Polish (xd is chat) */
    return (int)(s - s0);
}

/* Cyrillic letters of a line less its Latin ones. */
static int cyr_letters(const char *in)
{
    const unsigned char *s = (const unsigned char *)in;
    int n = 0, up;
    while (*s) {
        if (cyr_at(s, &up)) { n++; s += 2; continue; }
        if ((*s | 0x20) >= 'a' && (*s | 0x20) <= 'z') n--;
        s++;
    }
    return n;
}

/* Word-profile scores of the ten languages (en es pt fr de it nl pl ru uk); returns the words found. */
int mtx_profile(const char *in, int *score)
{
    const unsigned char *s = (const unsigned char *)in;
    int found = 0, i;
    for (i = 0; i < XLD_NLANG; i++) score[i] = 0;
    while (*s) {
        dword_t w;
        const unsigned char *p;
        int used, up;
        if (s[0] == 0xC2 && (s[1] == 0xBF || s[1] == 0xA1)) { score[D_ES] += 12; s += 2; continue; }   /* inverted marks */
        used = dword(s, &w);
        if (!used) break;
        /* letters that say which language */
        for (p = s; p < s + used; p++) {
            const unsigned cp = cyr_at(p, &up);
            if (cp == 0x44B || cp == 0x44D || cp == 0x44A || cp == 0x451) score[D_RU] += 12;    /* ы э ъ ё */
            else if (cp == 0x456 || cp == 0x457 || cp == 0x454 || cp == 0x491) score[D_UK] += 12;  /* і ї є ґ */
            if (p[0] == 0xC3 && (p[1] & 0xC0) == 0x80) {
                const unsigned d = p[1] | 0x20;
                if (d == 0xA3 || d == 0xB5) score[D_PT] += 9;                           /* ã õ */
                else if (d == 0xB1) score[D_ES] += 9;                                   /* ñ */
                else if (d == 0xA7) { score[D_PT] += 3; score[D_FR] += 3; }             /* ç */
                else if (p[1] == 0x9F || d == 0xA4 || d == 0xB6 || d == 0xBC) score[D_DE] += 8;   /* ß ä ö ü */
                else if (d == 0xA8 || d == 0xAC || d == 0xB2 || d == 0xB9) { score[D_IT] += 4; score[D_FR] += 3; }
                else if (d == 0xAA || d == 0xA2 || d == 0xAE || d == 0xBB || d == 0xB4 || d == 0xAB || d == 0xAF) {
                    score[D_FR] += 3; score[D_PT] += (d == 0xAA || d == 0xB4 || d == 0xA2) ? 3 : 0;
                }
            } else if (p[0] == 0xC5 && (p[1] == 0x92 || p[1] == 0x93)) score[D_FR] += 6;      /* œ */
            else if ((p[0] == 0xC4 && (p[1] == 0x84 || p[1] == 0x85 || p[1] == 0x86 || p[1] == 0x87 || p[1] == 0x98 ||
                                       p[1] == 0x99)) ||
                     (p[0] == 0xC5 && (p[1] == 0x81 || p[1] == 0x82 || p[1] == 0x83 || p[1] == 0x84 || p[1] == 0x9A ||
                                       p[1] == 0x9B || (p[1] >= 0xB9 && p[1] <= 0xBC))))
                score[D_PL] += 7;                                                       /* ą ć ę ł ń ś ź ż */
        }
        s += used;
        if (!w.letters || w.n >= LK - 1) continue;
        if (w.cyr) {
            /* Cyrillic: Russian and Ukrainian only, each by its own transliteration */
            const int r = cweight(w.kr, 0), u = cweight(w.ku, 1);
            score[D_RU] += 3 + (r > 0 ? r : 0);
            score[D_UK] += 3 + (u > 0 ? u : 0);
            found += r >= 0 || u >= 0;
            continue;
        }
        {
            const char *k = w.kd;
            const int n = (int)strlen(k);
            if (n < 1) continue;
            if (streq(k, "ok") || streq(k, "okay") || streq(k, "okey")) continue;
            /* Dutch spelling: ij, oo, aa, uu; Polish rz, cz */
            if (strstr(k, "ij")) score[D_NL] += 4;
            else if (strstr(k, "oo") || strstr(k, "aa") || strstr(k, "uu")) { score[D_NL] += 3; score[D_DE] += 1; }
            if (strstr(k, "rz") || strstr(k, "cz") || strstr(k, "szcz")) score[D_PL] += 2;
            /* how people laugh: jajaja (es), kkkk (pt) */
            if (n >= 4 && k[0] == 'j' && (k[1] == 'a' || k[1] == 'e') && k[2] == 'j') { score[D_ES] += 12; found++; continue; }
            {
                int ks = 0;
                while (k[ks] == 'k') ks++;
                if (n >= 3 && ks == n) { score[D_PT] += 12; found++; continue; }
            }
            if (dfind(k) >= 0) found++;
            dadd(score, k, w.can);
            /* a letter a language never writes counts against it (praça is not Polish, vediamo neither) */
            for (i = 0; i < D_RU; i++)
                if (!(w.can & DB(i))) score[i] -= 3;
        }
    }
    return found;
}

/* Words of a line that a language's own hand tables or irregular forms know:
   Spanish always, the others when their data is attached. Cyrillic words
   count for Russian and Ukrainian only; Latin words count for them only when
   long enough not to be another language's short word (net, da, kak). */
void mtx_votes(const char *in, int *v)
{
    const unsigned char *s = (const unsigned char *)in;
    int l;
    for (l = 0; l < XLD_NLANG; l++) v[l] = 0;
    while (*s) {
        dword_t w;
        int hit[XLD_NLANG], strong = 0;
        const int used = dword(s, &w);
        if (!used) break;
        s += used;
        if (!w.letters || w.n < 2 || w.n >= LK - 1) continue;
        /* 2: a function word, chat spelling or irregular form; 1: a headword or an inflection the rules know */
        for (l = 1; l < XLD_NLANG; l++) {
            const mt_lang_t lang = ORDERL[l];
            const char *key = lang == MT_LANG_DE ? w.kd : lang == MT_LANG_RU ? w.kr : lang == MT_LANG_UK ? w.ku : w.k;
            const int n = (int)strlen(key);
            hit[l] = 0;
            if (!n || (w.cyr && !CYRL(lang)) || !(w.can & (1 << l))) continue;
            if (lang == MT_LANG_ES) {
                hit[l] = mtx_es_word(key, w.acc) ? 2 : mtx_es_lemma(key) ? 1 : 0;
            } else if (mt_xlate_lang_ready(lang)) {
                int mis = 0, f;
                rd_t r[3];
                X = &D[lang];
                XL = lang;
                f = func_find(key, w.acc, &mis);
                if ((f >= 0 && !mis && !FHAS(f, F_HYPH)) || xfind(X->exc, X->nexc, key) >= 0 ||
                    xfind(X->abbr, X->nabbr, key) >= 0) hit[l] = 2;
                else if (xfind(X->lex, X->nlex, key) >= 0 || (n >= 4 && analyze_core(key, r))) hit[l] = 1;
                if (hit[l] && f >= 0 && !mis && FCLASS(f) == C_NUM) hit[l] = 1;   /* numbers are short and shared (sto, tre) */
                if (CYRL(lang) && !w.cyr && (hit[l] == 2 ? n < 4 : n < 6)) hit[l] = 0;   /* typed Latin: long words only */
            }
            strong += hit[l] == 2;
        }
        /* a word only one language's tables know says the most (kun, ontem, domani) */
        for (l = 1; l < XLD_NLANG; l++)
            v[l] += hit[l] == 2 ? (strong == 1 ? 9 : 4) : hit[l] == 1 ? 2 : 0;
    }
}

mt_lang_t mt_xlate_detect(const char *in, int *conf)
{
    int sc[XLD_NLANG], v[XLD_NLANG], found, best = 0, second = -1, l, oconf = 0, margin;
    mt_lang_t old;
    if (conf) *conf = 0;
    if (!in || !*in) return MT_LANG_UNKNOWN;
    old = mtx_detect_es_en(in, &oconf);
    found = mtx_profile(in, sc);
    mtx_votes(in, v);
    for (l = 1; l < XLD_NLANG; l++) sc[l] += v[l];
    /* written in Cyrillic: Russian or Ukrainian, whichever its letters and words say; on a tie Russian,
       just sure enough to be read (mt_xlate_incoming_in then takes the one that is on) */
    if (cyr_letters(in) > 0) {
        const int d = sc[D_RU] - sc[D_UK], ad = d < 0 ? -d : d;
        if (conf) *conf = 55 + 45 * ad / (ad + 12);
        return d >= 0 ? MT_LANG_RU : MT_LANG_UK;
    }
    for (l = 1; l < XLD_NLANG; l++) if (sc[l] > sc[best]) best = l;
    for (l = 0; l < XLD_NLANG; l++) if (l != best && (second < 0 || sc[l] > sc[second])) second = l;
    margin = sc[best] - sc[second];
    if (!found && margin < 8) { if (conf) *conf = oconf; return old; }
    /* a line the Spanish/English reader is sure of takes more than one word of another language to move */
    if (ORDERL[best] == MT_LANG_EN || ORDERL[best] == MT_LANG_ES || margin < 6 || sc[best] < 9 ||
        ((old == MT_LANG_ES || old == MT_LANG_EN) && oconf >= 90 && margin < 12)) {
        if (conf) *conf = oconf;
        return old;
    }
    /* another language: its words beat every other's */
    {
        int c = 55 + margin * 45 / (sc[best] + 1);
        if (c > 100) c = 100;
        if (conf) *conf = c;
        return ORDERL[best];
    }
}

mt_lang_t mt_xlate_detect_in(const char *in, unsigned langs, int *conf)
{
    int c = 0;
    mt_lang_t l = mt_xlate_detect(in, &c);
    /* Russian or Ukrainian by a few words only (no ы э ъ ё or і ї є ґ): the one that is read and here */
    if (CYRL(l) && c < 77 && (!(langs & MT_LANG_BIT(l)) || !mt_xlate_lang_ready(l))) {
        const mt_lang_t o = l == MT_LANG_RU ? MT_LANG_UK : MT_LANG_RU;
        if ((langs & MT_LANG_BIT(o)) && mt_xlate_lang_ready(o)) l = o;
    }
    if (conf) *conf = c;
    return l;
}

int mt_xlate_incoming_in(const char *in, unsigned langs, char *out, size_t cap, mt_lang_t *lang)
{
    int conf = 0;
    mt_lang_t l;
    if (lang) *lang = MT_LANG_UNKNOWN;
    if (!out || !cap) return 0;
    out[0] = 0;
    l = mt_xlate_detect_in(in, langs, &conf);
    if (lang) *lang = conf >= MT_XLATE_MIN_CONF ? l : MT_LANG_UNKNOWN;
    if (l <= MT_LANG_EN || conf < MT_XLATE_MIN_CONF || !(langs & MT_LANG_BIT(l)) || !mt_xlate_lang_ready(l)) return 0;
    return mt_xlate_to_en(l, in, out, cap);
}

/* ------------------------------------------------------------- phrasebook */

const char *mt_phrase_in(int i, mt_lang_t lang)
{
    const mt_phrase_t p = mt_phrase(i);
    if (!p.en[0]) return "";
    if (lang == MT_LANG_EN) return p.en;
    if (lang == MT_LANG_ES) return p.es;
    if (lang >= MT_LANG_FIRST_CARD && lang < MT_LANG_COUNT && i < XLB_NBOOK &&
        (int)(lang - MT_LANG_FIRST_CARD) < XLB_NLANG)
        return XLB_POOL + XLB_BOOK[(lang - MT_LANG_FIRST_CARD) * XLB_NBOOK + i];
    return "";
}
