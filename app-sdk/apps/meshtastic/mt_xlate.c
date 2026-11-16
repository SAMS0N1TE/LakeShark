/* Spanish -> English gist translation and language detection for chat lines.
   Written for this app.

   A rule-based reader, in passes over one batch of tokens:
   1. tokens (words, numbers, punctuation, anything else passes through);
      chat spellings expanded (q, xq, tb, porfa), laughter, long vowels;
   2. the longest phrase of the hand table first (por favor, nos vemos,
      cerca de), then every other word read by the lexicon with Spanish
      inflection undone: plural, gender, diminutive, -isimo, re-, verb
      endings with stem and spelling changes, clitics on the end;
   3. English written back with English grammar: subject pronouns from the
      verb ending, do-support, tense, progressive, going to, have to, can,
      adjective before noun, object pronouns after the verb, there is/are,
      questions turned round.
   The data is mt_xlate_es_en.h (tools/gen_xlate.py). No malloc: everything
   lives in the static buffers below, so one caller at a time. */
#include "mt_xlate.h"
#include "mt_xlate_es_en.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define XL_KEY      28      /* longest folded word kept; longer words pass through */
#define XL_MAXTOK   96      /* tokens per batch; longer input runs in batches */
#define XL_CHUNKS   224     /* English pieces per batch */
#define XL_ARENA    4096    /* English text per batch */
#define XL_BATCH    1200    /* source bytes per batch, so the English always fits */

#define NLEX   ((int)(sizeof XL_LEX / sizeof XL_LEX[0]))
#define NPHR   ((int)(sizeof XL_PHR / sizeof XL_PHR[0]))
#define NABBR  ((int)(sizeof XL_ABBR / sizeof XL_ABBR[0]))
#define NIRR   ((int)(sizeof XL_IRR / sizeof XL_IRR[0]))
#define NENV   ((int)(sizeof XL_ENV / sizeof XL_ENV[0]))
#define NENPL  ((int)(sizeof XL_ENPL / sizeof XL_ENPL[0]))
#define NTENER ((int)(sizeof XL_TENER / sizeof XL_TENER[0]))
#define NHACER ((int)(sizeof XL_HACER / sizeof XL_HACER[0]))
#define NENW   ((int)(sizeof XL_ENW / sizeof XL_ENW[0]))
#define NBOOK  ((int)(sizeof XL_BOOK / sizeof XL_BOOK[0]))
#define NCAT   ((int)(sizeof XL_BOOKCAT / sizeof XL_BOOKCAT[0]))
#define NEND   ((int)(sizeof XL_ENDINGS / sizeof XL_ENDINGS[0]))
#define NCHG   ((int)(sizeof XL_STEM_CHANGES / sizeof XL_STEM_CHANGES[0]))
#define NSPELL ((int)(sizeof XL_SPELLING / sizeof XL_SPELLING[0]))

/* ------------------------------------------------------------------ text */

