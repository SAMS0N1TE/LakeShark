/*
 * plane_audio.c - ADS-B callsign/ICAO classifier and SAM phrase builder.
 *
 * Classification rules (applied in priority order):
 *   1. Military - callsign prefix is in MIL_PREFIXES *or* ICAO falls in a
 *      known national military block (US AE00-AFFF, UK 43C0-43FF, etc.).
 *      Military wins over commercial if either matches.
 *   2. GA - callsign starts with a civil tail-number registry prefix
 *      (N/G-/EI-/D-/F-/VH-...) followed by digits/letters.
 *   3. Commercial - callsign starts with 3 letters, then digits
 *      (standard ATC flight ID: UAL123, RYR4721, AFR84).
 *   4. Unknown - anything else, or empty callsign.
 *
 * Phrases are plain English uppercased by the SAM wrapper. Digits and
 * letters in callsigns are spelled out phonetically because SAM butchers
 * alphanumeric strings badly otherwise.
 */
#include "plane_audio.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* -- known military callsign prefixes -------------------------------------
 * Non-exhaustive - covers US, UK, NATO common ones you'd actually hear in
 * North America / North Atlantic airspace. Easy to extend. */
static const char *const MIL_PREFIXES[] = {
    /* US Air Force */
    "RCH",    /* Reach - AMC transport                       */
    "SAM",    /* Special Air Mission (VIP transport)         */
    "PAT",    /* US Army Priority Air Transport              */
    "SHADO",  /* USAF SR-71/U-2/tanker (rare but iconic)     */
    "KNIFE",  /* AFSOC                                        */
    "GRIM",   /* AC-130                                       */
    "SPAR",   /* SAAM VIP                                     */
    "CNV",    /* US Navy Convoy                               */
    "VV",     /* US Navy prefix (callsign begins VV)          */
    "NAVY",
    "MC",     /* Marine Corps                                 */
    "ARMY",
    "CG",     /* Coast Guard                                  */
    /* UK / NATO */
    "RRR",    /* RAF Ascot (transport)                        */
    "NATO",
    "MAGMA",  /* RAF Typhoon QRA                              */
    /* Canada */
    "CFC",    /* Canadian Forces Cargo                        */
    "HUSKY",
    /* France / Germany */
    "COTAM",  /* French Air Force transport                   */
    "GAF",    /* German Air Force                             */
    NULL
};

/* -- military ICAO blocks ------------------------------------------------
 * Inclusive ranges. Keep short; the 5-10 busiest blocks catch almost every
 * mil hit. Add more as you encounter them. */
typedef struct { uint32_t lo, hi; const char *note; } icao_range_t;
static const icao_range_t MIL_RANGES[] = {
    { 0xADF7C8, 0xAFFFFF, "US MIL"   },  /* USAF/USN/USA common block       */
    { 0x43C000, 0x43FFFF, "UK MIL"   },  /* RAF/RN                          */
    { 0x3F8000, 0x3FFFFF, "LUFTWAFFE"},  /* German military                 */
    { 0x3B7000, 0x3B7FFF, "FR MIL"   },  /* French AdlA subset              */
    { 0xC87F00, 0xC87FFF, "CAF"      },  /* Canadian Forces                 */
    { 0, 0, NULL }
};

/* -- helpers --------------------------------------------------------------- */

static bool starts_with_ci(const char *s, const char *pfx)
{
    while (*pfx) {
        if (toupper((unsigned char)*s) != toupper((unsigned char)*pfx)) return false;
        s++; pfx++;
    }
    return true;
}

static bool callsign_is_mil(const char *cs)
{
    if (!cs || !*cs) return false;
    for (int i = 0; MIL_PREFIXES[i]; i++) {
        if (starts_with_ci(cs, MIL_PREFIXES[i])) return true;
    }
    return false;
}

static bool icao_is_mil(uint32_t icao)
{
    for (int i = 0; MIL_RANGES[i].note; i++) {
        if (icao >= MIL_RANGES[i].lo && icao <= MIL_RANGES[i].hi) return true;
    }
    return false;
}

