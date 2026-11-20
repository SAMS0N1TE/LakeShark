/* Between the two halves of the translator: mt_xlate.c (Spanish, and the
   English writer's word forms) and mt_xlate_lang.c (the other languages and
   detection across all of them). Not for the app. */
#pragma once
#include "mt_xlate.h"

/* English verb forms of the first word of a gloss ("take care of": take). */
enum { MTX_BASE, MTX_S, MTX_PAST, MTX_PP, MTX_ING };
void mtx_en_form(const char *verb, int form, char *dst, int cap);
void mtx_en_plural(const char *gloss, char *dst, int cap);
void mtx_negate(char *word, int cap);        /* do -> don't, can -> can't, else "x not" */
int mtx_vowel_start(const char *word);       /* takes "an" */
/* The Spanish-or-English reading of a line (the 1.3 detector). */
mt_lang_t mtx_detect_es_en(const char *utf8, int *confidence_pct);
/* A folded word Spanish's tables know (function word, chat spelling, irregular verb form). */
int mtx_es_word(const char *key, int acc);
int mtx_es_lemma(const char *key);           /* a headword of the Spanish lexicon */
/* Points a language's own tables give the words of a line (es always, the others when attached). */
void mtx_votes(const char *utf8, int *v);
/* For the in-place screen fold: a Cyrillic letter at s as at most two Latin
   letters (NULL when it is not one), and a Latin Extended-A letter as its plain one (0 if not). */
const char *mtx_cyr2(const unsigned char *s, int *up);
char mtx_lata(const unsigned char *s);
/* Word-profile scores of en es pt fr de it nl pl ru uk for a line; returns the words found. */
int mtx_profile(const char *utf8, int *score);