static int lowc(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
static int upc(int c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }
static int isdig(int c) { return c >= '0' && c <= '9'; }
static int isvow(int c) { return c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u'; }
static int streq(const char *a, const char *b) { return strcmp(a, b) == 0; }
static int ends(const char *s, int n, const char *e)
{
    int m = (int)strlen(e);
    return n >= m && memcmp(s + n - m, e, (size_t)m) == 0;
}

/* One letter of UTF-8 Latin text, folded: a-z, or 0xF1 for n-tilde. *n bytes,
   *acc for an acute accent, *up for a capital. 0 = not a letter. */
static int letter(const unsigned char *s, int *n, int *acc, int *up)
{
    unsigned c = s[0], d;
    *n = 1; *acc = 0; *up = 0;
    if (c < 0x80) {
        if (c >= 'A' && c <= 'Z') { *up = 1; return (int)c + 32; }
        return (c >= 'a' && c <= 'z') ? (int)c : 0;
    }
    if (c != 0xC3 || s[1] < 0x80 || s[1] > 0xBF) return 0;
    d = s[1];
    if (d < 0xA0) { *up = 1; d += 0x20; }
    *n = 2;
    switch (d) {
    case 0xA1: *acc = 1; return 'a';
    case 0xA0: case 0xA2: case 0xA3: case 0xA4: return 'a';
    case 0xA9: *acc = 1; return 'e';
    case 0xA8: case 0xAA: case 0xAB: return 'e';
    case 0xAD: *acc = 1; return 'i';
    case 0xAC: case 0xAE: case 0xAF: return 'i';
    case 0xB3: *acc = 1; return 'o';
    case 0xB2: case 0xB4: case 0xB5: case 0xB6: return 'o';
    case 0xBA: *acc = 1; return 'u';
    case 0xB9: case 0xBB: case 0xBC: return 'u';
    case 0xB1: return 0xF1;
    case 0xA7: return 'c';
    }
    *n = 1;
    return 0;
}

/* ---------------------------------------------------------------- tables */

static const char *P(uint32_t w) { return XL_POOL + (w & 0x3FFFFFu); }
static const char *NEXT(const char *s) { return s + strlen(s) + 1; }

static int find(const uint32_t *t, int n, const char *key)
{
    int lo = 0, hi = n;
    while (lo < hi) {
        int mid = (lo + hi) >> 1;
        if (strcmp(P(t[mid]), key) < 0) lo = mid + 1; else hi = mid;
    }
    return (lo < n && strcmp(P(t[lo]), key) == 0) ? lo : -1;
}

#define LX_POS(i)   ((int)((XL_LEX[i] >> 22) & 15))
#define LX_FLAGS(i) ((int)((XL_LEX[i] >> 28) & 15))
#define LX_RANK(i)  ((LX_FLAGS(i) >> XLEX_RANK_SHIFT) & 3)
static const char *lx_key(int i) { return P(XL_LEX[i]); }
static const char *lx_gloss(int i) { return NEXT(P(XL_LEX[i])); }

static int lex_pos(const char *key, int pos)
{
    int i = find(XL_LEX, NLEX, key);
    if (i < 0) return -1;
    for (; i < NLEX && streq(lx_key(i), key); i++)
        if (LX_POS(i) == pos) return i;
    return -1;
}

/* safe with i = -1 (no entry): class 0, no flags */
#define PH_A(i) ((i) >= 0 && (i) < NPHR ? XL_PHR_A[i] : 0)
#define PH_CLASS(i) (PH_A(i) & 15)
#define PH_HAS(i, f) ((PH_A(i) & (f)) != 0)
#define PH_PERSON(i) ((PH_A(i) >> XF_PERSON_SHIFT) & 7)
static const char *ph_key(int i) { return P(XL_PHR[i]); }
static const char *ph_en(int i) { return NEXT(P(XL_PHR[i])); }
static const char *ph_alt(int i) { const char *a = NEXT(ph_en(i)); return *a ? a : ph_en(i); }

/* The phrase/function entry for key, the written-with-accent one when
   acc says so and there is one. *other = the other spelling, or -1. */
static int phr_find(const char *key, int acc, int *other)
{
    int i = find(XL_PHR, NPHR, key), j;
    if (other) *other = -1;
    if (i < 0) return -1;
    j = (i + 1 < NPHR && streq(ph_key(i + 1), key)) ? i + 1 : -1;
    if (j < 0) return i;
    {
        int ia = PH_HAS(i, XF_ACC), pick = (acc ? ia : !ia) ? i : j;
        if (other) *other = pick == i ? j : i;
        return pick;
    }
}

/* ---------------------------------------------------------------- tokens */

enum { TK_WORD = 1, TK_NUM, TK_PUNCT, TK_OTHER };
enum { TF_CAP = 1, TF_UPPER = 2, TF_SPACE = 4, TF_ACCL = 8, TF_ACC = 16, TF_ALNUM = 32, TF_SYN = 64, TF_LONG = 128 };

typedef struct {
    uint16_t at, len;           /* source bytes */
    uint8_t kind, fl;
    uint8_t q;                  /* sentence is a question (1) / exclamation (2) */
    uint8_t sstart;             /* first token of a sentence */
    uint8_t oq;                 /* an inverted ? opened just before this token */
    char key[XL_KEY];
} xl_tok;

static xl_tok tk[XL_MAXTOK];
static int ntk;
static const char *src;
static int all_upper;           /* the whole input is in capitals */

static int is_laugh(const char *k)
{
    int n = (int)strlen(k), j = 0, i;
    if (n < 4) return 0;
    for (i = 0; i < n; i++) {
        char c = k[i];
        if (c == 'j') j++;
        else if (!(c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 's' || c == 'k' || c == 'h')) return 0;
    }
    return j * 4 >= n;
}

static void add_syn(const char *key, int at, int fl)
{
    if (ntk >= XL_MAXTOK) return;
    memset(&tk[ntk], 0, sizeof tk[ntk]);
    tk[ntk].at = (uint16_t)at;
    tk[ntk].kind = TK_WORD;
    tk[ntk].fl = (uint8_t)fl;
    snprintf(tk[ntk].key, XL_KEY, "%.*s", XL_KEY - 1, key);
    ntk++;
}

/* A chat spelling (q, xq, porfa) becomes the words it stands for. */
static void expand_abbr(int expand)
{
    xl_tok *t = &tk[ntk - 1];
    int i, at;
    char buf[48], *w, *sp;
    if (!expand || (t->fl & (TF_ALNUM | TF_LONG))) return;
    if (is_laugh(t->key)) { strcpy(t->key, "haha"); return; }
    if (t->len == 1 && (t->fl & TF_UPPER) && !all_upper) return;     /* C, D, X as letters */
    if (streq(t->key, "x") && ntk >= 2 && tk[ntk - 2].kind == TK_NUM) return;   /* 5 x 5 */
    i = find(XL_ABBR, NABBR, t->key);
    if (i < 0) {
        /* holaa, graciass: one doubled last letter */
        int n = (int)strlen(t->key);
        if (n > 3 && t->key[n - 1] == t->key[n - 2] && t->key[n - 1] != 'l' && t->key[n - 1] != 'r') {
            t->key[n - 1] = 0;
            if (phr_find(t->key, 0, 0) >= 0 || find(XL_LEX, NLEX, t->key) >= 0) return;
            t->key[n - 1] = t->key[n - 2];
        }
        return;
    }
    strncpy(buf, NEXT(P(XL_ABBR[i])), sizeof buf - 1);
    buf[sizeof buf - 1] = 0;
    at = t->at;
    w = buf;
    sp = strchr(w, ' ');
    if (sp) *sp = 0;
    snprintf(t->key, XL_KEY, "%.*s", XL_KEY - 1, w);
    t->fl &= (uint8_t)~(TF_ACCL | TF_ACC);
    while (sp) {
        w = sp + 1;
        sp = strchr(w, ' ');
        if (sp) *sp = 0;
        add_syn(w, at, TF_SPACE | TF_SYN);
    }
}

/* Tokens from src + pos until the batch is full (cut at a sentence end when
   possible). Returns where the next batch starts. */
static int tokenize(int pos, int expand)
{
    const unsigned char *s = (const unsigned char *)src;
    int space = 1, openq = 0, openx = 0;
    ntk = 0;
    while (s[pos]) {
        unsigned c = s[pos];
        int n, acc, up, l;
        if (ntk >= XL_MAXTOK - 6 || pos >= XL_BATCH) break;
        if (ntk >= XL_MAXTOK - 24 && ntk && tk[ntk - 1].kind == TK_PUNCT &&
            strchr(".!?\n", tk[ntk - 1].key[0])) break;
        if (c == ' ' || c == '\t' || c == '\r') { space = 1; pos++; continue; }
        if (c == 0xC2 && (s[pos + 1] == 0xBF || s[pos + 1] == 0xA1)) {   /* inverted ? ! */
            if (s[pos + 1] == 0xBF) openq = 1; else openx = 1;
            space = 1;
            pos += 2;
            continue;
        }
        xl_tok *t = &tk[ntk];
        memset(t, 0, sizeof *t);
        t->at = (uint16_t)pos;              /* a batch is under 2 KB */
        t->fl = (uint8_t)(space ? TF_SPACE : 0);
        space = 0;
        l = letter(s + pos, &n, &acc, &up);
        if (l || isdig((int)c)) {
            int k = 0, first = 1, uppers = 0, letters = 0, digits = 0, lastvacc = 0, start = pos;
            while (s[pos] && pos - start < 600 && pos < XL_BATCH + 600) {   /* longer runs: pieces */
                int ch = letter(s + pos, &n, &acc, &up);
                if (!ch && isdig(s[pos])) { ch = s[pos]; n = 1; digits++; }
                else if (!ch && s[pos] == '\'' && k && letter(s + pos + 1, &n, &acc, &up)) { ch = '\''; n = 1; }
                else if (!ch && (s[pos] == '.' || s[pos] == ',' || s[pos] == ':') && digits && !letters &&
                         isdig(s[pos + 1])) { ch = s[pos]; n = 1; }
                if (!ch) break;
                if (ch != '\'' && !isdig(ch) && ch != '.' && ch != ',' && ch != ':') {
                    letters++;
                    if (up) uppers++;
                    if (first && up) t->fl |= TF_CAP;
                    if (acc) t->fl |= TF_ACC;
                    if (isvow(ch)) lastvacc = acc;
                }
                first = 0;
                /* runs of three or more of one letter: one (holaaa, siii) */
                if (!(k >= 2 && t->key[k - 1] == ch && t->key[k - 2] == ch)) {
                    if (k < XL_KEY - 1) t->key[k++] = (char)ch;
                    else t->fl |= TF_LONG;
                } else if (k >= 2 && t->key[k - 1] == ch) {
                    /* already two: keep collapsing to one at the end */
                }
                pos += n;
            }
            t->key[k] = 0;
            /* collapse a remaining triple-run artefact: "aa" left from "aaa" */
            if (k >= 2 && t->key[k - 1] == t->key[k - 2] && pos - start > k && !digits) {
                t->key[--k] = 0;
            }
            t->len = (uint16_t)(pos - start);
            if (t->len > 40 || (k == 1 && t->len > 3)) t->fl |= TF_LONG;   /* aaaaaaa: not a word */
            if (lastvacc) t->fl |= TF_ACCL;
            if (letters && uppers == letters && letters > 1) t->fl |= TF_UPPER;
            if (letters == 1 && uppers == 1) t->fl |= TF_UPPER;
            if (digits && !letters) t->kind = TK_NUM;
            else {
                t->kind = TK_WORD;
                if (digits) t->fl |= TF_ALNUM;
            }
            ntk++;
            if (t->kind == TK_WORD) expand_abbr(expand);
        } else if (c < 0x80 && c != '\n') {
            int start = pos;
            t->kind = TK_PUNCT;
            t->key[0] = (char)c;
            pos++;
            if (c == '.' || c == '!' || c == '?') {
                while (s[pos] == '.' || s[pos] == '!' || s[pos] == '?') {
                    if (s[pos] == '?') t->key[0] = '?';
                    else if (s[pos] == '!' && t->key[0] != '?') t->key[0] = '!';
                    pos++;
                }
            }
            t->len = (uint16_t)(pos - start);
            ntk++;
        } else if (c == '\n') {
            t->kind = TK_PUNCT;
            t->key[0] = '\n';
            t->len = 1;
            pos++;
            ntk++;
            space = 1;
        } else {
            int start = pos;
            t->kind = TK_OTHER;
            pos++;
            while (s[pos] >= 0x80 && pos - start < 600 && !letter(s + pos, &n, &acc, &up) &&
                   !(s[pos] == 0xC2 && (s[pos + 1] == 0xBF || s[pos + 1] == 0xA1))) pos++;
            t->len = (uint16_t)(pos - start);
            ntk++;
        }
        if (openq && ntk) { tk[ntk - 1].q = 1; tk[ntk - 1].oq = 1; openq = 0; }
        if (openx && ntk) { if (!tk[ntk - 1].q) tk[ntk - 1].q = 2; openx = 0; }
    }
    /* sentences: question and exclamation marks reach back to the start */
    {
        int i = 0;
        while (i < ntk) {
            int j = i, q = 0;
            while (j < ntk && !(tk[j].kind == TK_PUNCT && strchr(".!?\n", tk[j].key[0]))) {
                if (tk[j].q) q = tk[j].q;
                j++;
            }
            int from = i, k0;
            for (k0 = i; k0 < j; k0++) if (tk[k0].oq) { from = k0; break; }   /* the question starts at ¿ */
            if (j < ntk && tk[j].key[0] == '?') {
                if (from == i && !tk[i].oq) {   /* no ¿: the question is the last clause (Mañana hay asado, vienen?) */
                    int k;
                    for (k = j - 1; k > i; k--)
                        if (tk[k].kind == TK_PUNCT && strchr(",;:", tk[k].key[0])) { from = k + 1; break; }
                }
                q = 1;
            } else if (j < ntk && tk[j].key[0] == '!' && q != 1) q = 2;
            tk[i].sstart = 1;
            for (int k = i; k <= j && k < ntk; k++) tk[k].q = (uint8_t)(k >= from ? q : 0);
            i = j + 1;
        }
    }
    return pos;
}

/* --------------------------------------------------------- Spanish words */

enum { LF_PL = 1, LF_DIM = 2, LF_SUP = 4, LF_RE = 8, LF_MENTE = 16, LF_NAME = 32 };

typedef struct {
    int16_t n, a, r, in;        /* lexicon entries: noun, adjective, adverb, interjection */
    uint8_t fl;                 /* LF_* for the noun / adjective */
} xl_nom;

static int try_pos(const char *key, int pos, int16_t *slot)
{
    if (*slot >= 0) return 1;
    int i = lex_pos(key, pos);
    if (i >= 0) *slot = (int16_t)i;
    return i >= 0;
}

static void exact(const char *key, xl_nom *o)
{
    int i = find(XL_LEX, NLEX, key);
    if (i < 0) return;
    for (; i < NLEX && streq(lx_key(i), key); i++) {
        int p = LX_POS(i);
        if (p == XP_N && o->n < 0) o->n = (int16_t)i;
        else if (p == XP_A && o->a < 0) o->a = (int16_t)i;
        else if (p == XP_R && o->r < 0) o->r = (int16_t)i;
        else if (p == XP_I && o->in < 0) o->in = (int16_t)i;
    }
}

static void cut(char *dst, const char *s, int n, const char *add)
{
    if (n < 0) n = 0;
    if (n > XL_KEY - 8) n = XL_KEY - 8;
    memcpy(dst, s, (size_t)n);
    dst[n] = 0;
    strcat(dst, add);
}

/* Plural and feminine forms back to the lemma: casas, luces, señales,
   cansada, buenas, españolas. */
static void inflected(const char *k, int n, xl_nom *o)
{
    char b[XL_KEY];
    if (o->n < 0) {
        if (ends(k, n, "ces")) { cut(b, k, n - 3, "z"); if (try_pos(b, XP_N, &o->n)) o->fl |= LF_PL; }
        if (o->n < 0 && ends(k, n, "es")) { cut(b, k, n - 2, ""); if (try_pos(b, XP_N, &o->n)) o->fl |= LF_PL; }
        if (o->n < 0 && ends(k, n, "s")) { cut(b, k, n - 1, ""); if (try_pos(b, XP_N, &o->n)) o->fl |= LF_PL; }
    }
    if (o->a < 0) {
        if (ends(k, n, "as")) { cut(b, k, n - 2, "o"); try_pos(b, XP_A, &o->a); }
        if (o->a < 0 && ends(k, n, "as")) { cut(b, k, n - 2, ""); try_pos(b, XP_A, &o->a); }
        if (o->a < 0 && ends(k, n, "a")) { cut(b, k, n - 1, "o"); try_pos(b, XP_A, &o->a); }
        if (o->a < 0 && ends(k, n, "a")) { cut(b, k, n - 1, ""); try_pos(b, XP_A, &o->a); }
        if (o->a < 0 && ends(k, n, "os")) { cut(b, k, n - 1, ""); try_pos(b, XP_A, &o->a); }
        if (o->a < 0 && ends(k, n, "ces")) { cut(b, k, n - 3, "z"); try_pos(b, XP_A, &o->a); }
        if (o->a < 0 && ends(k, n, "es")) { cut(b, k, n - 2, ""); try_pos(b, XP_A, &o->a); }
        if (o->a < 0 && ends(k, n, "s")) { cut(b, k, n - 1, ""); try_pos(b, XP_A, &o->a); }
    }
}

/* The stem of a diminutive or superlative with its ending put back. */
static int base_with(const char *stem, int sn, xl_nom *o, int pos_mask)
{
    static const char *const adds[] = { "o", "a", "", "e", "io" };
    char b[XL_KEY], s2[XL_KEY];
    int v;
    for (v = 0; v < 3; v++) {
        int m = sn;
        if (m <= 1) return 0;
        cut(s2, stem, m, "");
        if (v == 1) { if (ends(s2, m, "qu")) { s2[m - 2] = 'c'; s2[m - 1] = 0; } else continue; }
        if (v == 2) { if (ends(s2, m, "gu")) { s2[m - 1] = 0; } else continue; }
        for (unsigned a = 0; a < sizeof adds / sizeof adds[0]; a++) {
            cut(b, s2, (int)strlen(s2), adds[a]);
            if ((pos_mask & 1) && try_pos(b, XP_N, &o->n)) return 1;
            if ((pos_mask & 2) && try_pos(b, XP_A, &o->a)) return 1;
            if ((pos_mask & 4) && try_pos(b, XP_R, &o->r)) return 1;
        }
    }
    return 0;
}

static void derived(const char *k, int n, xl_nom *o)
{
    static const char *const dim[] = { "ecitos", "ecitas", "ecito", "ecita", "citos", "citas", "cito", "cita",
                                       "itos", "itas", "ito", "ita", "illos", "illas", "illo", "illa" };
    static const char *const sup[] = { "isimos", "isimas", "isimo", "isima" };
    unsigned i;
    for (i = 0; i < sizeof sup / sizeof sup[0]; i++)
        if (ends(k, n, sup[i]) && base_with(k, n - (int)strlen(sup[i]), o, 6)) { o->fl |= LF_SUP; return; }
    for (i = 0; i < sizeof dim / sizeof dim[0]; i++)
        if (ends(k, n, dim[i]) && base_with(k, n - (int)strlen(dim[i]), o, 7)) {
            o->fl |= LF_DIM;
            if (k[n - 1] == 's') o->fl |= LF_PL;
            return;
        }
    if (n > 7 && ends(k, n, "mente")) {
        char b[XL_KEY];
        cut(b, k, n - 5, "");
        if (b[strlen(b) - 1] == 'a') { b[strlen(b) - 1] = 'o'; }
        if (try_pos(b, XP_A, &o->a)) { o->fl |= LF_MENTE; return; }
        cut(b, k, n - 5, "");
        if (try_pos(b, XP_A, &o->a)) { o->fl |= LF_MENTE; return; }
    }
    if (n > 5 && k[0] == 'r' && k[1] == 'e') {
        xl_nom t = { -1, -1, -1, -1, 0 };
        int skip = (n > 9 && !memcmp(k, "requete", 7)) ? 7 : (n > 9 && !memcmp(k, "recontra", 8)) ? 8 : 2;
        exact(k + skip, &t);
        if (t.a < 0 && t.r < 0) inflected(k + skip, n - skip, &t);
        if (t.a >= 0 || t.r >= 0) { o->a = t.a; o->r = t.r; o->fl |= LF_RE; }
    }
}

static int nom_any(const xl_nom *o) { return o->n >= 0 || o->a >= 0 || o->r >= 0 || o->in >= 0; }

static void nominal(const char *k, xl_nom *o)
{
    int n = (int)strlen(k);
    o->n = o->a = o->r = o->in = -1;
    o->fl = 0;
    exact(k, o);
    if (o->n < 0 || o->a < 0) inflected(k, n, o);
    if (!nom_any(o)) derived(k, n, o);
}

/* ------------------------------------------------------------ Spanish verbs */

typedef struct {
    int16_t v;                  /* infinitive (lexicon index), -1 none */
    uint8_t t, p;               /* XT_* tense, person 1..6, 7 = first or third */
    int8_t score;               /* accent agreement */
    uint8_t ncl;
    int16_t cl[2];              /* clitics that were on the end: phrase-table indices */
    uint8_t imp;                /* read as an imperative (clitic on the end, vos form) */
} xl_verb;

static int acc_score(int word_last, int word_any, int form_acc)
{
    if (word_last) return form_acc ? 2 : -2;
    return (form_acc && word_any) ? -1 : 0;
}

static int verb_inf(const char *inf) { return lex_pos(inf, XP_V); }

/* Regular endings with stem and spelling changes undone, as gen_xlate.py's
   Reader does (keep the two in step: the generator lists only the forms this
   cannot read). */
static void regular(const char *k, int wl, int wa, xl_verb *best)
{
    int n = (int)strlen(k), e;
    int bscore = -100, brank = 9, border = 999;
    for (e = 0; e < NEND; e++) {
        const xl_ending_t *E = &XL_ENDINGS[e];
        int m = (int)strlen(E->end), sc, c;
        char stem[XL_KEY];
        if (n <= m || memcmp(k + n - m, E->end, (size_t)m)) continue;
        sc = acc_score(wl, wa, E->acc);
        cut(stem, k, n - m, "");
        if (E->cls & 8) {
            int sn = n - m, vi;
            if (sn < 3 || stem[sn - 1] != 'r' || !(stem[sn - 2] == 'a' || stem[sn - 2] == 'e' || stem[sn - 2] == 'i'))
                continue;
            vi = verb_inf(stem);
            if (vi >= 0 && (sc > bscore || (sc == bscore && LX_RANK(vi) < brank))) {
                bscore = sc; brank = LX_RANK(vi); border = e;
                best->v = (int16_t)vi; best->t = E->tense; best->p = E->person; best->score = (int8_t)sc;
            }
            continue;
        }
        for (c = 0; c < 3; c++) {
            static const char *const inf_end[3] = { "ar", "er", "ir" };
            char cand[8][XL_KEY];
            int nc = 1, ci, j;
            if (!(E->cls & (1 << c))) continue;
            strcpy(cand[0], stem);
            for (j = 0; j < NCHG && nc < 6; j++) {
                const xl_change_t *C = &XL_STEM_CHANGES[j];
                char *at, *last = 0;
                if (!(C->sets & E->sets)) continue;
                for (at = strstr(stem, C->from); at; at = strstr(at + 1, C->from)) last = at;
                if (!last) continue;
                {
                    int pre = (int)(last - stem);
                    cut(cand[nc], stem, pre, C->to);
                    strcat(cand[nc], last + strlen(C->from));
                    nc++;
                }
            }
            {
                int base_n = nc;
                for (ci = 0; ci < base_n && nc < 8; ci++) {
                    int sl = (int)strlen(cand[ci]);
                    for (j = 0; j < NSPELL && nc < 8; j++) {
                        const xl_change_t *S = &XL_SPELLING[j];
                        int fl = (int)strlen(S->from);
                        if (sl >= fl && !memcmp(cand[ci] + sl - fl, S->from, (size_t)fl)) {
                            cut(cand[nc], cand[ci], sl - fl, S->to);
                            nc++;
                        }
                    }
                }
            }
            for (ci = 0; ci < nc; ci++) {
                char inf[XL_KEY];
                int vi;
                if (!cand[ci][0]) continue;
                cut(inf, cand[ci], (int)strlen(cand[ci]), inf_end[c]);
                vi = verb_inf(inf);
                if (vi < 0) continue;
                if (sc > bscore || (sc == bscore && (LX_RANK(vi) < brank || (LX_RANK(vi) == brank && e < border)))) {
                    bscore = sc; brank = LX_RANK(vi); border = e;
                    best->v = (int16_t)vi; best->t = E->tense; best->p = E->person; best->score = (int8_t)sc;
                }
            }
        }
    }
}

static int verb_plain(const char *k, int wl, int wa, xl_verb *o)
{
    int i, bs = -100;
    o->v = -1; o->ncl = 0; o->imp = 0; o->score = 0;
    if (strlen(k) < 2) return 0;
    i = find(XL_IRR, NIRR, k);
    if (i >= 0) {
        for (; i < NIRR && streq(P(XL_IRR[i]), k); i++) {
            int sc = acc_score(wl, wa, (int)((XL_IRR[i] >> 29) & 1));
            if (sc > bs) {
                bs = sc;
                o->v = (int16_t)XL_IRR_LEX[i];
                o->t = (uint8_t)((XL_IRR[i] >> 22) & 15);
                o->p = (uint8_t)((XL_IRR[i] >> 26) & 7);
                o->score = (int8_t)sc;
            }
        }
        if (o->v >= 0 && bs >= -1) return 1;
    }
    {
        xl_verb r;
        r.v = -1;
        regular(k, wl, wa, &r);
        if (r.v >= 0 && (o->v < 0 || r.score > o->score)) {
            o->v = r.v; o->t = r.t; o->p = r.p; o->score = r.score;
        }
    }
    if (o->v < 0) {
        int vi = verb_inf(k);
        if (vi >= 0 && ends(k, (int)strlen(k), "r")) { o->v = (int16_t)vi; o->t = XT_INF; o->p = 0; }
    }
    return o->v >= 0;
}

static int clitic_index(const char *k)
{
    int o, i = phr_find(k, 0, &o);
    if (i >= 0 && PH_CLASS(i) == XC_CLIT) return i;
    if (i >= 0 && PH_CLASS(i) == XC_DET && PH_HAS(i, XF_CLIT)) return i;
    return -1;
}

/* Clitics on the end: decime, dámelo, verte, esperándote, quedate. */
static int verb_enclitic(const char *k, xl_verb *o)
{
    static const char *const cl[] = { "nos", "los", "las", "les", "me", "te", "se", "lo", "la", "le" };
    char w[XL_KEY];
    int n = (int)strlen(k), round, got[2], ng = 0;
    strcpy(w, k);
    for (round = 0; round < 2; round++) {
        unsigned c;
        int hit = 0;
        for (c = 0; c < sizeof cl / sizeof cl[0]; c++) {
            int m = (int)strlen(cl[c]);
            if (n - m >= 2 && !memcmp(w + n - m, cl[c], (size_t)m)) {
                char rest[XL_KEY];
                xl_verb r;
                cut(rest, w, n - m, "");
                got[ng] = clitic_index(cl[c]);
                if (verb_plain(rest, 0, 0, &r) &&
                    (r.t == XT_INF || r.t == XT_GER || r.t == XT_IMP || (r.t == XT_PRES && (r.p == 3 || r.p == 4)))) {
                    *o = r;
                    o->ncl = 0;
                    o->cl[o->ncl++] = (int16_t)(got[ng] < 0 ? 0 : got[ng]);
                    if (ng) o->cl[o->ncl++] = (int16_t)(got[0] < 0 ? 0 : got[0]);
                    if (o->ncl == 2) { int16_t t = o->cl[0]; o->cl[0] = o->cl[1]; o->cl[1] = t; }
                    if (r.t == XT_PRES || r.t == XT_IMP) { o->imp = 1; o->t = XT_IMP; o->p = 2; }
                    if (r.t == XT_PRES && r.p == 4) { o->p = 4; }
                    return 1;
                }
                if (ng == 0) {
                    ng = 1;
                    cut(w, w, n - m, "");
                    n -= m;
                    hit = 1;
                    break;
                }
            }
        }
        if (!hit) break;
    }
    return 0;
}

/* ------------------------------------------------------------------ items */

enum { IT_WORD = 1, IT_PHRASE, IT_NUM, IT_PUNCT, IT_OTHER };
enum { R_NONE = 0, R_FUNC, R_N, R_A, R_R, R_I, R_VERB, R_PASS, R_SKIP };

typedef struct {
    uint8_t t0, nt, kind, use;
    int16_t ph, ph2;            /* phrase or function entry, other accent spelling */
    xl_nom nm;
    xl_verb vb;
} xl_item;

static xl_item it[XL_MAXTOK];
static int nit;

static int tok_clause_end(int t)
{
    return t >= ntk || (tk[t].kind == TK_PUNCT && strchr(".,;:!?\n()", tk[t].key[0]));
}
static int tok_clause_start(int t)
{
    return t == 0 || tk[t].sstart || (tk[t - 1].kind == TK_PUNCT && strchr(".,;:!?\n()", tk[t - 1].key[0]));
}

static int is_name_tok(int t)
{
    /* capitalised in the middle of a sentence, or a short all-caps tag */
    if (tk[t].kind != TK_WORD) return 0;
    if (tk[t].fl & TF_ALNUM) return 1;
    if (all_upper) return 0;
    if ((tk[t].fl & TF_UPPER) && tk[t].len >= 2 && tk[t].len <= 5) return 1;
    return (tk[t].fl & TF_CAP) && !tk[t].sstart && !tok_clause_start(t);
}

static void build_items(void)
{
    int t = 0;
    nit = 0;
    while (t < ntk) {
        xl_item *I = &it[nit];
        memset(I, 0, sizeof *I);
        I->ph = I->ph2 = -1;
        I->nm.n = I->nm.a = I->nm.r = I->nm.in = -1;
        I->vb.v = -1;
        I->t0 = (uint8_t)t;
        I->nt = 1;
        if (tk[t].kind == TK_PUNCT) { I->kind = IT_PUNCT; nit++; t++; continue; }
        if (tk[t].kind == TK_NUM) { I->kind = IT_NUM; nit++; t++; continue; }
        if (tk[t].kind == TK_OTHER) { I->kind = IT_OTHER; nit++; t++; continue; }
        if (tk[t].fl & (TF_ALNUM | TF_LONG)) { I->kind = IT_WORD; I->use = R_PASS; nit++; t++; continue; }
        /* longest phrase of two or more words */
        {
            char buf[XL_KEY * 6 + 8];
            int best = -1, bestn = 0, k, n = 0, accany = 0;
            buf[0] = 0;
            for (k = 0; k < 6 && t + k < ntk && tk[t + k].kind == TK_WORD; k++) {
                if (k) strcat(buf, " ");
                strcat(buf, tk[t + k].key);
                if (tk[t + k].fl & TF_ACC) accany = 1;
                n = k + 1;
                if (n >= 2) {
                    int other, p = phr_find(buf, accany, &other);
                    if (p >= 0) { best = p; bestn = n; I->ph2 = (int16_t)other; }
                }
            }
            if (best >= 0) {
                int ok = 1, cls = PH_CLASS(best);
                uint16_t a = XL_PHR_A[best];
                int start = tok_clause_start(t), end = tok_clause_end(t + bestn);
                if ((a & XF_A) && !(start && end)) ok = 0;
                if ((a & XF_S) && !start) ok = 0;
                if ((a & XF_E) && !end) ok = 0;
                if (ok && cls == XC_CL && !start && nit > 0) {
                    /* a whole clause: where one starts, or after che, dale, y, pero, ya */
                    const xl_item *pv = &it[nit - 1];
                    int pc = pv->ph >= 0 ? PH_CLASS(pv->ph) : 0;
                    if (!(pc == XC_INTJ || pc == XC_CONJ || pc == XC_ADV || (pv->kind == IT_PHRASE && pc == XC_CL)))
                        ok = 0;
                }
                /* a phrase whose other spelling fits where this one does not */
                if (!ok && I->ph2 >= 0 && PH_CLASS(I->ph2) != XC_CL) {
                    uint16_t b = XL_PHR_A[I->ph2];
                    if (!((b & XF_A) && !(start && end)) && !((b & XF_S) && !start) && !((b & XF_E) && !end)) {
                        best = I->ph2; ok = 1;
                    }
                }
                if (ok) {
                    I->kind = IT_PHRASE;
                    I->ph = (int16_t)best;
                    I->nt = (uint8_t)bestn;
                    I->use = R_FUNC;
                    nit++;
                    t += bestn;
                    continue;
                }
                I->ph2 = -1;
            }
        }
        /* one word */
        I->kind = IT_WORD;
        {
            const char *k = tk[t].key;
            int acc = (tk[t].fl & TF_ACC) != 0, other;
            int p = phr_find(k, acc, &other);
            if (p >= 0) {
                uint16_t a = XL_PHR_A[p];
                int start = tok_clause_start(t), end = tok_clause_end(t + 1);
                if (((a & XF_A) && !(start && end)) || ((a & XF_S) && !start) || ((a & XF_E) && !end)) {
                    p = other;
                    other = -1;
                    if (p >= 0) {
                        a = XL_PHR_A[p];
                        if (((a & XF_A) && !(start && end)) || ((a & XF_S) && !start) || ((a & XF_E) && !end)) p = -1;
                    }
                }
            }
            I->ph = (int16_t)p;
            I->ph2 = (int16_t)other;
            nominal(k, &I->nm);
            if (!verb_plain(k, (tk[t].fl & TF_ACCL) != 0, acc, &I->vb) && !nom_any(&I->nm) && p < 0)
                verb_enclitic(k, &I->vb);
            /* está, sé, dé: written with the accent the function word lacks */
            if (p >= 0 && acc && !PH_HAS(p, XF_ACC) && I->vb.v >= 0 && I->vb.score > 0) { I->ph = -1; I->ph2 = (int16_t)p; }
            if (I->vb.v >= 0 && I->vb.t == XT_IMP && I->vb.ncl == 0) I->vb.imp = 1;
            /* no reading: n-tilde typed as n (manana, senal), k for qu (kiero) */
            if (p < 0 && !nom_any(&I->nm) && I->vb.v < 0) {
                char b[XL_KEY];
                int n = (int)strlen(k), j;
                for (j = 0; j < n; j++) {
                    int o2, pf;
                    if (k[j] != 'n') continue;
                    strcpy(b, k);
                    b[j] = (char)0xF1;
                    pf = phr_find(b, 0, &o2);
                    if (pf >= 0) { I->ph = (int16_t)pf; I->ph2 = (int16_t)o2; break; }
                    nominal(b, &I->nm);
                    if (nom_any(&I->nm)) break;
                    if (verb_plain(b, 0, 0, &I->vb)) break;
                }
                if (!nom_any(&I->nm) && I->vb.v < 0 && k[0] == 'k' && n < XL_KEY - 2) {
                    b[0] = 'q'; b[1] = 'u';
                    strcpy(b + 2, k + 1);
                    if (!verb_plain(b, 0, 0, &I->vb)) nominal(b, &I->nm);
                }
            }
        }
        nit++;
        t++;
    }
}

/* -------------------------------------------------------------- English */

typedef struct { uint16_t off; uint8_t len, fl; } xl_chunk;
enum { CF_GLUE = 1, CF_PASS = 2, CF_SENT = 4, CF_CAPSRC = 8, CF_GLUER = 16, CF_NL = 32 };

static char arena[XL_ARENA];
static int narena;
static xl_chunk ch[XL_CHUNKS];
static int nch;

static int emit_n(const char *s, int n, int fl)
{
    if (n > 255) {                  /* a long pass-through: pieces glued together */
        int first = emit_n(s, 255, fl);
        while (first >= 0 && (n -= 255) > 0) {
            s += 255;
            if (emit_n(s, n > 255 ? 255 : n, CF_GLUE | CF_PASS) < 0) break;
        }
        return first;
    }
    if (n <= 0 || nch >= XL_CHUNKS || narena + n > XL_ARENA) return -1;
    memcpy(arena + narena, s, (size_t)n);
    ch[nch].off = (uint16_t)narena;
    ch[nch].len = (uint8_t)n;
    ch[nch].fl = (uint8_t)fl;
    narena += n;
    return nch++;
}
static int emit(const char *s) { return emit_n(s, (int)strlen(s), 0); }

/* The last English piece ends in "to" (I'm going to, to). */
static int said_to(void)
{
    int c = nch - 1;
    while (c >= 0 && !ch[c].len) c--;
    if (c < 0 || ch[c].len < 2) return 0;
    {
        const char *e = arena + ch[c].off + ch[c].len - 2;
        return e[0] == 't' && e[1] == 'o' && (ch[c].len == 2 || e[-1] == ' ');
    }
}

/* Move chunk `from` (the last one usually) to position `to`. */
static void move_chunk(int from, int to)
{
    xl_chunk c;
    if (from < 0 || to < 0 || from >= nch || to >= nch || from == to) return;
    c = ch[from];
    if (from > to) memmove(&ch[to + 1], &ch[to], sizeof(xl_chunk) * (size_t)(from - to));
    else memmove(&ch[from], &ch[from + 1], sizeof(xl_chunk) * (size_t)(to - from));
    ch[to] = c;
}
static int is_vowel_start(const char *w)
{
    char c = (char)lowc(w[0]);
    if (!strncmp(w, "uni", 3) || !strncmp(w, "use", 3) || !strncmp(w, "one", 3) || !strncmp(w, "eu", 2)) return 0;
    if (!strncmp(w, "hour", 4) || !strncmp(w, "honest", 6)) return 1;
    return isvow(c);
}

static int en_find(const uint32_t *t, int n, const char *w) { return find(t, n, w); }

/* English verb forms of the first word of a gloss. */
enum { F_BASE, F_S, F_PAST, F_PP, F_ING };

static void en_form(const char *verb, int form, char *dst, int cap)
{
    char w[32];
    int n = 0, i, vg = 0, prev = 0;
    while (verb[n] && verb[n] != ' ' && verb[n] != '+' && n < 30) { w[n] = verb[n]; n++; }
    w[n] = 0;
    dst[0] = 0;
    if (form == F_BASE || !n) { snprintf(dst, (size_t)cap, "%s", w); return; }
    if (streq(w, "be")) {
        snprintf(dst, (size_t)cap, "%s", form == F_PAST ? "was" : form == F_PP ? "been" : form == F_ING ? "being" : "is");
        return;
    }
    if (form == F_S) {
        if (streq(w, "have")) { snprintf(dst, (size_t)cap, "has"); return; }
        if (streq(w, "do") || streq(w, "go")) { snprintf(dst, (size_t)cap, "%ses", w); return; }
        if (n >= 2 && w[n - 1] == 'y' && !isvow(w[n - 2])) { w[n - 1] = 0; snprintf(dst, (size_t)cap, "%sies", w); return; }
        if (ends(w, n, "s") || ends(w, n, "x") || ends(w, n, "z") || ends(w, n, "ch") || ends(w, n, "sh")) {
            snprintf(dst, (size_t)cap, "%ses", w); return;
        }
        snprintf(dst, (size_t)cap, "%ss", w);
        return;
    }
    if (form == F_PAST || form == F_PP) {
        int e = en_find(XL_ENV, NENV, w);
        if (e >= 0) {
            const char *past = NEXT(P(XL_ENV[e]));
            snprintf(dst, (size_t)cap, "%s", form == F_PAST ? past : NEXT(past));
            return;
        }
    }
    /* doubling: one vowel group, ending consonant-vowel-consonant (stop, plan) */
    for (i = 0; i < n; i++) {
        int v = isvow(w[i]);
        if (v && !prev) vg++;
        prev = v;
    }
    if (form == F_ING) {
        if (ends(w, n, "ie")) { w[n - 2] = 0; snprintf(dst, (size_t)cap, "%sying", w); return; }
        if (n > 2 && w[n - 1] == 'e' && w[n - 2] != 'e' && w[n - 2] != 'y' && w[n - 2] != 'o') {
            w[n - 1] = 0; snprintf(dst, (size_t)cap, "%sing", w); return;
        }
    } else {
        if (w[n - 1] == 'e') { snprintf(dst, (size_t)cap, "%sd", w); return; }
        if (n >= 2 && w[n - 1] == 'y' && !isvow(w[n - 2])) { w[n - 1] = 0; snprintf(dst, (size_t)cap, "%sied", w); return; }
    }
    if (vg == 1 && n >= 3 && !isvow(w[n - 1]) && isvow(w[n - 2]) && !isvow(w[n - 3]) &&
        w[n - 1] != 'w' && w[n - 1] != 'x' && w[n - 1] != 'y') {
        snprintf(dst, (size_t)cap, "%s%c%s", w, w[n - 1], form == F_ING ? "ing" : "ed");
        return;
    }
    snprintf(dst, (size_t)cap, "%s%s", w, form == F_ING ? "ing" : "ed");
}

/* The gloss after its first word: " up", " care of", "+for", " * know". */
static const char *gloss_rest(const char *g)
{
    while (*g && *g != ' ' && *g != '+') g++;
    return g;
}

static void en_plural(const char *g, char *dst, int cap)
{
    const char *last = strrchr(g, ' ');
    char head[64], w[32];
    int hn, n, e;
    last = last ? last + 1 : g;
    hn = (int)(last - g);
    if (hn > 60) hn = 60;
    memcpy(head, g, (size_t)hn);
    head[hn] = 0;
    snprintf(w, sizeof w, "%.31s", last);
    n = (int)strlen(w);
    e = en_find(XL_ENPL, NENPL, w);
    if (e >= 0) {
        const char *pl = NEXT(P(XL_ENPL[e]));
        snprintf(dst, (size_t)cap, "%.36s%.20s", head, streq(pl, "-") ? w : pl);
        return;
    }
    if (!n) { snprintf(dst, (size_t)cap, "%s", g); return; }
    if (n >= 2 && w[n - 1] == 'y' && !isvow(w[n - 2])) { w[n - 1] = 0; snprintf(dst, (size_t)cap, "%.36s%.20sies", head, w); return; }
    if (ends(w, n, "s") || ends(w, n, "x") || ends(w, n, "z") || ends(w, n, "ch") || ends(w, n, "sh")) {
        snprintf(dst, (size_t)cap, "%.36s%.20ses", head, w); return;
    }
    snprintf(dst, (size_t)cap, "%.36s%.20ss", head, w);
}

/* bigger, nicer, easier, better; more expensive. sup: biggest, most expensive */
static void en_compare(const char *g, int sup, char *dst, int cap)
{
    char w[40];
    int n, i, vg = 0, prev = 0;
    snprintf(w, sizeof w, "%s", g);
    n = (int)strlen(w);
    if (streq(w, "good") || streq(w, "well")) { snprintf(dst, (size_t)cap, sup ? "best" : "better"); return; }
    if (streq(w, "bad") || streq(w, "badly")) { snprintf(dst, (size_t)cap, sup ? "worst" : "worse"); return; }
    if (streq(w, "far")) { snprintf(dst, (size_t)cap, sup ? "farthest" : "farther"); return; }
    for (i = 0; i < n; i++) { int v = isvow(w[i]) || (w[i] == 'y' && i == n - 1); if (v && !prev) vg++; prev = v; }
    if (strchr(w, ' ') || n < 2 || vg > 2 || (vg == 2 && w[n - 1] != 'y')) {
        snprintf(dst, (size_t)cap, "%s %s", sup ? "most" : "more", w);
        return;
    }
    if (w[n - 1] == 'y' && !isvow(w[n - 2])) { w[n - 1] = 0; snprintf(dst, (size_t)cap, "%si%s", w, sup ? "est" : "er"); return; }
    if (w[n - 1] == 'e') { snprintf(dst, (size_t)cap, "%s%s", w, sup ? "st" : "r"); return; }
    if (n >= 3 && !isvow(w[n - 1]) && isvow(w[n - 2]) && !isvow(w[n - 3]) && w[n - 1] != 'w' && w[n - 1] != 'x' &&
        w[n - 1] != 'y') {
        snprintf(dst, (size_t)cap, "%s%c%s", w, w[n - 1], sup ? "est" : "er");
        return;
    }
    snprintf(dst, (size_t)cap, "%s%s", w, sup ? "est" : "er");
}

static void en_adverb(const char *g, char *dst, int cap)
{
    char w[40];
    int n;
    snprintf(w, sizeof w, "%.39s", g);
    n = (int)strlen(w);
    if (n >= 2 && w[n - 1] == 'y') { w[n - 1] = 0; snprintf(dst, (size_t)cap, "%sily", w); return; }
    if (ends(w, n, "le")) { w[n - 1] = 0; snprintf(dst, (size_t)cap, "%sy", w); return; }
    if (ends(w, n, "ic")) { snprintf(dst, (size_t)cap, "%sally", w); return; }
    if (ends(w, n, "ly")) { snprintf(dst, (size_t)cap, "%s", w); return; }
    snprintf(dst, (size_t)cap, "%sly", w);
}

/* --------------------------------------------------------------- reading */

typedef struct {
    int cstart;                 /* item where the clause starts */
    int subj_ch;                /* chunk where an explicit subject starts, -1 */
    int subj_p;                 /* its English person: 1, 2 (you), 3, 4, 6 */
    int np_ch, np_end;          /* last noun phrase: first chunk, item after it */
    int np_pl, np_clause;       /* plural; it started the clause */
    int neg, negcl;             /* "no" waiting for a verb; the clause is negative */
    int ncl, cl[3];             /* object pronouns waiting for a verb */
    int adv;                    /* adverb waiting for a verb (phrase index), -1 */
    int qword;                  /* the clause opened with a question word */
    int hadverb, lastverb;      /* a finite verb in this clause; its infinitive */
    const char *suffix;         /* said at the end of the clause (anymore, yet, ago) */
    int more;                   /* a comparison is open: que = than */
} xl_state;

static xl_state st;
static int sent_q;              /* the sentence is a question (1) or an exclamation (2) */
static int no_to;               /* the caller already wrote "to" (or what to, without) */
static int keep_a;              /* "a" before a noun stands for a dropped clitic: say "to" */
static int usted_seen;          /* usted or su in a question: su is "your" */
static int q_done;              /* the question's verb was turned round already */
static int last_imper;          /* the last verb was a command (doblá ... y seguí) */
static int rel_subj;            /* the clause is "que" after a noun: that noun is the subject */
static int last_person;         /* person of the sentence's last verb (dijo que venía) */
static char compl_buf[48];

static int lemma_is(int v, const char *inf) { return v >= 0 && streq(lx_key(v), inf); }

static int item_func_class(int i)
{
    if (i < 0 || i >= nit || it[i].ph < 0) return 0;
    return PH_CLASS(it[i].ph);
}
static int item_is(int i, const char *key)
{
    return i >= 0 && i < nit && it[i].kind == IT_WORD && streq(tk[it[i].t0].key, key);
}
static int item_punct(int i)
{
    return i >= nit || it[i].kind == IT_PUNCT;
}

static int is_finite_t(int t) { return t != XT_INF && t != XT_GER && t != XT_PART; }

/* Could item i be a noun here (it has a noun reading and no function word
   that wins)? */
static int has_noun(int i)
{
    return i < nit && it[i].kind == IT_WORD && it[i].nm.n >= 0;
}
static int has_adj(int i)
{
    return i < nit && it[i].kind == IT_WORD && (it[i].nm.a >= 0 ||
           (it[i].vb.v >= 0 && it[i].vb.t == XT_PART));
}
static int verbish(int i)
{
    return i < nit && it[i].kind == IT_WORD && it[i].vb.v >= 0 && it[i].ph < 0 && it[i].nm.n < 0 && it[i].nm.a < 0;
}
static int has_verb(int i)
{
    return i < nit && it[i].kind == IT_WORD && it[i].vb.v >= 0;
}

static int decide(int i);

/* A word the lexicon does not know but that ends like a verb (se inundó). */
static int unknown_verbish(int i)
{
    const char *k;
    int n;
    if (i >= nit || it[i].kind != IT_WORD || it[i].ph >= 0 || it[i].vb.v >= 0 || nom_any(&it[i].nm)) return 0;
    if (is_name_tok(it[i].t0)) return 0;
    k = tk[it[i].t0].key;
    n = (int)strlen(k);
    return (n > 3 && k[n - 1] == 'o' && (tk[it[i].t0].fl & TF_ACCL)) || ends(k, n, "aron") || ends(k, n, "ieron") ||
           ends(k, n, "aba") || ends(k, n, "ando") || ends(k, n, "iendo") || ends(k, n, "ado") || ends(k, n, "ido");
}

/* Skipping object pronouns and "no", does a verb come next? */
static int verb_follows(int i)
{
    int j;
    for (j = i; j < nit && j < i + 4; j++) {
        if (it[j].kind != IT_WORD) return 0;
        if (it[j].ph >= 0) {
            int c = PH_CLASS(it[j].ph);
            if (c == XC_CLIT || (c == XC_DET && PH_HAS(it[j].ph, XF_CLIT) && has_verb(j + 1) && !has_noun(j + 1)) ||
                c == XC_NEG || (c == XC_ADV && j > i)) continue;
            if (item_is(j, "se")) continue;
        }
        return has_verb(j) && (it[j].ph < 0 || PH_CLASS(it[j].ph) == XC_INTJ || it[j].vb.score > 0 ||
                               (it[j].ph >= 0 && PH_CLASS(it[j].ph) == XC_DET && !has_noun(j + 1)));
    }
    return 0;
}

static int decide(int i)
{
    xl_item *I = &it[i];
    int prev = i - 1, next = i + 1, pc = item_func_class(prev);
    if (I->kind != IT_WORD) return R_NONE;
    if (I->use) return I->use;
    if (is_name_tok(I->t0) && !(I->ph >= 0 && (tk[I->t0].fl & TF_UPPER) == 0)) return R_PASS;
    if (I->ph >= 0) {
        int c = PH_CLASS(I->ph);
        const char *k = tk[I->t0].key;
        /* este/esta vs está; el vs él; tu vs tú */
        if (c == XC_DET && I->vb.v >= 0 && (lemma_is(I->vb.v, "estar"))) {
            if ((tk[I->t0].fl & TF_ACC) || !(has_noun(next) || has_adj(next))) return R_VERB;
            if (has_adj(next) && !has_noun(next)) return R_VERB;
            return R_FUNC;
        }
        if (c == XC_DET && (streq(k, "el") || streq(k, "tu")) && has_verb(next) && !has_noun(next) && !has_adj(next)) {
            return R_FUNC;      /* handled as a pronoun by the caller */
        }
        /* mañana after an article: the morning */
        if (c == XC_ADV && (pc == XC_DET) && I->nm.n >= 0) return R_N;
        /* bajo, claro, bueno, listo as adjectives */
        if ((c == XC_PREP || c == XC_INTJ || c == XC_ADV) && I->nm.a >= 0 && c != XC_ADV) {
            int after_be = prev >= 0 && it[prev].use == R_VERB && (lemma_is(it[prev].vb.v, "estar") || lemma_is(it[prev].vb.v, "ser"));
            if (after_be || (prev >= 0 && it[prev].use == R_N) || (pc == XC_ADV && PH_HAS(it[prev].ph, XF_INT)) ||
                (has_noun(next) && c != XC_PREP && !(it[next].ph >= 0)))
                return R_A;
        }
        return R_FUNC;
    }
    {
        int n = I->nm.n >= 0, a = I->nm.a >= 0, v = I->vb.v >= 0;
        int after_det = pc == XC_DET || pc == XC_NUM || (prev >= 0 && it[prev].kind == IT_NUM);
        int after_adj = prev >= 0 && it[prev].use == R_A;
        if (v && I->vb.ncl) return R_VERB;
        if (v && (n || a)) {
            int pfin = is_finite_t(I->vb.t);
            if (after_det || after_adj) return n ? R_N : R_A;
            if (pc == XC_PREP && I->vb.t != XT_INF) return n ? R_N : R_A;
            if (pc == XC_PRON || pc == XC_CLIT || pc == XC_NEG || item_is(prev, "se") || item_is(prev, "que") ||
                item_is(prev, "ya") || (pc == XC_DET && prev >= 0 && PH_HAS(it[prev].ph, XF_CLIT) && it[prev].use == R_FUNC &&
                                         st.ncl > 0))
                return pfin || I->vb.t == XT_INF ? R_VERB : (a ? R_A : R_N);
            if (I->vb.t == XT_PART) {
                int after_aux = prev >= 0 && it[prev].use == R_VERB &&
                                (lemma_is(it[prev].vb.v, "estar") || lemma_is(it[prev].vb.v, "ser"));
                if (a) return R_A;
                return (n && !after_aux) ? R_N : R_VERB;
            }
            if (I->vb.t == XT_GER) return R_VERB;
            if (I->vb.t == XT_INF) return R_VERB;
            /* after a finite verb, a noun is its object (traje vino) */
            if (st.hadverb) return n ? R_N : R_A;
            if (prev >= 0 && it[prev].use == R_N && a) return R_A;
            if (I->vb.score > 0) return R_VERB;
            /* a verb is more likely when nothing else in the clause is one */
            {
                int j, other = 0;
                for (j = i + 1; j < nit && !item_punct(j); j++)
                    if (verbish(j) && is_finite_t(it[j].vb.t)) { other = 1; break; }
                /* except a clause that is only it and an adjective, a label
                   rather than a sentence: "Prueba final," is a final test */
                if (!other && pfin && n && i == st.cstart && has_adj(next) && !has_verb(next) &&
                    (next + 1 >= nit || item_punct(next + 1)))
                    return R_N;
                if (!other && pfin) return R_VERB;
            }
            return n ? R_N : R_A;
        }
        if (v) return R_VERB;
        if (n && a) {
            if (prev >= 0 && it[prev].use == R_VERB && item_is(prev, "hay")) return R_N;   /* hay heridos */
            if (prev >= 0 && (it[prev].use == R_N)) return R_A;
            if (after_det && !has_noun(next)) return R_N;
            if (has_noun(next) && !after_det) return R_A;
            if (prev >= 0 && it[prev].use == R_VERB) return R_A;
            if (pc == XC_ADV && PH_HAS(it[prev].ph, XF_INT)) return R_A;
            return (i == st.cstart || pc == XC_PREP) ? R_N : R_A;
        }
        if (n) return R_N;
        if (a) return R_A;
        if (I->nm.r >= 0) return R_R;
        if (I->nm.in >= 0) return R_I;
    }
    return R_PASS;
}

/* -------------------------------------------------------- English pieces */

static int emit_item_src(int i, int fl)
{
    const xl_tok *t = &tk[it[i].t0];
    int last = it[i].t0 + it[i].nt - 1;
    int n = tk[last].at + tk[last].len - t->at;
    if (t->fl & TF_SYN) return -1;
    return emit_n(src + t->at, n, fl | CF_PASS);
}

static const char *subject_word(int p, int v)
{
    switch (p) {
    case 1: case 7: return "I";
    case 2: case 5: return "you";
    case 4: return "we";
    case 6: return "they";
    }
    /* 3rd person singular: "it" for verbs that are mostly about things */
    {
        static const char *const it_verbs[] = { "ser", "estar", "haber", "parecer", "llover", "nevar", "funcionar",
            "andar", "pasar", "costar", "servir", "faltar", "quedar", "sonar", "doler", "importar", "tardar",
            "demorar", "convenir", "valer", "bastar", "ocurrir", "suceder", "gustar", "encantar", "cortarse",
            "apagarse", "romperse", "caerse", "empezar", "terminar", "cargar", "molestar", "interesar", "existir" };
        unsigned k;
        for (k = 0; k < sizeof it_verbs / sizeof it_verbs[0]; k++)
            if (lemma_is(v, it_verbs[k])) return "it";
    }
    return "he";
}

/* English person class for agreement: 1 = I, 3 = he/she/it, 0 = the rest */
static int agree(int p) { return (p == 1 || p == 7) ? 1 : p == 3 ? 3 : 0; }

static const char *be_form(int past, int a)
{
    if (past) return (a == 1 || a == 3) ? "was" : "were";
    return a == 1 ? "am" : a == 3 ? "is" : "are";
}

static void negate(char *w, int cap)
{
    static const char *const pairs[][2] = {
        { "do", "don't" }, { "does", "doesn't" }, { "did", "didn't" }, { "is", "isn't" }, { "are", "aren't" },
        { "was", "wasn't" }, { "were", "weren't" }, { "have", "haven't" }, { "has", "hasn't" }, { "had", "hadn't" },
        { "will", "won't" }, { "would", "wouldn't" }, { "can", "can't" }, { "could", "couldn't" },
        { "must", "mustn't" }, { "should", "shouldn't" }, { "am", "am not" } };
    unsigned k;
    for (k = 0; k < sizeof pairs / sizeof pairs[0]; k++)
        if (streq(w, pairs[k][0])) { snprintf(w, (size_t)cap, "%s", pairs[k][1]); return; }
    snprintf(w + strlen(w), (size_t)cap - strlen(w), " not");
}

/* Object pronoun for a clitic (phrase index). */
static const char *object_word(int ph, int v)
{
    const char *k = ph >= 0 ? ph_key(ph) : "";
    if (streq(k, "me")) return "me";
    if (streq(k, "te") || streq(k, "os")) return "you";
    if (streq(k, "nos")) return "us";
    if (streq(k, "los") || streq(k, "las") || streq(k, "les")) return "them";
    if (streq(k, "le") || streq(k, "se")) return "him";
    if (streq(k, "lo") || streq(k, "la")) {
        static const char *const people[] = { "conocer", "llamar", "querer", "amar", "extrañar", "besar",
                                              "ayudar", "invitar", "odiar", "matar" };
        unsigned j;
        for (j = 0; j < sizeof people / sizeof people[0]; j++)
            if (lemma_is(v, people[j])) return streq(k, "la") ? "her" : "him";
        return "it";
    }
    return "it";
}

static int clitic_person(int ph)
{
    const char *k = ph >= 0 ? ph_key(ph) : "";
    if (streq(k, "me")) return 1;
    if (streq(k, "te")) return 2;
    if (streq(k, "nos")) return 4;
    if (streq(k, "os")) return 5;
    if (streq(k, "se")) return 3;
    return 0;
}
static int clitic_direct(int ph)
{
    const char *k = ph >= 0 ? ph_key(ph) : "";
    return streq(k, "lo") || streq(k, "la") || streq(k, "los") || streq(k, "las");
}

/* Main verb gloss with its object pronouns: "look for me", "let you know". */
static void emit_main(const char *g, int form, const char *obj1, const char *obj2, int fl)
{
    char w[48], buf[96];
    const char *rest = gloss_rest(g);
    if (!*g) { if (obj1) emit(obj1); return; }
    en_form(g, form, w, sizeof w);
    if (*rest == '+') {
        /* "wait+for": the particle only before an object */
        if (obj1) snprintf(buf, sizeof buf, "%s %s %s%s%s", w, rest + 1, obj1, obj2 ? " to " : "", obj2 ? obj2 : "");
        else snprintf(buf, sizeof buf, "%s", w);
    } else if (strstr(rest, "*")) {
        const char *star = strstr(rest, "*");
        snprintf(buf, sizeof buf, "%s%.*s%s%s", w, (int)(star - rest), rest, obj1 ? obj1 : "us", star + 1);
    } else if (obj1) {
        if (obj2) snprintf(buf, sizeof buf, "%s%s %s to %s", w, rest, obj1, obj2);
        else snprintf(buf, sizeof buf, "%s%s %s", w, rest, obj1);
    } else {
        snprintf(buf, sizeof buf, "%s%s", w, rest);
    }
    emit_n(buf, (int)strlen(buf), fl);
}

/* ------------------------------------------------------------ verb phrase */

enum { A_NONE, A_PERF, A_PROG, A_GOING, A_HAVETO, A_CAN, A_MUST, A_TO, A_JUST, A_AGAIN, A_STOP, A_START,
       A_KEEP, A_USUALLY, A_LETS };

typedef struct {
    int tense;                  /* XT_* of the finite Spanish verb */
    int person;                 /* Spanish person (1..7) */
    int en_person;              /* English person (1, 2, 3, 4, 6) */
    const char *subj;           /* pronoun to say, NULL when explicit or none */
    int aux[3], naux;           /* A_* steps before the main verb */
    const char *vglue[3];       /* gloss of the verb of an A_TO / A_START step */
    int main_v;                 /* infinitive of the main verb */
    const char *gloss;          /* its English */
    int ncl, cl[4];             /* object pronouns (phrase indices) */
    int imper;
    int there;                  /* hay: there is / are */
    int there_pl;
    const char *compl;          /* complement replacing the object (tener frío -> cold) */
    int gustar;                 /* gustar-type: the object is the Spanish subject */
} xl_vp;

static const char *vp_gloss(int v) { return v >= 0 ? lx_gloss(v) : "do"; }

/* Write a verb phrase. Returns the chunk where its finite part went. */
static int vp_write(xl_vp *V, int q, int neg, const char *adverb, int subj_ch)
{
    char fin[40], word[48];
    int past = 0, will = 0, would = 0, a = agree(V->en_person), k, fin_ch = -1;
    int nonfin_form = F_BASE;               /* form of the next verb after the finite part */
    const char *first;                      /* the gloss that carries the tense */
    int first_is_aux = 0, start_step = 0;
    const char *obj1 = 0, *obj2 = 0;
    int t = V->tense;

    if (t == XT_PRET || t == XT_IMPF || t == XT_SUBJI) past = 1;
    if (V->there) a = V->there_pl ? 0 : 3;      /* there is / there are */
    if (t == XT_FUT) will = 1;
    if (t == XT_COND) would = 1;

    /* objects: indirect before direct in English as "verb DO to IO" */
    for (k = 0; k < V->ncl; k++) {
        const char *o = object_word(V->cl[k], V->main_v);
        if (!obj1) obj1 = o;
        else if (!obj2) {
            if (clitic_direct(V->cl[k])) { obj2 = obj1; obj1 = o; } else obj2 = o;
        }
    }
    if (V->gustar) obj1 = obj2 = 0;

    /* the finite word */
    first = V->naux ? 0 : V->gloss;
    if (V->naux) {
        switch (V->aux[0]) {
        case A_PERF: first = "have"; first_is_aux = 1; nonfin_form = F_PP; break;
        case A_PROG: first = "be"; first_is_aux = 1; nonfin_form = F_ING; break;
        case A_GOING: first = "be"; first_is_aux = 1; break;
        case A_HAVETO: first = "have"; break;
        case A_CAN: first = "can"; first_is_aux = 2; break;
        case A_MUST: first = "must"; first_is_aux = 2; break;
        case A_TO: case A_START: first = V->vglue[0]; break;
        case A_STOP: first = "stop"; nonfin_form = F_ING; break;
        case A_KEEP: first = "keep"; nonfin_form = F_ING; break;
        case A_JUST: first = V->gloss; past = 1; will = would = 0; break;
        case A_USUALLY: first = V->gloss; break;
        case A_AGAIN: first = V->gloss; break;
        case A_LETS: first = "let's"; first_is_aux = 3; break;
        }
        start_step = (V->aux[0] == A_JUST || V->aux[0] == A_USUALLY || V->aux[0] == A_AGAIN) ? 1 : 0;
    }
    if (V->there) { first = "be"; }
    if (streq(first, "be") || (first_is_aux == 1 && streq(first, "have"))) first_is_aux = first_is_aux ? first_is_aux : 1;

    /* tense on the finite word */
    fin[0] = 0;
    if (V->imper) {
        if (neg) snprintf(fin, sizeof fin, "don't");
    } else if (first_is_aux == 2) {
        const char *m = first;
        if (streq(m, "can")) m = (past || would) ? "could" : will ? "will" : "can";
        else if (streq(m, "must")) m = would ? "should" : past ? "had to" : will ? "will have to" : "must";
        snprintf(fin, sizeof fin, "%s", m);
        if (will && streq(first, "can")) { snprintf(fin, sizeof fin, "will"); }
    } else if (first_is_aux == 3) {
        snprintf(fin, sizeof fin, "%s", first);
    } else if (will || would) {
        snprintf(fin, sizeof fin, "%s", will ? "will" : "would");
    } else if (streq(first, "be")) {
        snprintf(fin, sizeof fin, "%s", be_form(past, a));
    } else if (first_is_aux == 1 && streq(first, "have")) {
        snprintf(fin, sizeof fin, "%s", past ? "had" : a == 3 ? "has" : "have");
    } else if (neg || q) {
        snprintf(fin, sizeof fin, "%s", past ? "did" : a == 3 ? "does" : "do");
    }
    if (neg && fin[0] && !V->imper) negate(fin, sizeof fin);

    /* I'm going to, it's going to */
    if (fin[0] && !q && !neg && V->naux && V->aux[0] == A_GOING && V->subj && subj_ch < 0 && !will && !would &&
        !past && !V->imper) {
        char b[24];
        snprintf(b, sizeof b, "%s'%s", V->subj, a == 1 ? "m" : a == 3 ? "s" : "re");
        fin_ch = emit(b);
    } else
    /* question: the finite word before the subject */
    if (fin[0]) {
        fin_ch = emit(fin);
        if (q && !V->imper) {
            if (subj_ch >= 0) move_chunk(fin_ch, subj_ch), fin_ch = subj_ch;
            else if (V->subj) emit(V->subj);
        } else if (V->subj && !V->imper) {
            int s = emit(V->subj);
            move_chunk(s, fin_ch);
            fin_ch++;
        }
    } else if (V->subj && !V->imper) {
        emit(V->subj);
    }
    if (V->there) {
        /* there is / is there */
        int th = emit("there");
        if (!q) move_chunk(th, fin_ch);
        if (will || would) emit("be");
        return fin_ch;
    }
    if (adverb) emit(adverb);

    /* the rest: aux steps then the main verb */
    if (V->naux && !start_step) {
        int s;
        for (s = 0; s < V->naux; s++) {
            int step = V->aux[s];
            const char *g = 0;
            int form = s == 0 ? (fin[0] ? F_BASE : -1) : nonfin_form;
            /* the step's own verb, if the finite word did not say it */
            switch (step) {
            case A_PERF: g = "have"; break;
            case A_PROG: g = "be"; break;
            case A_GOING: g = "be"; break;
            case A_HAVETO: g = "have"; break;
            case A_CAN: g = s ? "be able" : 0; break;
            case A_MUST: g = s ? "have" : 0; break;
            case A_TO: case A_START: g = V->vglue[s]; break;
            case A_STOP: g = "stop"; break;
            case A_KEEP: g = "keep"; break;
            case A_LETS: g = 0; break;
            }
            if (s == 0) {
                /* the finite word already said the step's verb unless it was do/will */
                if (form == -1) {
                    /* no auxiliary: inflect the step verb itself */
                    int f = past ? F_PAST : a == 3 ? F_S : F_BASE;
                    if (g) { en_form(g, f, word, sizeof word); emit(word); }
                } else if (!(first_is_aux == 1 && (step == A_PERF || step == A_PROG || step == A_GOING)) &&
                           !(first_is_aux == 2) && first_is_aux != 3) {
                    if (g) { en_form(g, F_BASE, word, sizeof word); emit(word); }
                } else if ((will || would) && g && (step == A_PERF || step == A_PROG || step == A_GOING)) {
                    emit(g);
                }
            } else if (g) {
                en_form(g, nonfin_form, word, sizeof word);
                emit(word);
            }
            /* what the step needs next */
            switch (step) {
            case A_PERF: nonfin_form = F_PP; break;
            case A_PROG: nonfin_form = F_ING; break;
            case A_GOING: emit("going to"); nonfin_form = F_BASE; break;
            case A_HAVETO: emit("to"); nonfin_form = F_BASE; break;
            case A_CAN: if (s) emit("to"); nonfin_form = F_BASE; break;
            case A_MUST: if (s) emit("to"); nonfin_form = F_BASE; break;
            case A_TO: case A_START: emit("to"); nonfin_form = F_BASE; break;
            case A_STOP: case A_KEEP: nonfin_form = F_ING; break;
            case A_LETS: nonfin_form = F_BASE; break;
            }
        }
        emit_main(V->gloss, nonfin_form, obj1, obj2, 0);
    } else {
        int form;
        if (V->imper || fin[0]) form = F_BASE;
        else form = past ? F_PAST : a == 3 ? F_S : F_BASE;
        if (V->naux && V->aux[0] == A_JUST) {
            if (!fin[0]) { emit("just"); form = F_PAST; } else { emit("just"); form = F_BASE; }
        }
        if (V->naux && V->aux[0] == A_USUALLY) emit("usually");
        if (streq(V->gloss, "be") && (fin[0] || V->imper)) {
            if (!fin[0] || will || would || first_is_aux == 2 || V->imper) emit("be");
        } else if (streq(V->gloss, "be")) {
            /* finite "be" was written as fin (am/is/are) */
        } else {
            emit_main(V->gloss, form, obj1, obj2, 0);
        }
        if (V->naux && V->aux[0] == A_AGAIN) st.suffix = "again";
    }
    if (V->compl) emit(V->compl);
    return fin_ch;
}

/* ------------------------------------------------------------ noun phrase */

static int np(int i, int end);

static const char *noun_en(int i, int pl, char *buf, int cap)
{
    const char *g = lx_gloss(it[i].nm.n);
    if (pl && !(LX_FLAGS(it[i].nm.n) & XLEX_UNCOUNT)) { en_plural(g, buf, cap); return buf; }
    return g;
}

static void emit_adj(int i)
{
    char b[64];
    const char *g;
    if (it[i].nm.a >= 0) g = lx_gloss(it[i].nm.a);
    else if (it[i].vb.v >= 0) { en_form(lx_gloss(it[i].vb.v), F_PP, b, sizeof b); g = b; }
    else if (it[i].ph >= 0) g = ph_en(it[i].ph);
    else return;
    if (it[i].nm.fl & LF_SUP) emit("very");
    if (it[i].nm.fl & LF_RE) emit("really");
    if (it[i].nm.fl & LF_MENTE) { char a[64]; en_adverb(g, a, sizeof a); emit(a); return; }
    if (g != b) { snprintf(b, sizeof b, "%s", g); }
    emit(b);
}

/* A noun phrase from item i: [det] [number] [adj] noun [adj] [de Name].
   Returns the item after it. */
static int is_inf(int i);

static int key_in(int i, const char *const *list, int n)
{
    int k;
    const char *key;
    if (i < 0 || i >= nit || it[i].nm.n < 0) return 0;
    key = lx_key(it[i].nm.n);
    for (k = 0; k < n; k++) if (streq(key, list[k])) return 1;
    return 0;
}
#define KEY_IN(i, list) key_in(i, list, (int)(sizeof list / sizeof list[0]))

static const char *const QUANTITY[] = { "par", "kilo", "litro", "mont\xf3n", "monton", "grupo", "lista", "vaso", "taza",
    "mitad", "resto", "parte", "mayoria", "cantidad", "botella", "caja", "pedazo", "docena", "metro", "kilometro",
    "cuadra", "minuto", "hora", "segundo", "poco", "millon", "ronda", "serie", "montaje" };
static const char *const PERSON[] = { "vecino", "vecina", "amigo", "amiga", "hermano", "hermana", "padre", "madre",
    "hijo", "hija", "chico", "chica", "pibe", "piba", "novio", "novia", "jefe", "primo", "prima", "tio", "tia", "abuelo",
    "abuela", "se\xf1or", "se\xf1ora", "profesor", "doctor", "medico", "mama", "papa", "viejo", "vieja", "nene", "nena",
    "hombre", "mujer", "compa\xf1" "ero", "compa\xf1" "era", "due\xf1o", "due\xf1" "a", "esposo", "esposa", "boludo", "chabon" };
static const char *const WEEKDAY[] = { "lunes", "martes", "miercoles", "jueves", "viernes", "sabado", "domingo" };
static const char *const DISTANCE[] = { "cuadra", "metro", "kilometro", "minuto", "hora", "paso", "segundo" };
static int np_depth;            /* a noun phrase inside a noun phrase (el nodo de la plaza) */
static int dist_mode;           /* "a tres cuadras": the "de" after it is "from" */

static int np(int i, int end)
{
    int j = i, det = -1, num = -1, pre[2], npre = 0, head = -1, post[3], npost = 0, ints[3] = { -1, -1, -1 };
    int pl = 0, k, start_ch = nch, name0 = -1, name1 = -1, poss = -1, comp = -1, owner = -1, of_pp = 0, today = 0;
    int weekday_on = 0, owner_d = -1;
    char buf[64];
    if (j < end && it[j].ph >= 0 && PH_CLASS(it[j].ph) == XC_DET &&
        (it[j].kind == IT_PHRASE || (it[j].kind == IT_WORD && decide(j) == R_FUNC))) {
        det = j++;
        if (PH_HAS(it[det].ph, XF_P)) pl = 1;
    }
    /* el mejor / la peor + noun: the best, the worst */
    if (j + 1 < end && (item_is(j, "mejor") || item_is(j, "peor") || item_is(j, "mejores") || item_is(j, "peores")) &&
        has_noun(j + 1)) {
        int def = det >= 0 && PH_HAS(it[det].ph, XF_DEF), good = tk[it[j].t0].key[0] == 'm';
        if (det >= 0) emit(ph_en(it[det].ph));
        emit(def ? (good ? "best" : "worst") : (good ? "better" : "worse"));
        it[j].use = R_FUNC;
        det = -1;
        j++;
    }
    if (j < end && (it[j].kind == IT_NUM || (it[j].kind == IT_WORD && it[j].ph >= 0 && PH_CLASS(it[j].ph) == XC_NUM &&
                                             !item_is(j, "medio") && !item_is(j, "media")))) {
        num = j++;
        if (it[num].kind == IT_NUM ? !(tk[it[num].t0].len == 1 && tk[it[num].t0].key[0] == '1') : !item_is(num, "uno"))
            pl = 1;
    }
    while (j < end && npre < 2 && it[j].kind == IT_WORD && it[j].ph < 0 && it[j].nm.a >= 0 && has_noun(j + 1) &&
           !(it[j].nm.n >= 0 && !has_noun(j + 1)) && !is_name_tok(it[j].t0) && !is_name_tok(it[j + 1].t0)) {
        it[j].use = R_A;
        pre[npre++] = j++;
    }
    if (j < end && it[j].kind == IT_WORD && (it[j].nm.n >= 0) && (it[j].ph < 0 || det >= 0 || decide(j) == R_N) &&
        !is_name_tok(it[j].t0)) {
        head = j++;
        it[head].use = R_N;
        if (it[head].nm.fl & LF_PL) pl = 1;
        else if (det < 0 && num < 0) pl = 0;
    } else if (det >= 0 && j < end && it[j].kind == IT_WORD && is_name_tok(it[j].t0)) {
        /* mi Heltec V3, un T-Beam: a name as the noun */
        name0 = j;
        while (j < end && ((it[j].kind == IT_WORD && is_name_tok(it[j].t0)) ||
                           ((it[j].kind == IT_PUNCT || it[j].kind == IT_NUM) && j > name0 &&
                            !(tk[it[j].t0].fl & TF_SPACE) && it[j].kind != IT_PUNCT) ||
                           (it[j].kind == IT_PUNCT && tk[it[j].t0].key[0] == '-' && !(tk[it[j].t0].fl & TF_SPACE) &&
                            j + 1 < end && !(tk[it[j + 1].t0].fl & TF_SPACE))))
            j++;
        name1 = j;
    } else if (det >= 0 && j < end && it[j].kind == IT_WORD && it[j].ph < 0 && it[j].vb.v < 0 && !nom_any(&it[j].nm)) {
        /* el handy: a word the lexicon lacks, kept as the noun */
        name0 = j;
        name1 = ++j;
    } else if (det >= 0 && j < end && has_adj(j) && it[j].ph < 0) {
        /* el rojo: the red one */
        it[j].use = R_A;
        post[npost++] = j++;
    }
    while (j < end && npost < 3 && it[j].kind == IT_WORD) {
        int c = it[j].ph >= 0 ? PH_CLASS(it[j].ph) : 0;
        if (c == XC_ADV && PH_HAS(it[j].ph, XF_INT) && j + 1 < end && has_adj(j + 1) && it[j + 1].ph < 0) {
            ints[npost] = j;
            it[j].use = R_FUNC;
            j++;
        }
        if (it[j].ph < 0 && has_adj(j) && head >= 0 && !(has_verb(j) && is_finite_t(it[j].vb.t) && it[j].nm.a < 0)) {
            it[j].use = R_A;
            post[npost++] = j++;
        } else break;
    }
    /* el perro de mi vecino -> my neighbour's dog */
    if (head >= 0 && det >= 0 && PH_HAS(it[det].ph, XF_DEF) && npost == 0 && j + 2 < end + 1 && item_is(j, "de") &&
        j + 2 < nit && it[j + 1].ph >= 0 && PH_CLASS(it[j + 1].ph) == XC_DET && it[j + 1].kind == IT_WORD &&
        (streq(ph_en(it[j + 1].ph), "my") || streq(ph_en(it[j + 1].ph), "your") || streq(ph_en(it[j + 1].ph), "their") ||
         streq(ph_en(it[j + 1].ph), "our")) &&
        has_noun(j + 2) && it[j + 2].ph < 0 && !(j + 3 < nit && (has_adj(j + 3) || item_is(j + 3, "de")) && it[j + 3].ph < 0)) {
        char b2[64], b3[72];
        int pl2 = (it[j + 2].nm.fl & LF_PL) != 0;
        emit(ph_en(it[j + 1].ph));
        snprintf(b3, sizeof b3, "%s%s", noun_en(j + 2, pl2, b2, sizeof b2), pl2 ? "'" : "'s");
        emit(b3);
        it[j + 2].use = R_N;
        poss = j + 3;
        det = -1;
    }
    /* N de N: the hill repeater, the neighbour's house, today's help; a pair of chargers */
    if (head >= 0 && poss < 0 && np_depth < 2 && j + 1 < end && (item_is(j, "de") || item_is(j, "del"))) {
        int k2 = j + 1, d2 = -1;
        if (item_is(j, "de") && (item_is(k2, "hoy") || item_is(k2, "ayer") || item_is(k2, "anoche"))) {
            today = k2;
        } else {
            if (item_is(j, "de") && it[k2].kind == IT_WORD && PH_CLASS(it[k2].ph) == XC_DET && PH_HAS(it[k2].ph, XF_DEF) &&
                !item_is(k2, "lo")) { d2 = k2; k2++; }
            if (k2 < end && has_noun(k2) && it[k2].ph < 0 && !is_name_tok(it[k2].t0) && !is_inf(k2) &&
                !KEY_IN(head, QUANTITY) && !(k2 + 1 < end && it[k2 + 1].ph < 0 && has_adj(k2 + 1) && !has_noun(k2 + 1)) &&
                !item_is(k2 + 1, "de") && !item_is(k2 + 1, "del") && !(dist_mode && KEY_IN(head, DISTANCE))) {
                if (KEY_IN(k2, PERSON) && (d2 >= 0 || item_is(j, "del"))) owner = k2, owner_d = d2;
                else comp = k2;
            } else if (np_depth == 0 && k2 < end && !is_inf(k2) && !is_name_tok(it[k2].t0) &&
                       !(dist_mode && KEY_IN(head, DISTANCE)) &&
                       (has_noun(k2) || (d2 >= 0 && (has_noun(k2) || has_adj(k2))) ||
                        (item_func_class(k2) == XC_DET && !PH_HAS(it[k2].ph, XF_CLIT) && has_noun(k2 + 1)))) {
                of_pp = 1;
            }
        }
    }
    if (head >= 0 && det >= 0 && PH_HAS(it[det].ph, XF_DEF) && KEY_IN(head, WEEKDAY) && comp < 0) {
        weekday_on = !(i > 0 && (item_func_class(i - 1) == XC_PREP || it[i - 1].kind == IT_PHRASE));
        det = -1;
    }
    /* el mío: mine */
    if (det >= 0 && head < 0 && npost == 1 && it[post[0]].nm.a >= 0) {
        const char *g = lx_gloss(it[post[0]].nm.a);
        if (streq(g, "mine") || streq(g, "yours") || streq(g, "theirs") || streq(g, "ours")) {
            emit(g);
            st.np_ch = start_ch; st.np_end = j; st.np_pl = pl; st.np_clause = (i == st.cstart) || st.qword;
            return j;
        }
    }
    if (weekday_on) emit("on");
    if (owner >= 0) {                   /* the neighbour's */
        char b2[64], b3[72];
        int pl2 = (it[owner].nm.fl & LF_PL) != 0;
        emit("the");
        snprintf(b3, sizeof b3, "%s%s", noun_en(owner, pl2, b2, sizeof b2), pl2 ? "'" : "'s");
        emit(b3);
        it[owner].use = R_N;
        if (owner_d >= 0) it[owner_d].use = R_FUNC;
        det = -1;
    }
    if (today) {
        emit(item_is(today, "hoy") ? "today's" : item_is(today, "ayer") ? "yesterday's" : "last night's");
        det = -1;
    }
    /* determiner */
    if (det >= 0) {
        const char *d = (head >= 0 || npost || npre || name0 >= 0) ? ph_en(it[det].ph) : ph_alt(it[det].ph);
        int skip = 0;
        if ((streq(d, "their")) && (sent_q == 1 || usted_seen)) d = "your";   /* ¿Cuál es su nombre? */
        /* las 3: the hour without an article */
        if (num >= 0 && (item_is(det, "la") || item_is(det, "las"))) skip = 1;
        if (!skip) {
            if (streq(d, "a")) {
                char nb[8];
                const char *nx = npre ? (it[pre[0]].nm.a >= 0 ? lx_gloss(it[pre[0]].nm.a) : "x")
                               : npost ? (it[post[0]].nm.a >= 0 ? lx_gloss(it[post[0]].nm.a) : "x")
                               : comp >= 0 ? lx_gloss(it[comp].nm.n)
                               : head >= 0 ? lx_gloss(it[head].nm.n) : "x";
                if (name0 >= 0) { snprintf(nb, sizeof nb, "%c", lowc(src[tk[it[name0].t0].at])); nx = nb; }
                if (ints[0] >= 0) nx = ph_en(it[ints[0]].ph);
                if (head >= 0 && (LX_FLAGS(it[head].nm.n) & XLEX_UNCOUNT) && !npre && !npost) d = "some";
                else if (is_vowel_start(nx)) d = "an";
            }
            if (st.negcl && PH_HAS(it[det].ph, XF_NEG)) d = "any";
            emit(d);
        }
    }
    if (num >= 0) {
        if (it[num].kind == IT_NUM) emit_item_src(num, 0);
        else emit(ph_en(it[num].ph));
    }
    for (k = 0; k < npre; k++) emit_adj(pre[k]);
    for (k = 0; k < npost; k++) {
        const char *g = it[post[k]].nm.a >= 0 ? lx_gloss(it[post[k]].nm.a) : "";
        if (streq(g, "on") || streq(g, "off") || streq(g, "alone")) continue;     /* after the noun */
        if (ints[k] >= 0) emit(ph_en(it[ints[k]].ph));
        emit_adj(post[k]);
    }
    if (comp >= 0) {
        emit(noun_en(comp, 0, buf, sizeof buf));
        it[comp].use = R_N;
    }
    if (head >= 0) {
        emit(noun_en(head, pl, buf, sizeof buf));
        for (k = 0; k < npost; k++) {
            const char *g = it[post[k]].nm.a >= 0 ? lx_gloss(it[post[k]].nm.a) : "";
            if (streq(g, "on") || streq(g, "off") || streq(g, "alone")) emit(g);
        }
    } else if (name0 >= 0) {
        int a0 = tk[it[name0].t0].at, l = tk[it[name1 - 1].t0].at + tk[it[name1 - 1].t0].len - a0;
        emit_n(src + a0, l, CF_PASS);
    } else if (det >= 0 && npost && !npre) {
        emit("one");
    }
    if (poss >= 0) j = poss;
    if (comp >= 0 || owner >= 0) j = (comp >= 0 ? comp : owner) + 1;
    if (today) j = today + 1;
    if (of_pp) {                        /* el nodo de la plaza nueva: of the new square */
        int e;
        emit(item_is(j, "del") ? "of the" : "of");
        it[j].use = R_FUNC;
        np_depth++;
        e = np(j + 1, end);
        np_depth--;
        j = e;
    }
    if (dist_mode && head >= 0 && KEY_IN(head, DISTANCE)) {
        if (j < end && (item_is(j, "de") || item_is(j, "del"))) {
            emit(item_is(j, "del") ? "from the" : "from");      /* a tres cuadras de la estación */
            j++;
        } else {
            emit("away");                                       /* vivo a dos cuadras */
        }
        dist_mode = 0;
    }
    /* la casa de Juan -> Juan's house */
    if (head >= 0 && j + 1 < end && item_is(j, "de") && is_name_tok(it[j + 1].t0) &&
        (j + 2 >= end || it[j + 2].kind != IT_WORD || !is_name_tok(it[j + 2].t0))) {
        char nm[64];
        const xl_tok *t = &tk[it[j + 1].t0];
        int n = t->len < 60 ? t->len : 60;
        int c;
        snprintf(nm, sizeof nm, "%.*s's", n, src + t->at);
        c = emit_n(nm, (int)strlen(nm), CF_PASS);
        /* drop "the" and put the name first */
        if (det >= 0 && (streq(ph_en(it[det].ph), "the")) && ch[start_ch].len == 3 && !memcmp(arena + ch[start_ch].off, "the", 3)) {
            move_chunk(c, start_ch);
            memmove(&ch[start_ch + 1], &ch[start_ch + 2], sizeof(xl_chunk) * (size_t)(nch - start_ch - 2));
            nch--;
        } else {
            move_chunk(c, start_ch);
        }
        j += 2;
    }
    st.np_ch = start_ch;
    st.np_end = j;
    st.np_pl = pl;
    st.np_clause = (i == st.cstart) || st.qword;
    if (j == i) j = i + 1;      /* nothing taken: never stall */
    return j;
}

/* ------------------------------------------------------------- clause flow */

static void clause_reset(int i)
{
    if (st.suffix) { emit(st.suffix); st.suffix = 0; }
    st.cstart = i;
    rel_subj = 0;
    st.subj_ch = -1;
    st.subj_p = 0;
    st.neg = 0;
    st.negcl = 0;
    st.ncl = 0;
    st.adv = -1;
    st.qword = 0;
    st.hadverb = 0;
    st.np_end = -1;
    st.more = 0;
}

static int is_time_noun(int i)
{
    static const char *const tn[] = { "minuto", "hora", "dia", "semana", "mes", "ano", "segundo", "rato", "tiempo",
                                      "momento", "noche" };
    unsigned k;
    if (i >= nit || it[i].nm.n < 0) return 0;
    for (k = 0; k < sizeof tn / sizeof tn[0]; k++) {
        char b[12];
        snprintf(b, sizeof b, "%s", tn[k]);
        if (!strcmp(b, "ano")) { b[1] = (char)0xF1; }
        if (streq(lx_key(it[i].nm.n), b)) return 1;
    }
    return 0;
}

static int tener_noun(int i, const char **adj)
{
    int e;
    if (i >= nit || it[i].kind != IT_WORD) return 0;
    e = find(XL_TENER, NTENER, tk[it[i].t0].key);
    if (e < 0) return 0;
    *adj = NEXT(P(XL_TENER[e]));
    return 1;
}
static int hacer_noun(int i, const char **adj)
{
    int e;
    if (i >= nit || it[i].kind != IT_WORD) return 0;
    e = find(XL_HACER, NHACER, tk[it[i].t0].key);
    if (e < 0) return 0;
    *adj = NEXT(P(XL_HACER[e]));
    return 1;
}

static int is_inf(int i)
{
    return i < nit && it[i].kind == IT_WORD && it[i].vb.v >= 0 && it[i].vb.t == XT_INF;
}

static int gustar_type(int v, int io)
{
    if (lemma_is(v, "gustar") || lemma_is(v, "encantar") || lemma_is(v, "importar") || lemma_is(v, "copar") ||
        lemma_is(v, "faltar")) return 1;
    if (lemma_is(v, "quedar") && io) return 2;
    return 0;
}

static const char *pick_adverb(int ph)
{
    return ph == -2 ? "also" : ph >= 0 ? ph_en(ph) : 0;
}

/* A verb at item i and what hangs on it. Returns the next item. */
static int vp(int i, int end)
{
    xl_vp V;
    int j = i + 1, v = it[i].vb.v, k, subj_ch = -1, io = 0, refl = 0, q = sent_q == 1 && !st.hadverb && !q_done, p7 = 0;
    int accidental = 0, was_imper = 0;
    const char *adverb = pick_adverb(st.adv);
    memset(&V, 0, sizeof V);
    V.tense = it[i].vb.t;
    V.person = it[i].vb.p;
    V.main_v = v;
    V.gloss = vp_gloss(v);
    V.imper = it[i].vb.imp || V.tense == XT_IMP;
    it[i].use = R_VERB;

    /* fui / fue + a, al, para, hasta: ir, not ser */
    if (lemma_is(v, "ser") && (V.tense == XT_PRET || V.tense == XT_SUBJI) &&
        (item_is(j, "a") || item_is(j, "al") || item_is(j, "para") || item_is(j, "hasta") || item_is(j, "pa") ||
         item_is(j, "aca") || item_is(j, "alla") || item_is(j, "ahi") || item_is(j, "a\xf1i"))) {
        int irv = verb_inf("ir");
        if (irv >= 0) { v = irv; V.main_v = v; V.gloss = vp_gloss(v); }
    }
    /* proclitics, then enclitics */
    for (k = 0; k < st.ncl && V.ncl < 4; k++) V.cl[V.ncl++] = st.cl[k];
    for (k = 0; k < it[i].vb.ncl && V.ncl < 4; k++) V.cl[V.ncl++] = it[i].vb.cl[k];
    st.ncl = 0;

    /* non-finite on its own */
    if (V.tense == XT_INF || V.tense == XT_GER || V.tense == XT_PART) {
        const char *obj = V.ncl ? object_word(V.cl[0], v) : 0;
        int pc = item_func_class(i - 1);
        int form = V.tense == XT_GER ? F_ING : V.tense == XT_PART ? F_PP : F_BASE;
        if (V.tense == XT_INF) {
            /* after without/before/after/of/by: -ing */
            if (pc == XC_PREP && !item_is(i - 1, "a") && !item_is(i - 1, "para") && !item_is(i - 1, "de") &&
                !item_is(i - 1, "pa")) form = F_ING;
            else if (!(pc == XC_PREP) && !no_to && i > st.cstart && !said_to()) emit("to");
            no_to = 0;
            /* reflexive infinitive (irme) */
            if (V.ncl && clitic_person(V.cl[0])) {
                char inf[XL_KEY];
                int r;
                snprintf(inf, sizeof inf, "%sse", lx_key(v));
                r = verb_inf(inf);
                if (r >= 0) { V.gloss = lx_gloss(r); obj = 0; }
                else if (clitic_person(V.cl[0]) == 3) obj = 0;
            }
        }
        if (V.tense == XT_GER && V.ncl && clitic_person(V.cl[0]) == 3) obj = 0;
        if (V.tense == XT_PART && it[i].nm.a >= 0) { emit(lx_gloss(it[i].nm.a)); return j; }
        emit_main(V.gloss, form, obj, V.ncl > 1 ? object_word(V.cl[1], v) : 0, 0);
        return j;
    }
    if (q) q_done = 1;              /* later verbs of the question (si hay, que vienen) stay in order */
    was_imper = last_imper;
    last_imper = 0;

    /* doblá a la derecha y seguí: a command joined to a command */
    if (was_imper && V.tense == XT_PRET && V.person == 1 && (tk[it[i].t0].fl & TF_ACCL) && i > 0 &&
        (item_is(i - 1, "y") || item_is(i - 1, "e")) && tk[it[i].t0].key[strlen(tk[it[i].t0].key) - 1] == 'i') {
        V.imper = 1; V.tense = XT_IMP; V.person = 2;
    }
    /* eviten la zona, vengan: a plural command to the group */
    if (V.tense == XT_SUBJ && V.person == 6 && i == st.cstart && st.subj_ch < 0 && sent_q != 1 &&
        (i == 0 || item_punct(i - 1) || item_func_class(i - 1) == XC_INTJ || item_func_class(i - 1) == XC_NEG))
        V.imper = 1;

    /* the subject */
    if (st.subj_ch >= 0) {
        subj_ch = st.subj_ch;
        V.en_person = st.subj_p;
        st.subj_ch = -1;            /* said: the next verb has its own */
    } else if (st.np_end >= 0 && st.np_clause && st.np_ch < nch) {
        int gap = 1;
        for (k = st.np_end; k < i; k++)
            if (!(it[k].use == R_FUNC && (PH_CLASS(it[k].ph) == XC_CLIT || PH_CLASS(it[k].ph) == XC_NEG ||
                                          PH_CLASS(it[k].ph) == XC_ADV || PH_CLASS(it[k].ph) == XC_DET)) &&
                !(it[k].use == R_PASS && is_name_tok(it[k].t0))) gap = 0;
        if (gap && (V.person == 3 || V.person == 6 || V.person == 7)) { subj_ch = st.np_ch; V.en_person = st.np_pl ? 6 : 3; }
    }
    p7 = V.person == 7;
    if (V.person == 7)                  /* dijo que venía; había gente */
        V.person = (last_person == 3 || last_person == 6 || rel_subj || (lemma_is(v, "haber") && !(j < end && has_verb(j) &&
                                                                                     it[j].vb.t == XT_PART))) ? 3 : 1;
    if (subj_ch < 0) {
        V.en_person = V.person;
        V.subj = subject_word(V.person, v);
    }

    /* se me + verb (se me cortó, se me terminó): the thing did it, on me */
    if (V.ncl >= 2 && clitic_person(V.cl[0]) == 3 && streq(ph_key(V.cl[0]), "se") &&
        (V.person == 3 || V.person == 6) && !clitic_direct(V.cl[1])) {
        static const char *const on[] = { "", "on me", "on you", "on him", "on us", "on you", "on them" };
        const char *key = ph_key(V.cl[1]);
        int cp = streq(key, "le") ? 3 : streq(key, "les") ? 6 : clitic_person(V.cl[1]);
        if (cp >= 1 && cp <= 6) {
            char inf[XL_KEY];
            int r;
            st.suffix = on[cp];
            V.ncl = 0;
            accidental = 1;
            snprintf(inf, sizeof inf, "%sse", lx_key(v));
            r = verb_inf(inf);
            if (r >= 0) V.gloss = lx_gloss(r), V.main_v = r;     /* se me cortó: cut out */
        }
    }
    /* reflexive and impersonal se */
    for (k = 0; k < V.ncl; k++) {
        int cp = clitic_person(V.cl[k]);
        if (!cp) continue;
        if (cp == V.person || (cp == 3 && (V.person == 6 || V.person == 3))) {
            char inf[XL_KEY];
            int r;
            snprintf(inf, sizeof inf, "%sse", lx_key(v));
            r = verb_inf(inf);
            if (r >= 0) V.gloss = lx_gloss(r), V.main_v = r;
            else if (cp == 3 && !(LX_FLAGS(v) & XLEX_SUBJ_AFTER) && subj_ch < 0 && V.person == 3 &&
                     !lemma_is(v, "ir") && !(k + 1 < V.ncl && clitic_direct(V.cl[k + 1]))) {
                V.subj = "you"; V.en_person = 2;          /* se puede: you can */
            }
            if (cp == 3 && k + 1 < V.ncl && clitic_direct(V.cl[k + 1])) {
                io = 3;                                   /* se lo dije: to him */
                continue;
            }
            memmove(&V.cl[k], &V.cl[k + 1], sizeof(int) * (size_t)(V.ncl - k - 1));
            V.ncl--;
            k--;
            refl = 1;
        } else if (cp != 3 || (k + 1 < V.ncl && clitic_direct(V.cl[k + 1]))) io = cp;
    }
    for (k = 0; k < V.ncl; k++) {
        const char *key = ph_key(V.cl[k]);
        if (streq(key, "le") || streq(key, "les")) io = streq(key, "le") ? 3 : 6;
        if (!io && (streq(key, "me") || streq(key, "te") || streq(key, "nos"))) io = clitic_person(V.cl[k]);
    }
    (void)refl;
    if (V.subj && subj_ch < 0 && V.person == 3 && !streq(V.subj, "you")) V.subj = subject_word(3, V.main_v);
    /* ¿vienen?: a question in the third plural is to the group; ¿tiene su radio?: to you */
    if (V.subj && subj_ch < 0 && sent_q == 1 && !V.imper &&
        (V.person == 6 || (V.person == 3 && usted_seen && !streq(V.subj, "it")))) {
        V.subj = "you";
        V.en_person = 2;
        if (V.person == 6 && (item_is(j, "todos") || item_is(j, "todas"))) { V.subj = "you all"; it[j].use = R_SKIP; }
    }
    if (p7 && V.subj && subj_ch < 0 && streq(subject_word(3, V.main_v), "it") && !lemma_is(V.main_v, "ser") &&
        !lemma_is(V.main_v, "estar") && !lemma_is(V.main_v, "haber"))
        V.person = 3, V.en_person = 3, V.subj = "it";

    if (rel_subj) {
        if (V.subj && subj_ch < 0 && (V.person == 3 || V.person == 6)) V.subj = 0;
        rel_subj = 0;
    }
    /* la cargo y vuelvo: the same subject again after "y" */
    if (V.subj && subj_ch < 0 && !q && last_person == V.person && i > 0) {
        int b = i - 1;
        while (b > 0 && it[b].use == R_FUNC && (PH_CLASS(it[b].ph) == XC_CLIT || PH_CLASS(it[b].ph) == XC_NEG ||
                                                (PH_CLASS(it[b].ph) == XC_DET && PH_HAS(it[b].ph, XF_CLIT))))
            b--;
        if (item_is(b, "y") || item_is(b, "e")) V.subj = 0;
    }
    /* gustar: me gusta X -> I like X */
    if (gustar_type(v, io) && io) {
        static const char *const subj[] = { "", "I", "you", "he", "we", "you", "they" };
        int g = gustar_type(v, io);
        V.gustar = 1;
        V.en_person = io;
        V.subj = subj[io];
        subj_ch = -1;
        V.ncl = 0;
        if (g == 2) { V.gloss = "have"; st.suffix = "left"; }
        for (k = j; k < end && k < j + 8 && !item_punct(k); k++) {
            if ((item_is(k, "a") || item_is(k, "al")) && k + 1 < end &&
                (has_noun(k + 1) || item_func_class(k + 1) == XC_DET || is_name_tok(it[k + 1].t0))) {
                int s0 = nch, e, x;
                if (item_is(k, "al")) emit("the");
                if (is_name_tok(it[k + 1].t0) && !has_noun(k + 1)) { emit_item_src(k + 1, 0); e = k + 2; }
                else e = np(k + 1, end);
                if (nch > s0) {
                    subj_ch = s0;
                    V.subj = 0;
                    V.en_person = st.np_pl ? 6 : 3;
                    for (x = k; x < e; x++) it[x].use = R_SKIP;
                }
                break;
            }
        }
        /* falta batería: low on battery */
        if (lemma_is(v, "faltar") && j < end && it[j].nm.n >= 0) {
            static const char *const low[] = { "bateria", "carga", "se\xf1" "al", "nafta", "pila", "saldo",
                                               "credito" };
            if (KEY_IN(j, low)) { V.gloss = "be"; V.compl = "low on"; }
        }
        if (!(j < end && it[j].kind == IT_WORD &&
              ((it[j].ph < 0 && it[j].nm.n >= 0) || is_inf(j) || it[j].kind == IT_NUM ||
               (it[j].ph >= 0 && (PH_CLASS(it[j].ph) == XC_DET || PH_CLASS(it[j].ph) == XC_NUM) &&
                (has_noun(j + 1) || has_adj(j + 1)))))) {
            V.compl = strchr(V.gloss, '+') ? 0 : (V.person == 6 ? "them" : "it");
            if (lemma_is(v, "importar")) V.compl = 0;
        }
    }

    /* querer + person, no verb after: love (te quiero); decir + person: tell; llegar + person: reach */
    if (lemma_is(v, "querer") && V.ncl && !is_inf(j) && (item_punct(j) || item_func_class(j) == XC_ADV)) V.gloss = "love";
    if (lemma_is(v, "decir") && io) V.gloss = "tell";
    if (lemma_is(v, "llegar") && io) V.gloss = "reach";
    if (lemma_is(v, "hablar")) {
        static const char *const langs[] = { "espa\xf1ol", "ingles", "portugues", "castellano", "frances", "aleman",
                                             "italiano", "espa\xf1ol" };
        unsigned n2;
        for (n2 = 0; n2 < sizeof langs / sizeof langs[0]; n2++)
            if (item_is(j, langs[n2]) || item_is(j + 1, langs[n2])) V.gloss = "speak";
    }

    /* a clitic said again by "a" + noun (se lo dije a Juan): keep the noun */
    if (io) {
        for (k = j; k < end && k < j + 6 && !item_punct(k); k++)
            if (item_is(k, "a") && k + 1 < end && (is_name_tok(it[k + 1].t0) || item_func_class(k + 1) == XC_DET ||
                                                   item_func_class(k + 1) == XC_PRONX || has_noun(k + 1))) {
                int c;
                for (c = 0; c < V.ncl; c++) {
                    const char *key = ph_key(V.cl[c]);
                    if (streq(key, "le") || streq(key, "les") || streq(key, "se")) {
                        memmove(&V.cl[c], &V.cl[c + 1], sizeof(int) * (size_t)(V.ncl - c - 1));
                        V.ncl--;
                        keep_a = 1;
                        break;
                    }
                }
                break;
            }
    }

    /* chains */
    {
        int m = j, more = 1;
        while (more && V.naux < 3) {
            int cv = V.naux ? V.main_v : v;
            more = 0;
            if (lemma_is(cv, "haber") && m < end && has_verb(m) && it[m].vb.t == XT_PART && !item_is(i, "hay")) {
                V.aux[V.naux++] = A_PERF; V.main_v = it[m].vb.v; it[m].use = R_VERB;
                for (k = 0; k < it[m].vb.ncl && V.ncl < 4; k++) V.cl[V.ncl++] = it[m].vb.cl[k];
                m++; more = 1; continue;
            }
            if (lemma_is(cv, "estar") && m < end && has_verb(m) && it[m].vb.t == XT_GER) {
                V.aux[V.naux++] = A_PROG; V.main_v = it[m].vb.v; it[m].use = R_VERB;
                for (k = 0; k < it[m].vb.ncl && V.ncl < 4; k++) V.cl[V.ncl++] = it[m].vb.cl[k];
                m++; more = 1; continue;
            }
            if (lemma_is(cv, "seguir") && m < end && has_verb(m) && it[m].vb.t == XT_GER) {
                V.aux[V.naux++] = A_KEEP; V.main_v = it[m].vb.v; it[m].use = R_VERB;
                m++; more = 1; continue;
            }
            if ((lemma_is(cv, "ir") || lemma_is(cv, "volver") || lemma_is(cv, "empezar") || lemma_is(cv, "comenzar")) &&
                m + 1 < end && item_is(m, "a") && is_inf(m + 1)) {
                int step = lemma_is(cv, "ir") ? A_GOING : lemma_is(cv, "volver") ? A_AGAIN : A_START;
                if (step == A_GOING && V.imper == 0 && V.person == 4 && i == st.cstart && !q && !st.neg &&
                    V.tense == XT_PRES && subj_ch < 0) step = A_LETS;
                if (step == A_GOING && V.tense == XT_PRET) { step = A_TO; V.vglue[V.naux] = "go"; }
                if (step == A_START) V.vglue[V.naux] = vp_gloss(cv);
                V.aux[V.naux++] = step; V.main_v = it[m + 1].vb.v; it[m + 1].use = R_VERB;
                for (k = 0; k < it[m + 1].vb.ncl && V.ncl < 4; k++) V.cl[V.ncl++] = it[m + 1].vb.cl[k];
                m += 2; more = 1; continue;
            }
            if ((lemma_is(cv, "tener") || lemma_is(cv, "haber")) && m + 1 < end && item_is(m, "que") && is_inf(m + 1)) {
                V.aux[V.naux++] = A_HAVETO; V.main_v = it[m + 1].vb.v; it[m + 1].use = R_VERB;
                if (lemma_is(cv, "haber") && V.naux == 1) { V.subj = "we"; V.en_person = 4; subj_ch = -1; }
                for (k = 0; k < it[m + 1].vb.ncl && V.ncl < 4; k++) V.cl[V.ncl++] = it[m + 1].vb.cl[k];
                m += 2; more = 1; continue;
            }
            if ((lemma_is(cv, "acabar") || lemma_is(cv, "dejar") || lemma_is(cv, "tratar")) && m + 1 < end &&
                item_is(m, "de") && is_inf(m + 1)) {
                int step = lemma_is(cv, "acabar") ? A_JUST : lemma_is(cv, "dejar") ? A_STOP : A_TO;
                if (step == A_TO) V.vglue[V.naux] = "try";
                V.aux[V.naux++] = step; V.main_v = it[m + 1].vb.v; it[m + 1].use = R_VERB;
                for (k = 0; k < it[m + 1].vb.ncl && V.ncl < 4; k++) V.cl[V.ncl++] = it[m + 1].vb.cl[k];
                m += 2; more = 1; continue;
            }
            if (m < end && is_inf(m) && !lemma_is(cv, "ser") && !lemma_is(cv, "estar") && !lemma_is(cv, "haber")) {
                int step = lemma_is(cv, "poder") ? A_CAN : lemma_is(cv, "deber") ? A_MUST :
                           lemma_is(cv, "soler") ? A_USUALLY : A_TO;
                if (V.gustar) break;
                if (step == A_TO) V.vglue[V.naux] = vp_gloss(cv);
                V.aux[V.naux++] = step; V.main_v = it[m].vb.v; it[m].use = R_VERB;
                for (k = 0; k < it[m].vb.ncl && V.ncl < 4; k++) V.cl[V.ncl++] = it[m].vb.cl[k];
                m++; more = 1; continue;
            }
        }
        if (V.naux) {
            V.gloss = vp_gloss(V.main_v);
            /* reflexive infinitive at the end (tengo que irme) */
            for (k = 0; k < V.ncl; k++) {
                int cp = clitic_person(V.cl[k]);
                if (cp && (cp == V.person || cp == 3)) {
                    char inf[XL_KEY];
                    int r;
                    snprintf(inf, sizeof inf, "%sse", lx_key(V.main_v));
                    r = verb_inf(inf);
                    if (r >= 0) V.gloss = lx_gloss(r);
                    memmove(&V.cl[k], &V.cl[k + 1], sizeof(int) * (size_t)(V.ncl - k - 1));
                    V.ncl--;
                    k--;
                }
            }
            if (V.aux[0] == A_LETS) V.subj = 0, subj_ch = -1, V.imper = 0;
            if (V.aux[0] == A_AGAIN && V.tense == XT_PRES && !V.imper) {
                V.aux[0] = A_PROG;      /* vuelve a funcionar: is working again */
                st.suffix = "again";
            }
            if (p7 && V.subj && subj_ch < 0 && streq(subject_word(3, V.main_v), "it") && !streq(subject_word(3, v), "he"))
                V.person = 3, V.en_person = 3, V.subj = "it";
            if (V.subj && subj_ch < 0 && V.person == 3 && !streq(V.subj, "you") && V.aux[0] != A_HAVETO)
                V.subj = subject_word(3, V.main_v);
        }
        j = m;
    }

    /* ¿venís?, ¿vienen?: are you coming? */
    if (q && V.tense == XT_PRES && !V.naux && !V.imper &&
        (lemma_is(V.main_v, "venir") || lemma_is(V.main_v, "ir") || lemma_is(V.main_v, "llegar") ||
         lemma_is(V.main_v, "salir") || lemma_is(V.main_v, "volver") || lemma_is(V.main_v, "irse"))) {
        V.naux = 1;
        V.aux[0] = A_PROG;
    }
    /* bajá la potencia: lower; bajar el archivo: download */
    if ((lemma_is(V.main_v, "bajar") || lemma_is(V.main_v, "subir")) && j < end) {
        static const char *const level[] = { "potencia", "volumen", "precio", "velocidad", "ganancia", "brillo",
                                             "temperatura", "persiana" };
        static const char *const data[] = { "archivo", "foto", "video", "mapa", "firmware", "imagen", "mensaje",
                                            "aplicacion", "programa", "version" };
        int o = item_func_class(j) == XC_DET ? j + 1 : j;
        int down = lemma_is(V.main_v, "bajar");
        static const char *const climb[] = { "escalera", "cerro", "monta\xf1" "a", "cuesta", "calle", "ruta", "loma",
                                             "rampa", "colina" };
        if (KEY_IN(o, level)) V.gloss = down ? "lower" : "raise";
        else if (KEY_IN(o, data)) V.gloss = down ? "download" : "upload";
        else if (o > j - 1 && it[o].nm.n >= 0 && it[o].ph < 0 && item_func_class(j) == XC_DET && !KEY_IN(o, climb))
            V.gloss = down ? "take+down" : "take+up";           /* subimos el nodo al techo */
    }
    /* mi casa queda a cinco cuadras, queda cerca: is */
    if (lemma_is(V.main_v, "quedar") && !io && !V.naux &&
        (item_is(j, "a") || item_is(j, "en") || item_is(j, "cerca") || item_is(j, "lejos") || item_is(j, "por") ||
         item_is(j, "aca") || item_is(j, "ahi") || item_is(j, "alla") || item_is(j, "enfrente") || item_is(j, "arriba") ||
         item_is(j, "abajo") || item_func_class(j) == XC_PREP))
        V.gloss = "be";
    /* el nodo no anda: a thing that "anda" works */
    if (lemma_is(V.main_v, "andar") && subj_ch >= 0 && !V.naux && st.subj_ch < 0) V.gloss = "work";
    /* te llego bien, llego mucho mejor: the signal comes through */
    if (lemma_is(V.main_v, "llegar") && !V.naux &&
        (item_is(j, "bien") || item_is(j, "mal") || item_is(j, "mejor") || item_is(j, "peor") || item_is(j, "clarito") ||
         item_is(j, "fuerte") || item_is(j, "debil") || item_is(j, "joya") ||
         (it[j].kind == IT_PHRASE && !strncmp(ph_key(it[j].ph), "mucho ", 6))))
        V.gloss = "come through";

    /* haber on its own: there is / there are */
    if (lemma_is(v, "haber") && !V.naux && V.person == 3) {
        int nx = j;
        V.there = 1;
        V.subj = 0;
        subj_ch = -1;
        if (nx < end && it[nx].kind == IT_WORD) {
            if (it[nx].ph >= 0 && PH_CLASS(it[nx].ph) == XC_DET) V.there_pl = PH_HAS(it[nx].ph, XF_P) ? 1 : 0;
            else if (it[nx].nm.n >= 0) V.there_pl = (it[nx].nm.fl & LF_PL) != 0;
            if (it[nx].ph >= 0 && PH_CLASS(it[nx].ph) == XC_NUM && !item_is(nx, "uno")) V.there_pl = 1;
        }
        if (nx < end && it[nx].kind == IT_NUM) V.there_pl = 1;
        if (st.neg && nx < end && (it[nx].nm.n >= 0 || item_func_class(nx) == XC_DET)) {
            /* no hay señal: there is no signal */
            vp_write(&V, q, 0, 0, -1);
            emit(q ? "any" : "no");
            st.neg = 0;
            if (nx < end && it[nx].ph >= 0 && PH_CLASS(it[nx].ph) == XC_DET &&
                (item_is(nx, "un") || item_is(nx, "una") || item_is(nx, "ningun") || item_is(nx, "ninguna"))) {
                it[nx].use = R_FUNC;
                j = nx + 1;
            }
            st.hadverb = 1;
            st.lastverb = v;
            last_person = 3;
            return j;
        }
    }

    /* tener frío -> be cold (mucho frío -> very cold); tener N años -> be N years old */
    if (lemma_is(V.main_v, "tener") && !V.naux) {
        const char *adj, *how = 0;
        int jj = j;
        if (item_is(jj, "mucho") || item_is(jj, "mucha")) how = "very", jj++;
        else if (item_is(jj, "tanto") || item_is(jj, "tanta")) how = "so", jj++;
        else if (item_is(jj, "demasiado") || item_is(jj, "demasiada")) how = "too", jj++;
        else if (item_is(jj, "un") && item_is(jj + 1, "poco") && item_is(jj + 2, "de")) how = "a bit", jj += 3;
        if (tener_noun(jj, &adj)) {
            snprintf(compl_buf, sizeof compl_buf, "%s%s%s", how ? how : "", how ? " " : "", adj);
            V.gloss = "be"; V.compl = compl_buf; it[jj].use = R_FUNC; j = jj + 1;
        } else if (j + 1 < end && (it[j].kind == IT_NUM || item_func_class(j) == XC_NUM) && it[j + 1].kind == IT_WORD &&
                   it[j + 1].nm.n >= 0 && (streq(lx_key(it[j + 1].nm.n), "a\xf1o"))) {
            V.gloss = "be";
            vp_write(&V, q, st.neg, adverb, subj_ch);
            if (it[j].kind == IT_NUM) emit_item_src(j, 0); else emit(ph_en(it[j].ph));
            emit("years old");
            st.neg = 0; st.adv = -1; st.hadverb = 1; st.lastverb = v; last_person = V.person;
            return j + 2;
        }
    }
    /* hace frío -> it is cold; va a hacer frío; hace 2 horas -> 2 hours ago */
    if (lemma_is(V.main_v, "hacer") && V.person == 3 && subj_ch < 0) {
        const char *adj, *how = 0;
        int jj = j;
        if (item_is(jj, "mucho") || item_is(jj, "mucha")) how = "very", jj++;
        else if (item_is(jj, "tanto") || item_is(jj, "tanta")) how = "so", jj++;
        else if (item_is(jj, "bastante")) how = "quite", jj++;
        else if (item_is(jj, "un") && item_is(jj + 1, "poco") && item_is(jj + 2, "de")) how = "a bit", jj += 3;
        if (hacer_noun(jj, &adj)) {
            snprintf(compl_buf, sizeof compl_buf, "%s%s%s", how ? how : "", how ? " " : "", adj);
            V.gloss = "be"; V.subj = "it"; V.compl = compl_buf; it[jj].use = R_FUNC; j = jj + 1;
        } else if (!V.naux && j < end && (it[j].kind == IT_NUM || item_func_class(j) == XC_NUM ||
                                          item_func_class(j) == XC_DET) &&
                   (is_time_noun(j + 1) || is_time_noun(j))) {
            int e = np(j, end);
            emit("ago");
            st.adv = -1;
            return e;
        }
    }
    /* son las 5: it is 5 */
    if (lemma_is(v, "ser") && !V.naux && subj_ch < 0 && (item_is(j, "las") || item_is(j, "la")) && j + 1 < end &&
        (it[j + 1].kind == IT_NUM || item_func_class(j + 1) == XC_NUM)) {
        V.person = 3; V.en_person = 3; V.subj = "it";
        vp_write(&V, q, st.neg, adverb, -1);
        if (it[j + 1].kind == IT_NUM) emit_item_src(j + 1, 0); else emit(ph_en(it[j + 1].ph));
        st.neg = 0; st.adv = -1; st.hadverb = 1; st.lastverb = v;
        return j + 2;
    }
    if (lemma_is(v, "ser") && !V.naux && subj_ch < 0 && j < end && it[j].kind == IT_NUM) {
        V.person = 3; V.en_person = 3; V.subj = "it";
    }
    /* pasar with something/nothing/what: happen */
    if (lemma_is(V.main_v, "pasar") && (item_is(j, "algo") || item_is(j, "nada") || item_is(i - 1, "que") ||
                                         item_is(i - 1, "algo") || item_is(i - 1, "nada"))) V.gloss = "happen";
    /* deber + object: owe; poder alone: can */
    if (lemma_is(V.main_v, "deber") && !V.naux) V.gloss = "owe";
    if (lemma_is(V.main_v, "poder") && !V.naux) { V.naux = 1; V.aux[0] = A_CAN; V.gloss = ""; V.compl = 0; }

    /* subject after the verb (llega el bus, ¿dónde está la radio?) */
    if (subj_ch < 0 && !V.gustar && !V.there && !V.imper && (V.person == 3 || V.person == 6) && j < end &&
        it[j].kind == IT_WORD &&
        (((accidental || (LX_FLAGS(V.main_v) & XLEX_SUBJ_AFTER)) && !lemma_is(V.main_v, "ser") &&
          !lemma_is(V.main_v, "estar")) ||
         (q && (lemma_is(v, "ser") || lemma_is(v, "estar")) && !V.naux &&
          ((item_func_class(j) == XC_DET && has_noun(j + 1)) || is_name_tok(it[j].t0)))) &&
        (item_func_class(j) == XC_DET || (it[j].nm.n >= 0 && it[j].ph < 0 && decide(j) == R_N) || is_name_tok(it[j].t0))) {
        int s0 = nch, e;
        if (is_name_tok(it[j].t0)) { emit_item_src(j, 0); e = j + 1; }
        else e = np(j, end);
        if (nch > s0) {
            subj_ch = s0;
            V.subj = 0;
            V.en_person = (V.person == 6 || st.np_pl) ? 6 : 3;
            vp_write(&V, q, st.neg, adverb, subj_ch);
            st.neg = 0; st.adv = -1; st.hadverb = 1; st.lastverb = V.main_v; last_person = V.person;
            return e;
        }
    }

    if (st.adv >= 0 && PH_HAS(st.adv, XF_YA) && V.tense == XT_PRES && (V.person == 1 || V.person == 4) && !st.neg &&
        !V.naux) {
        V.tense = XT_FUT;               /* ya te contesto: I'll answer you right away */
        adverb = 0;
        st.suffix = "right away";
    }
    vp_write(&V, q, st.neg, adverb, subj_ch);
    if (st.neg) st.negcl = 1;
    st.neg = 0;
    st.adv = -1;
    st.hadverb = 1;
    st.lastverb = V.main_v;
    last_person = V.person;
    last_imper = V.imper;
    return j;
}

/* ------------------------------------------------------------ one item */

static int motion_verb(int v)
{
    static const char *const mv[] = { "ir", "venir", "llegar", "volver", "regresar", "bajar", "subir", "entrar",
                                      "salir", "viajar", "pasar", "irse", "mudarse", "acercarse", "correr", "caminar",
                                      "mandar", "enviar", "llevar", "traer" };
    unsigned k;
    for (k = 0; k < sizeof mv / sizeof mv[0]; k++)
        if (lemma_is(v, mv[k])) return 1;
    return 0;
}

static int que_is_question(int i)
{
    int start = (i == st.cstart) || (i > 0 && it[i - 1].use == R_FUNC &&
                                     (PH_CLASS(it[i - 1].ph) == XC_INTJ || PH_CLASS(it[i - 1].ph) == XC_CONJ));
    if (tk[it[i].t0].fl & TF_ACC) return 1;
    if (sent_q == 1 && start) return 1;
    if (sent_q == 1 && i + 1 < nit && item_punct(i + 1)) return 1;
    if (sent_q && (has_noun(i + 1) && !has_verb(i + 1)) && (st.hadverb || start)) return 1;
    return 0;
}

/* Re piola tu nodo, muy buena la señal: adjective first, no verb -> "Your node is really cool" */
static int predicate_first(int i, int end)
{
    int a0 = i, intens = -1, n0, k, e, s0;
    if (i != st.cstart || it[i].kind != IT_WORD) return -1;
    if (it[i].ph >= 0 && PH_CLASS(it[i].ph) == XC_ADV && PH_HAS(it[i].ph, XF_INT)) { intens = i; a0 = i + 1; }
    if (a0 >= end || it[a0].kind != IT_WORD || !has_adj(a0) || it[a0].nm.a < 0 ||
        (it[a0].ph >= 0 && !PH_HAS(it[a0].ph, XF_ADJ)) || (it[a0].vb.v >= 0 && is_finite_t(it[a0].vb.t))) return -1;
    n0 = a0 + 1;
    if (!(n0 < end && item_func_class(n0) == XC_DET && has_noun(n0 + 1)))     /* buena pregunta is just adj + noun */
        return -1;
    for (k = n0; k < end && !item_punct(k); k++)
        if (verbish(k) && is_finite_t(it[k].vb.t)) return -1;
    k = n0 + 2;
    while (k < end && it[k].kind == IT_WORD && it[k].ph < 0 && has_adj(k)) k++;
    if (!(k >= end || item_punct(k))) return -1;
    s0 = nch;
    e = np(n0, end);
    if (nch == s0) return -1;
    emit(st.np_pl ? "are" : "is");
    if (intens >= 0) { emit(ph_en(it[intens].ph)); it[intens].use = R_FUNC; }
    emit_adj(a0);
    it[a0].use = R_A;
    return e;
}

static int item_go(int i, int end)
{
    xl_item *I = &it[i];
    int r, c;
    if (I->use == R_SKIP) return i + 1;
    if (tk[I->t0].oq && i > st.cstart) {        /* ¿ in the middle: a question clause starts here */
        clause_reset(i);
        sent_q = 1;
        q_done = 0;
    }
    if (i == st.cstart) {
        int e = predicate_first(i, end);
        if (e > i) return e;
    }
    if (I->kind == IT_PUNCT) {
        const xl_tok *t = &tk[I->t0];
        char p = t->key[0];
        clause_reset(i + 1);
        if (i + 1 < nit && tk[it[i + 1].t0].q != sent_q) {   /* the question may be only the last clause */
            sent_q = tk[it[i + 1].t0].q;
            q_done = 0;
        }
        if (p == '\n') { emit_n("\n", 1, CF_GLUE | CF_GLUER | CF_NL); return i + 1; }
        if (strchr(",.!?;:)]}%", p)) emit_n(src + t->at, t->len, CF_GLUE | CF_PASS);
        else if (strchr("([{", p)) emit_n(src + t->at, t->len, CF_GLUER | CF_PASS | ((t->fl & TF_SPACE) ? 0 : CF_GLUE));
        else emit_n(src + t->at, t->len, CF_PASS | ((t->fl & TF_SPACE) ? 0 : CF_GLUE));
        return i + 1;
    }
    if (I->kind == IT_OTHER) {
        emit_item_src(i, (tk[I->t0].fl & TF_SPACE) ? 0 : CF_GLUE);
        return i + 1;
    }
    if (I->kind == IT_NUM) {
        if (i + 1 < end && has_noun(i + 1) && it[i + 1].ph < 0) return np(i, end);
        emit_item_src(i, (tk[I->t0].fl & TF_SPACE) ? 0 : CF_GLUE);
        return i + 1;
    }
    if (I->kind == IT_PHRASE) {
        c = PH_CLASS(I->ph);
        if (c == XC_DET && i + 1 < end && (has_noun(i + 1) || has_adj(i + 1))) return np(i, end);
        if (c == XC_Q && (i == st.cstart)) st.qword = 1;
        if (c == XC_CONJ) { clause_reset(i + 1); }
        if (c == XC_CL) st.hadverb = 1;
        emit(ph_en(I->ph));
        return i + 1;
    }
    r = decide(i);
    I->use = (uint8_t)r;
    switch (r) {
    case R_PASS:
        {
            int fl = (tk[I->t0].fl & TF_SPACE) || i == 0 ? 0 : CF_GLUE;
            int s = nch;
            emit_item_src(i, fl);
            if ((tk[I->t0].fl & TF_CAP) && (i == st.cstart || st.qword)) {
                st.np_ch = s; st.np_end = i + 1; st.np_pl = 0; st.np_clause = 1;
            }
        }
        return i + 1;
    case R_N:
        return np(i, end);
    case R_A:
        if (i > 0 && (it[i - 1].use == R_VERB || it[i - 1].use == R_FUNC) && !has_noun(i + 1)) { emit_adj(i); return i + 1; }
        if (has_noun(i + 1) && it[i + 1].ph < 0) return np(i, end);
        emit_adj(i);
        return i + 1;
    case R_R:
        if (I->nm.fl & LF_RE) emit("really");
        emit(lx_gloss(I->nm.r));
        return i + 1;
    case R_I:
        emit(lx_gloss(I->nm.in));
        return i + 1;
    case R_VERB:
        if (I->vb.v < 0) { emit_item_src(i, 0); return i + 1; }
        return vp(i, end);
    case R_FUNC:
        break;
    default:
        emit_item_src(i, 0);
        return i + 1;
    }

    /* function words */
    c = PH_CLASS(I->ph);
    {
        const char *k = tk[I->t0].key;
        int ph = I->ph;
        switch (c) {
        case XC_DET:
            if (PH_HAS(ph, XF_CLIT) && verb_follows(i + 1) && !has_noun(i + 1)) {
                if (st.ncl < 3) st.cl[st.ncl++] = ph;
                return i + 1;
            }
            if ((streq(k, "el") || streq(k, "tu")) && has_verb(i + 1) && !has_noun(i + 1) && !has_adj(i + 1)) {
                int s = emit(streq(k, "el") ? "he" : "you");
                st.subj_ch = s; st.subj_p = streq(k, "el") ? 3 : 2;
                return i + 1;
            }
            if (streq(k, "lo") && has_adj(i + 1)) {      /* lo bueno: the good thing */
                emit("the");
                emit_adj(i + 1);
                emit("thing");
                return i + 2;
            }
            return np(i, end);
        case XC_PRON:
            {
                int s = emit(ph_en(ph));
                if (st.subj_ch < 0 && !st.hadverb) {
                    st.subj_ch = s;
                    st.subj_p = PH_HAS(ph, XF_U) ? 2 : PH_PERSON(ph);
                    if (PH_HAS(ph, XF_U) && PH_PERSON(ph) == 6) st.subj_p = 2;
                } else if (st.hadverb && !item_is(i - 1, "a") && !item_is(i - 1, "con") && !item_is(i - 1, "para") &&
                           !item_is(i - 1, "de") && (PH_PERSON(ph) == last_person ||
                                                     (PH_HAS(ph, XF_U) && (last_person == 3 || last_person == 6)) ||
                                                     (PH_PERSON(ph) == 2 && last_person == 2))) {
                    ch[s].len = 0;      /* ¿ves vos? the verb said it */
                } else if (st.hadverb) {
                    /* after the verb: an object (te quiero a vos) */
                    static const char *const obj[] = { "", "me", "you", "him", "us", "you", "them" };
                    int p = PH_PERSON(ph);
                    ch[s].len = 0;
                    if (streq(k, "ella")) emit("her");
                    else if (PH_HAS(ph, XF_U)) emit("you");
                    else emit(obj[p]);
                }
            }
            return i + 1;
        case XC_CLIT:
            if (streq(k, "se") && !verb_follows(i + 1) && item_func_class(i + 1) != XC_CLIT && !unknown_verbish(i + 1)) {
                /* sé: I know */
                xl_vp V;
                memset(&V, 0, sizeof V);
                V.tense = XT_PRES; V.person = 1; V.en_person = 1; V.gloss = "know"; V.main_v = verb_inf("saber");
                V.subj = st.subj_ch >= 0 ? 0 : "I";
                vp_write(&V, sent_q == 1, st.neg, 0, st.subj_ch);
                if (st.neg) st.negcl = 1;
                st.neg = 0; st.hadverb = 1;
                return i + 1;
            }
            if (streq(k, "se") && unknown_verbish(i + 1)) return i + 1;
            if (verb_follows(i) || (streq(k, "se") && item_func_class(i + 1) == XC_CLIT)) {
                if (st.ncl < 3) st.cl[st.ncl++] = ph;
                return i + 1;
            }
            emit(ph_en(ph));
            return i + 1;
        case XC_NEG:
            if (streq(k, "no")) {
                if (verb_follows(i + 1) || (item_is(i + 1, "se") && !verb_follows(i + 2))) {
                    st.neg = 1; st.negcl = 1; return i + 1;
                }
                if (i + 1 < end && (has_adj(i + 1) || item_func_class(i + 1) == XC_ADV || item_func_class(i + 1) == XC_DET))
                    emit("not");
                else emit(ph_alt(ph));
                return i + 1;
            }
            /* nunca, jamás, tampoco */
            if (st.hadverb || st.negcl) { emit(ph_alt(ph)); return i + 1; }
            if (verb_follows(i + 1)) {
                if (streq(k, "tampoco")) { st.neg = 1; st.negcl = 1; st.suffix = "either"; }
                else { st.adv = ph; st.negcl = 1; }
                return i + 1;
            }
            emit(ph_en(ph));
            return i + 1;
        case XC_ADV:
            if (PH_HAS(ph, XF_YA) && item_is(i + 1, "no")) { st.suffix = "anymore"; return i + 1; }
            if ((streq(k, "todavia") || streq(k, "aun")) && item_is(i + 1, "no")) { st.suffix = "yet"; return i + 1; }
            if (PH_HAS(ph, XF_SOLO) && i > 0 && (lemma_is(st.lastverb, "estar") || lemma_is(st.lastverb, "quedar") ||
                                                  lemma_is(st.lastverb, "quedarse"))) { emit("alone"); return i + 1; }
            if (PH_HAS(ph, XF_MAS)) {
                st.more = 1;
                if (st.negcl && (item_punct(i + 1) || i + 1 >= end)) { emit("anymore"); return i + 1; }
                if (i + 1 < end && it[i + 1].kind == IT_WORD && it[i + 1].nm.a >= 0 && it[i + 1].ph < 0) {
                    char b[64];
                    int sup = i > 0 && it[i - 1].ph >= 0 && PH_CLASS(it[i - 1].ph) == XC_DET && PH_HAS(it[i - 1].ph, XF_DEF);
                    en_compare(lx_gloss(it[i + 1].nm.a), sup, b, sizeof b);
                    emit(b);
                    it[i + 1].use = R_A;
                    return i + 2;
                }
            }
            if (streq(k, "mas") || streq(k, "menos") || streq(k, "mejor") || streq(k, "peor")) st.more = 1;
            if (streq(k, "tambien") && !st.hadverb && verb_follows(i + 1)) { st.adv = -2; return i + 1; }
            if (streq(k, "asi") && st.hadverb && verb_follows(i + 1)) {   /* prendé el GPS así te vemos */
                emit("so");
                clause_reset(i + 1);
                return i + 1;
            }
            if (!PH_HAS(ph, XF_INT) && !st.hadverb && verb_follows(i + 1) && i + 1 < end &&
                (PH_HAS(ph, XF_YA) || streq(k, "siempre") || streq(k, "tambien") || streq(k, "todavia") ||
                 streq(k, "casi") || streq(k, "apenas") || streq(k, "recien") || streq(k, "solo") || streq(k, "aun"))) {
                st.adv = ph;
                if (streq(k, "recien")) st.adv = ph;
                return i + 1;
            }
            if (PH_HAS(ph, XF_INT) && streq(k, "re") && i + 1 < end && has_verb(i + 1) && !has_adj(i + 1)) {
                st.adv = ph;   /* re va: really goes */
                return i + 1;
            }
            emit((st.hadverb && !PH_HAS(ph, XF_INT) && *NEXT(ph_en(ph)) && item_punct(i + 1)) ? ph_alt(ph) : ph_en(ph));
            return i + 1;
        case XC_PREP:
            /* estoy a tres cuadras (de la estación): three blocks (from) */
            if ((streq(k, "a")) && i + 2 < end && (it[i + 1].kind == IT_NUM || item_func_class(i + 1) == XC_NUM) &&
                (!st.hadverb || lemma_is(st.lastverb, "estar") || lemma_is(st.lastverb, "quedar") ||
                 lemma_is(st.lastverb, "vivir") || lemma_is(st.lastverb, "quedarse") || lemma_is(st.lastverb, "ser")) &&
                (KEY_IN(i + 2, DISTANCE) || item_is(i + 2, "km") || item_is(i + 2, "kms") || item_is(i + 2, "mts"))) {
                dist_mode = 1;
                if (item_is(i + 2, "km") || item_is(i + 2, "kms") || item_is(i + 2, "mts")) {
                    emit_item_src(i + 1, 0);
                    emit_item_src(i + 2, 0);
                    if (item_is(i + 3, "de") || item_is(i + 3, "del")) {
                        emit(item_is(i + 3, "del") ? "from the" : "from");
                        dist_mode = 0;
                        return i + 4;
                    }
                    emit("away");
                    return i + 3;
                }
                return i + 1;
            }
            /* al 20%: at; al domingo: to Sunday */
            if ((streq(k, "al") || streq(k, "a")) && i + 1 < end && it[i + 1].kind == IT_NUM) { emit("at"); return i + 1; }
            if (streq(k, "al") && KEY_IN(i + 1, WEEKDAY)) { emit("to"); return i + 1; }
            if (streq(k, "del") && KEY_IN(i + 1, WEEKDAY)) { emit("from"); return i + 1; }
            if (streq(k, "a")) {
                /* a mí me gusta: said already by the pronoun */
                if (i + 2 < end && (item_func_class(i + 1) == XC_PRONX || item_func_class(i + 1) == XC_PRON) &&
                    item_func_class(i + 2) == XC_CLIT) { it[i + 1].use = R_FUNC; return i + 2; }
                /* a las 3 */
                if (i + 2 < end && (item_is(i + 1, "las") || item_is(i + 1, "la")) &&
                    (it[i + 2].kind == IT_NUM || item_func_class(i + 2) == XC_NUM)) {
                    emit("at");
                    if (it[i + 2].kind == IT_NUM) emit_item_src(i + 2, 0); else emit(ph_en(it[i + 2].ph));
                    return i + 3;
                }
                /* the personal a: veo a Juan */
                if (keep_a) { keep_a = 0; emit("to"); return i + 1; }
                if (st.hadverb && !motion_verb(st.lastverb) && i + 1 < end &&
                    (is_name_tok(it[i + 1].t0) || item_func_class(i + 1) == XC_PRON || item_func_class(i + 1) == XC_PRONX ||
                     item_is(i + 1, "alguien") || item_is(i + 1, "nadie")))
                    return i + 1;
                if (is_inf(i + 1)) { emit("to"); no_to = 1; return vp(i + 1, end); }
            }
            if ((streq(k, "para") || streq(k, "pa")) && item_is(i + 1, "no") && is_inf(i + 2)) {
                emit("so as not to");
                no_to = 1;
                return vp(i + 2, end);
            }
            if ((streq(k, "para") || streq(k, "pa")) && is_inf(i + 1)) {
                emit("to");
                it[i].use = R_FUNC;
                return vp(i + 1, end);
            }
            if (streq(k, "sin") && is_inf(i + 1)) { emit("without"); return vp(i + 1, end); }
            if (streq(k, "de") || streq(k, "del")) {
                if (is_inf(i + 1)) { emit("to"); return vp(i + 1, end); }
                if (i > 0 && (it[i - 1].use == R_VERB || item_func_class(i - 1) == XC_ADV) &&
                    (lemma_is(st.lastverb, "venir") || lemma_is(st.lastverb, "llegar") || lemma_is(st.lastverb, "salir") ||
                    lemma_is(st.lastverb, "volver") || lemma_is(st.lastverb, "regresar") || lemma_is(st.lastverb, "irse") ||
                    lemma_is(st.lastverb, "bajar") || (lemma_is(st.lastverb, "ser") && is_name_tok(it[i + 1].t0)))) {
                    emit(streq(k, "del") ? "from the" : "from");
                    return i + 1;
                }
            }
            if (streq(k, "al") && is_inf(i + 1)) { emit("when"); return vp(i + 1, end); }
            if (streq(k, "en") && i + 1 < end && is_time_noun(i + 1)) { emit("in"); return i + 1; }
            if (streq(k, "por") && i + 2 < end && item_func_class(i + 1) == XC_DET && has_noun(i + 2)) {
                static const char *const along[] = { "calle", "ruta", "avenida", "autopista", "camino", "costanera", "vereda",
                                                     "orilla", "costa" };
                static const char *const through[] = { "puerta", "ventana", "entrada", "tunel", "parque", "plaza", "zona",
                                                       "barrio", "centro", "bosque", "campo" };
                if (KEY_IN(i + 2, along)) { emit("along"); return i + 1; }
                if (KEY_IN(i + 2, through)) { emit("through"); return i + 1; }
            }
            if (streq(k, "en") && i + 2 < end && item_func_class(i + 1) == XC_DET && has_noun(i + 2)) {
                static const char *const on[] = { "terraza", "techo", "azotea", "cerro", "monta\xf1" "a", "ruta", "calle",
                                                  "esquina", "mapa", "piso", "suelo", "pantalla", "playa", "colectivo",
                                                  "tren", "bondi", "red", "canal", "aire", "linea" };
                static const char *const at[] = { "puerta", "casa", "trabajo", "estacion", "oficina", "escuela",
                                                  "reunion", "fiesta", "laburo" };
                unsigned q2;
                const char *nk = lx_key(it[i + 2].nm.n);
                for (q2 = 0; q2 < sizeof on / sizeof on[0]; q2++) if (streq(nk, on[q2])) { emit("on"); return i + 1; }
                for (q2 = 0; q2 < sizeof at / sizeof at[0]; q2++) if (streq(nk, at[q2])) { emit("at"); return i + 1; }
            }
            if (streq(k, "hasta") && i + 1 < end && it[i + 1].kind == IT_WORD && !is_time_noun(i + 1) &&
                !is_time_noun(i + 2) && (item_func_class(i + 1) == XC_DET || has_noun(i + 1) || is_name_tok(it[i + 1].t0))) {
                emit("to");
                return i + 1;
            }
            emit(ph_en(ph));
            return i + 1;
        case XC_CONJ:
            if (PH_HAS(ph, XF_QUE)) {
                if (sent_q == 2 && i + 2 < end && has_adj(i + 1) && has_noun(i + 2) && it[i + 1].ph < 0) {
                    emit((it[i + 2].nm.fl & LF_PL) ? "what" : "what a");
                    return np(i + 1, end);
                }
                if (que_is_question(i)) {
                    int hn = has_noun(i + 1) && !has_verb(i + 1);
                    if (sent_q == 2 && (has_adj(i + 1) || item_func_class(i + 1) == XC_ADV)) emit("how");
                    else emit("what");
                    if (i == st.cstart) st.qword = 1;
                    if (hn) { int e = np(i + 1, end); (void)e; return e; }
                    return i + 1;
                }
                if (sent_q == 2 && i == st.cstart && (has_adj(i + 1) || item_func_class(i + 1) == XC_ADV)) {
                    emit("how");
                    return i + 1;
                }
                if (st.more) { emit("than"); st.more = 0; return i + 1; }
                if (i > 0 && is_inf(i + 1)) {
                    /* no sé qué hacer: what to do */
                    emit("what to");
                    no_to = 1;
                    return vp(i + 1, end);
                }
                emit("that");
                {
                    int rel = i > 0 && (it[i - 1].use == R_N || it[i - 1].use == R_PASS || st.np_end == i);
                    clause_reset(i + 1);
                    rel_subj = rel;     /* la tormenta que viene: no "he" */
                }
                return i + 1;
            }
            if (PH_HAS(ph, XF_SI)) {
                if ((tk[I->t0].fl & TF_ACC) || item_punct(i + 1) || i + 1 >= end) { emit("yes"); return i + 1; }
                emit("if");
                clause_reset(i + 1);
                return i + 1;
            }
            if (PH_HAS(ph, XF_COMO)) {
                if ((tk[I->t0].fl & TF_ACC) || (sent_q == 1 && i == st.cstart)) {
                    emit("how");
                    st.qword = 1;
                    return i + 1;
                }
                if (st.subj_ch >= 0 || item_is(i - 1, "no")) {
                    /* yo como: I eat */
                    xl_verb vb;
                    if (verb_plain("como", 0, 0, &vb)) { it[i].vb = vb; it[i].use = R_VERB; return vp(i, end); }
                }
                if (i + 1 < end && it[i + 1].kind == IT_NUM) { emit("about"); return i + 1; }
                emit(verb_follows(i + 1) ? "as" : ph_en(ph));
                clause_reset(i + 1);
                return i + 1;
            }
            if (PH_HAS(ph, XF_WHY)) {
                emit(sent_q == 1 && (i == st.cstart) ? "why" : ph_en(ph));
                if (sent_q == 1 && i == st.cstart) st.qword = 1;
                clause_reset(i + 1);
                if (sent_q == 1) st.qword = 1;
                return i + 1;
            }
            if (PH_HAS(ph, XF_Q)) {
                int qs = sent_q == 1 && i == st.cstart;
                emit(ph_en(ph));
                if (!qs) clause_reset(i + 1);
                st.qword = qs;
                if (!qs) st.cstart = i + 1;
                return i + 1;
            }
            emit(ph_en(ph));
            if (streq(k, "y") || streq(k, "e") || streq(k, "o") || streq(k, "u") || streq(k, "ni")) {
                st.neg = 0; st.ncl = 0; st.adv = -1;
                if (verb_follows(i + 1)) { st.subj_ch = -1; st.np_end = -1; }
                return i + 1;
            }
            clause_reset(i + 1);
            return i + 1;
        case XC_Q:
            if (sent_q == 2 && streq(k, "que") && i + 2 < end && has_adj(i + 1) && has_noun(i + 2)) {
                emit((it[i + 2].nm.fl & LF_PL) ? "what" : "what a");
                return np(i + 1, end);
            }
            if (sent_q == 2 && streq(k, "que") && has_adj(i + 1) && !has_noun(i + 1)) { emit("how"); return i + 1; }
            emit(ph_en(ph));
            if (i == st.cstart) st.qword = 1;
            if (has_noun(i + 1) && !has_verb(i + 1) && !item_punct(i + 1)) return np(i + 1, end);
            return i + 1;
        case XC_PRONX:
            {
                int subj = !st.hadverb && st.subj_ch < 0 && verb_follows(i + 1);
                int s = emit((st.negcl || st.neg || (sent_q == 1 && !PH_HAS(ph, XF_NEG))) && !subj ? ph_alt(ph)
                             : (subj && sent_q == 1 && !PH_HAS(ph, XF_NEG)) ? ph_alt(ph) : ph_en(ph));
                if (subj) { st.subj_ch = s; st.subj_p = 3; }
            }
            return i + 1;
        case XC_INTJ:
            if (PH_HAS(ph, XF_SI)) { emit("yes"); return i + 1; }
            emit(ph_en(ph));
            return i + 1;
        case XC_NUM:
            if (item_is(i, "medio") && has_adj(i + 1)) { emit("kind of"); return i + 1; }
            if (i + 1 < end && has_noun(i + 1) && it[i + 1].ph < 0) return np(i, end);
            emit(ph_en(ph));
            return i + 1;
        default:
            emit(ph_en(ph));
            return i + 1;
        }
    }
}

/* -------------------------------------------------------------- assembly */

static int put(char *out, size_t cap, size_t *n, const char *s, int len)
{
    int k;
    for (k = 0; k < len; k++) {
        if (*n + 1 >= cap) return 0;
        out[(*n)++] = s[k];
    }
    return 1;
}

/* Join the chunks of one batch into out. */
static void assemble(char *out, size_t cap, size_t *n, int first_cap)
{
    int c, sent = 1, glue_next = 0;
    size_t start = *n;                  /* the caller puts the space between batches */
    for (c = 0; c < nch; c++) {
        const char *s = arena + ch[c].off;
        int len = ch[c].len, k;
        char tmp[256];
        if (!len) continue;
        if (*n > start && !(ch[c].fl & CF_GLUE) && !glue_next && out[*n - 1] != '\n') {
            if (!put(out, cap, n, " ", 1)) return;
        }
        glue_next = (ch[c].fl & CF_GLUER) != 0;
        memcpy(tmp, s, (size_t)len);
        if (all_upper && !(ch[c].fl & CF_PASS)) {
            for (k = 0; k < len; k++) tmp[k] = (char)upc(tmp[k]);
        } else if (sent && first_cap && !(ch[c].fl & CF_PASS) && tmp[0] >= 'a' && tmp[0] <= 'z') {
            tmp[0] = (char)upc(tmp[0]);
        }
        if (!(ch[c].fl & CF_GLUE) || (ch[c].fl & CF_PASS) == 0) sent = 0;
        if (ch[c].fl & CF_PASS) sent = 0;
        if (len == 1 && (tmp[0] == '.' || tmp[0] == '!' || tmp[0] == '?' || tmp[0] == '\n')) sent = 1;
        else if (len > 1 && (tmp[len - 1] == '.' || tmp[len - 1] == '!' || tmp[len - 1] == '?') && (ch[c].fl & CF_GLUE)) sent = 1;
        if (!put(out, cap, n, tmp, len)) return;
    }
}

/* Sentence starts that were written with a capital in the source. */
static int src_caps(void)
{
    int t;
    for (t = 0; t < ntk; t++)
        if (tk[t].kind == TK_WORD) return (tk[t].fl & TF_CAP) != 0;
    return 0;
}

static void translate_batch(char *out, size_t cap, size_t *n)
{
    int i = 0, caps = src_caps();
    nch = 0;
    narena = 0;
    build_items();
    memset(&st, 0, sizeof st);
    st.adv = -1;
    st.lastverb = -1;
    clause_reset(0);
    while (i < nit) {
        int e = i;
        /* one sentence */
        while (e < nit && !(it[e].kind == IT_PUNCT && strchr(".!?\n", tk[it[e].t0].key[0]))) e++;
        if (e < nit) e++;
        sent_q = tk[it[i].t0].q;
        last_person = 0;
        keep_a = 0;
        q_done = 0;
        last_imper = 0;
        dist_mode = 0;
        usted_seen = 0;
        {
            int u;
            for (u = i; u < e; u++)
                if (item_is(u, "usted") || item_is(u, "ud") || item_is(u, "ustedes") || item_is(u, "uds") ||
                    (sent_q == 1 && (item_is(u, "su") || item_is(u, "sus"))))
                    usted_seen = 1;
        }
        clause_reset(i);
        while (i < e) {
            int nx = item_go(i, e);
            if (nx <= i) nx = i + 1;
            i = nx;
        }
        if (st.suffix) {
            /* before the sentence's closing mark */
            int c = emit(st.suffix);
            st.suffix = 0;
            if (c > 0 && (ch[c - 1].fl & CF_GLUE) && ch[c - 1].len && strchr(".!?", arena[ch[c - 1].off])) move_chunk(c, c - 1);
        }
    }
    assemble(out, cap, n, caps);
}

static void detect_upper(const char *s)
{
    int lo = 0, up = 0, words = 0, in = 0;
    for (; *s; s++) {
        int l = (*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z');
        if (*s >= 'a' && *s <= 'z') lo++;
        else if (*s >= 'A' && *s <= 'Z') up++;
        if (l && !in) words++;
        in = l;
    }
    all_upper = up >= 6 && lo == 0 && words >= 2;
}

int mt_xlate_es_en(const char *in, char *out, size_t cap)
{
    size_t n = 0;
    int pos = 0;
    if (!out || !cap) return 0;
    out[0] = 0;
    if (!in || !*in) return 0;
    detect_upper(in);
    while (in[pos]) {
        int used;
        src = in + pos;                     /* token positions are relative to the batch */
        used = tokenize(0, 1);
        if (ntk) {
            int gap = pos > 0 && (in[pos - 1] == ' ' || in[pos] == ' ' || in[pos - 1] == '\n');
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

mt_lang_t mt_xlate_detect(const char *in, int *conf)
{
    int es = 0, en = 0, pos = 0, t;
    if (conf) *conf = 0;
    if (!in || !*in) return MT_LANG_UNKNOWN;
    detect_upper(in);
    while (in[pos]) {
        int next;
        src = in + pos;
        next = tokenize(0, 0);
        for (t = 0; t < ntk; t++) {
            const xl_tok *T = &tk[t];
            const char *k = T->key;
            int w_es = 0, w_en = 0, p, e, n = (int)strlen(k);
            xl_nom nm;
            xl_verb vb;
            if (T->q == 1 && T->sstart) { /* ¿ handled by the accent counter below */ }
            if (T->kind != TK_WORD || (T->fl & (TF_ALNUM | TF_LONG))) continue;
            if ((T->fl & TF_CAP) && !T->sstart && t > 0 && !all_upper) continue;   /* names */
            if (streq(k, "ok") || streq(k, "okey") || streq(k, "okay") || streq(k, "oka")) continue;
            if (T->fl & TF_ACC) w_es += 15;
            if (strchr(k, (char)0xF1)) w_es += 15;
            if (strchr(k, '\'')) w_en += 12;
            p = phr_find(k, (T->fl & TF_ACC) != 0, 0);
            e = find(XL_ENW, NENW, k);
            if (p >= 0) {
                int c = PH_CLASS(p);
                /* words English uses too */
                if (streq(k, "a") || streq(k, "me") || streq(k, "no") || streq(k, "solo") || streq(k, "con") ||
                    streq(k, "e") || streq(k, "o") || streq(k, "u") || streq(k, "haha") || streq(k, "eh") ||
                    streq(k, "ah") || streq(k, "oh") || streq(k, "ay") || streq(k, "ey") || streq(k, "roger") ||
                    streq(k, "negativo") || streq(k, "afirmativo"))
                    w_es += 3;
                else w_es += (c == XC_INTJ || c == XC_PRON || c == XC_DET || c == XC_CONJ || c == XC_PREP) ? 12 : 10;
            } else if (is_laugh(k)) {
                w_es += 8;
            } else if (find(XL_ABBR, NABBR, k) >= 0) {
                w_es += n == 1 ? 3 : 8;
            }
            if (e >= 0) w_en += (XL_ENW[e] >> 22) & 1 ? 12 : 9;
            if (p < 0 && w_en < 9) {
                nominal(k, &nm);
                if (nom_any(&nm) || verb_plain(k, (T->fl & TF_ACCL) != 0, (T->fl & TF_ACC) != 0, &vb) ||
                    verb_enclitic(k, &vb))
                    w_es += 6;
            } else if (p < 0 && e >= 0) {
                nominal(k, &nm);
                if (nom_any(&nm) || verb_plain(k, 0, 0, &vb)) w_es += 2;
            }
            if (!w_en && !w_es) {
                if (n >= 5 && ends(k, n, "ing")) w_en += 5;
                else if (strstr(k, "th") || strstr(k, "sh") || strchr(k, 'w')) w_en += 3;
                else if (ends(k, n, "cion") || ends(k, n, "mente") || ends(k, n, "ado") || ends(k, n, "ando")) w_es += 4;
            }
            /* a two-word phrase of the table (buen día, nos vemos) */
            if (t + 1 < ntk && tk[t + 1].kind == TK_WORD) {
                char b2[XL_KEY * 2 + 2];
                snprintf(b2, sizeof b2, "%.27s %.27s", k, tk[t + 1].key);
                if (phr_find(b2, 0, 0) >= 0) w_es += 12;
            }
            es += w_es;
            en += w_en;
        }
        for (t = 0; t < ntk; t++)
            if (tk[t].sstart && tk[t].q) {
                /* the inverted marks are Spanish only: count them when the source has them */
            }
        if (next <= 0) break;
        pos += next;
    }
    {
        const unsigned char *s = (const unsigned char *)in;
        for (; *s; s++) if (s[0] == 0xC2 && (s[1] == 0xBF || s[1] == 0xA1)) es += 15;
    }
    {
        int total = es + en, best = es > en ? es : en, c;
        if (total < 8 || es == en) return MT_LANG_UNKNOWN;
        c = best * 100 / total;
        if (total < 15) c = c * total / 15;
        if (c < 50 && best * 100 / total < 60) return MT_LANG_UNKNOWN;
        if (conf) *conf = c;
        return es > en ? MT_LANG_ES : MT_LANG_EN;
    }
}

int mt_xlate_incoming(const char *in, char *out, size_t cap)
{
    int conf = 0;
    if (!out || !cap) return 0;
    out[0] = 0;
    if (mt_xlate_detect(in, &conf) != MT_LANG_ES || conf < MT_XLATE_MIN_CONF) return 0;
    return mt_xlate_es_en(in, out, cap);
}

int mt_xlate_fold_ascii(char *s)
{
    unsigned char *r = (unsigned char *)s, *w = (unsigned char *)s;
    if (!s) return 0;
    while (*r) {
        int n, acc, up, l;
        if (*r < 0x80) { *w++ = *r++; continue; }
        if (r[0] == 0xC2 && (r[1] == 0xBF || r[1] == 0xA1)) { r += 2; continue; }
        l = letter(r, &n, &acc, &up);
        if (l) {
            int c = l == 0xF1 ? 'n' : l;
            *w++ = (unsigned char)(up ? upc(c) : c);
            r += n;
            continue;
        }
        /* any other sequence: one '?' */
        *w++ = '?';
        r++;
        while (*r >= 0x80 && !(r[0] == 0xC3 && letter(r, &n, &acc, &up)) && !(r[0] == 0xC2 && (r[1] == 0xBF || r[1] == 0xA1)))
            r++;
    }
    *w = 0;
    return (int)(w - (unsigned char *)s);
}

/* ------------------------------------------------------------- phrasebook */

int mt_phrase_count(void) { return NBOOK; }

mt_phrase_t mt_phrase(int i)
{
    mt_phrase_t p = { "", "", "" };
    if (i < 0 || i >= NBOOK) return p;
    p.en = P(XL_BOOK[i]);
    p.es = NEXT(p.en);
    p.category = P(XL_BOOKCAT[(XL_BOOK[i] >> 22) & 63]);
    return p;
}

int mt_phrase_category_count(void) { return NCAT; }

const char *mt_phrase_category(int c)
{
    return (c < 0 || c >= NCAT) ? "" : P(XL_BOOKCAT[c]);
}