/* GA tail-number patterns: a registry prefix followed by alphanumerics.
 * Most common ones - this catches the 80% case. */
static bool callsign_is_ga(const char *cs)
{
    if (!cs || !*cs) return false;
    /* US: N followed by digits then optional letters (N12345, N737BA) */
    if (cs[0] == 'N' && isdigit((unsigned char)cs[1])) return true;
    /* UK: G-XXXX → but ADS-B strips the dash, arrives as GXXXX */
    if (cs[0] == 'G' && isalpha((unsigned char)cs[1]) && isalpha((unsigned char)cs[2])) return true;
    /* Ireland: EI-XXX → EIXXX */
    if (cs[0] == 'E' && cs[1] == 'I' && isalpha((unsigned char)cs[2])) return true;
    /* Germany: D-XXXX → DXXXX */
    if (cs[0] == 'D' && isalpha((unsigned char)cs[1]) && isalpha((unsigned char)cs[2])) return true;
    /* France: F-XXXX */
    if (cs[0] == 'F' && isalpha((unsigned char)cs[1]) && isalpha((unsigned char)cs[2])) return true;
    /* Canada: C-XXXX → CXXXX (but be careful - CFC is mil, handled earlier) */
    if (cs[0] == 'C' && isalpha((unsigned char)cs[1]) && isalpha((unsigned char)cs[2])
        && cs[1] != 'F') return true;
    return false;
}

/* Commercial: exactly 3 letters, then 1+ digits, then optional trailing letters. */
static bool callsign_is_commercial(const char *cs)
{
    if (!cs || !*cs) return false;
    if (!isalpha((unsigned char)cs[0])) return false;
    if (!isalpha((unsigned char)cs[1])) return false;
    if (!isalpha((unsigned char)cs[2])) return false;
    if (!isdigit((unsigned char)cs[3])) return false;
    return true;
}

plane_category_t plane_classify(uint32_t icao, const char *callsign)
{
    /* Military wins over everything if either heuristic matches. */
    if (icao_is_mil(icao) || callsign_is_mil(callsign)) return PLANE_MILITARY;
    if (callsign_is_ga(callsign))                        return PLANE_GA;
    if (callsign_is_commercial(callsign))                return PLANE_COMMERCIAL;
    return PLANE_UNKNOWN;
}

const char *plane_category_label(plane_category_t cat)
{
    switch (cat) {
    case PLANE_MILITARY:   return "MIL";
    case PLANE_COMMERCIAL: return "COM";
    case PLANE_GA:         return "GA";
    default:               return "UNK";
    }
}

/* -- phonetic spell-out --------------------------------------------------- */

/* NATO phonetic alphabet + digit words. SAM pronounces these cleanly. */
static const char *const PHONETIC[26] = {
    "ALFA","BRAVO","CHARLIE","DELTA","ECHO","FOXTROT","GOLF","HOTEL",
    "INDIA","JULIETT","KILO","LIMA","MIKE","NOVEMBER","OSCAR","PAPA",
    "QUEBEC","ROMEO","SIERRA","TANGO","UNIFORM","VICTOR","WHISKEY","XRAY",
    "YANKEE","ZULU"
};
static const char *const DIGIT_WORD[10] = {
    "ZERO","ONE","TWO","THREE","FOUR","FIVE","SIX","SEVEN","EIGHT","NINER"
};

/* Append one character's spoken form, plus a trailing space. Safe against
 * overflow - silently truncates if out of room. */
static void append_spoken_char(char *buf, size_t sz, size_t *pos, char c)
{
    const char *word = NULL;
    char lit[2] = {0, 0};

    if (isalpha((unsigned char)c)) word = PHONETIC[toupper((unsigned char)c) - 'A'];
    else if (isdigit((unsigned char)c)) word = DIGIT_WORD[c - '0'];
    else if (c == '-') return;   /* skip dashes entirely */
    else { lit[0] = c; word = lit; }

    size_t want = strlen(word) + 1;
    if (*pos + want >= sz) return;
    memcpy(buf + *pos, word, want - 1);
    *pos += want - 1;
    buf[(*pos)++] = ' ';
    buf[*pos]     = 0;
}

