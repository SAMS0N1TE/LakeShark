/* Offline translation for chat lines heard on the mesh: which language a line
   is in, a readable gist of it in English, and a phrasebook for sending. No
   network, no malloc; static buffers only, so call it from one task at a time.

   Spanish is built in (mt_xlate.c, data mt_xlate_es_en.h from
   tools/gen_xlate.py). Portuguese, French, German, Italian, Dutch, Polish,
   Russian and Ukrainian are read
   by mt_xlate_lang.c from a data file each (lang/<code>.bin, made by
   tools/gen_xlate_lang.py), which the caller loads (from the card) and hands
   over with mt_xlate_lang_attach(). Detection and the phrasebooks of every
   language are built in, so they work without the files. See README.md for
   the sources. The English side (verb forms, plurals, negation) is shared. */
#pragma once
#include <stddef.h>

typedef enum {
    MT_LANG_UNKNOWN = 0,    /* too short, numbers, names, "ok", or no clear winner */
    MT_LANG_EN = 1,
    MT_LANG_ES = 2,
    MT_LANG_PT = 3,
    MT_LANG_FR = 4,
    MT_LANG_DE = 5,
    MT_LANG_IT = 6,
    MT_LANG_NL = 7,
    MT_LANG_PL = 8,
    MT_LANG_RU = 9,             /* Russian and Ukrainian are read from Cyrillic or from */
    MT_LANG_UK = 10,            /* chat Latin ("privet", "pryvit") alike */
    MT_LANG_COUNT
} mt_lang_t;
#define MT_LANG_FIRST_CARD MT_LANG_PT       /* languages from here on need their data file */

/* "es", "pt" ...; "" for unknown. */
const char *mt_lang_code(mt_lang_t lang);
/* "SPANISH", "PORTUGUESE" ...; "" for unknown. */
const char *mt_lang_name(mt_lang_t lang);
/* The language with that two-letter code, or MT_LANG_UNKNOWN. */
mt_lang_t mt_lang_from_code(const char *code);

/* The language of one chat line (UTF-8, NUL-terminated), among all of the
   above. *confidence_pct (may be NULL) is 0..100 for the answer given; 0 with
   MT_LANG_UNKNOWN. Works on short lines without accents and on mixed lines
   (the language with most of the words wins, at a lower confidence). A line
   in Cyrillic is always Russian or Ukrainian: ы э ъ ё say Russian, і ї є ґ
   Ukrainian, else the words; a line both could have written is Russian at
   MT_XLATE_MIN_CONF. */
mt_lang_t mt_xlate_detect(const char *utf8, int *confidence_pct);

/* Spanish -> English gist of in_utf8 into out (cap bytes, always
   NUL-terminated, never overflows; cap 0 writes nothing). Returns the length
   written. Words it does not know, numbers, names, emoji and other bytes pass
   through unchanged (so the output is UTF-8, ASCII when the input was).
   Costs a few binary searches per word: well under a millisecond for a
   200-byte message on the board. Any input length is accepted. */
int mt_xlate_es_en(const char *in_utf8, char *out, size_t cap);

/* The same for any language: Spanish always; the others once their data is
   attached (0 and out = "" before that). Same limits as mt_xlate_es_en. */
int mt_xlate_to_en(mt_lang_t from, const char *in_utf8, char *out, size_t cap);

/* The usual call for an incoming text message: if it reads as Spanish with
   at least MT_XLATE_MIN_CONF confidence, the English gist into out and its
   length; otherwise 0 and out = "". */
#define MT_XLATE_MIN_CONF 55
int mt_xlate_incoming(const char *in_utf8, char *out, size_t cap);

/* The same over a set of languages: bit (1u << lang) for each one the user
   translates from. The line's language goes to *lang (may be NULL) whatever
   the answer; the gist only when that language is in the set and ready. */
#define MT_LANG_BIT(l) (1u << (l))
int mt_xlate_incoming_in(const char *in_utf8, unsigned langs, char *out, size_t cap, mt_lang_t *lang);
/* mt_xlate_detect for someone who reads langs: a Cyrillic line only a few
   words tell apart (no ы э ъ ё, no і ї є ґ) goes to whichever of Russian and
   Ukrainian is in langs and attached. */
mt_lang_t mt_xlate_detect_in(const char *utf8, unsigned langs, int *confidence_pct);

/* Card data of one language (the bytes of lang/<code>.bin). The bytes must
   stay where they are, unchanged, until mt_xlate_lang_detach() or another
   attach of that language. Returns 0, or <0 when the file is not that
   language's data (wrong magic, code or sizes): nothing is kept then. */
int mt_xlate_lang_attach(mt_lang_t lang, const void *data, size_t len);
void mt_xlate_lang_detach(mt_lang_t lang);
/* Spanish (built in) or an attached language: mt_xlate_to_en works. */
int mt_xlate_lang_ready(mt_lang_t lang);

/* In place: accented Latin letters to plain ASCII (a with acute -> a,
   n with tilde -> n, sharp s -> ss, oe ligature -> oe, Polish l with stroke
   -> l), Cyrillic letters to Latin (at most two letters each: shch -> sh),
   inverted ? and ! dropped, any other non-ASCII sequence to one '?'. For a
   screen that only draws ASCII. Returns the new length. */
int mt_xlate_fold_ascii(char *s);

/* A line for a screen that draws Latin only: Cyrillic transliterated
   (BGN/PCGN without marks: Privet, Zhdu, Shchas; the Ukrainian scheme when
   the line has a Ukrainian letter, or lang says Ukrainian), letters of
   Latin Extended-A to their plain letter (l for l with stroke); anything
   else copied. Always NUL-terminated within cap; returns the length. The
   input itself is what is sent and kept. */
int mt_xlate_screen(const char *in_utf8, mt_lang_t lang, char *out, size_t cap);
/* The line has a letter mt_xlate_screen changes. */
int mt_xlate_needs_screen(const char *in_utf8);

/* Phrasebook: about fifty ready lines (mesh, outdoors, meetup, emergency)
   with what to send in each language. Strings are static and UTF-8. */
typedef struct {
    const char *en;         /* "Where are you?" */
    const char *es;         /* "¿Dónde estás?" (Rioplatense: vos) */
    const char *category;   /* "basic", "radio", "status", "battery", "meetup", "emergency", "weather" */
} mt_phrase_t;

int mt_phrase_count(void);
mt_phrase_t mt_phrase(int i);           /* i out of range: all three "" */
/* Phrase i in a language (EN gives the English, UNKNOWN or out of range ""). */
const char *mt_phrase_in(int i, mt_lang_t lang);
int mt_phrase_category_count(void);
const char *mt_phrase_category(int c);  /* in phrasebook order; "" out of range */