/* Spell out a string character by character. Used when we don't have a
 * callsign and have to fall back to ICAO hex. */
static void spell_string(char *out, size_t sz, size_t *pos, const char *s)
{
    for (; s && *s; s++) append_spoken_char(out, sz, pos, *s);
}

/* Spell an ICAO as 6 hex characters. */
static void spell_icao(char *out, size_t sz, size_t *pos, uint32_t icao)
{
    static const char HEX[] = "0123456789ABCDEF";
    for (int shift = 20; shift >= 0; shift -= 4) {
        append_spoken_char(out, sz, pos, HEX[(icao >> shift) & 0xF]);
    }
}

/* -- the phrase builders -------------------------------------------------- */

void plane_phrase_new_contact(char *out, size_t out_sz,
                              uint32_t icao, const char *callsign,
                              plane_category_t cat, bool crc_shaky)
{
    if (!out || out_sz < 8) return;
    size_t pos = 0;
    out[0] = 0;

    if (crc_shaky) {
        const char *pfx = "QUESTIONABLE. ";
        size_t L = strlen(pfx);
        if (L < out_sz) { memcpy(out, pfx, L); pos = L; out[pos] = 0; }
    }

    const char *lead = "NEW CONTACT. ";
    size_t L = strlen(lead);
    if (pos + L < out_sz) { memcpy(out + pos, lead, L); pos += L; out[pos] = 0; }

    if (callsign && *callsign) {
        spell_string(out, out_sz, &pos, callsign);
    } else {
        /* No callsign yet - fall back to spelling ICAO. Reads as
         * "ALFA ECHO ZERO ONE TWO THREE" for AE0123. */
        spell_icao(out, out_sz, &pos, icao);
    }

    /* Category suffix - keeps the sentence short but informative. */
    const char *suffix = NULL;
    switch (cat) {
    case PLANE_MILITARY:   suffix = ". MILITARY.";   break;
    case PLANE_COMMERCIAL: suffix = ". COMMERCIAL."; break;
    case PLANE_GA:         suffix = ". GENERAL AVIATION."; break;
    default:               suffix = ".";             break;
    }
    size_t slen = strlen(suffix);
    if (pos + slen < out_sz) { memcpy(out + pos, suffix, slen); pos += slen; }
    out[pos < out_sz ? pos : out_sz - 1] = 0;
}

void plane_phrase_lost_contact(char *out, size_t out_sz,
                               uint32_t icao, const char *callsign)
{
    if (!out || out_sz < 8) return;
    size_t pos = 0;
    out[0] = 0;

    const char *lead = "LOST CONTACT. ";
    size_t L = strlen(lead);
    if (L < out_sz) { memcpy(out, lead, L); pos = L; out[pos] = 0; }

    if (callsign && *callsign) spell_string(out, out_sz, &pos, callsign);
    else                       spell_icao  (out, out_sz, &pos, icao);

    if (pos + 1 < out_sz) { out[pos++] = '.'; out[pos] = 0; }
}

void plane_phrase_position(char *out, size_t out_sz,
                           uint32_t icao, const char *callsign)
{
    if (!out || out_sz < 8) return;
    size_t pos = 0;
    out[0] = 0;

    const char *lead = "POSITION FIX. ";
    size_t L = strlen(lead);
    if (L < out_sz) { memcpy(out, lead, L); pos = L; out[pos] = 0; }

    if (callsign && *callsign) spell_string(out, out_sz, &pos, callsign);
    else                       spell_icao  (out, out_sz, &pos, icao);

    if (pos + 1 < out_sz) { out[pos++] = '.'; out[pos] = 0; }
}
