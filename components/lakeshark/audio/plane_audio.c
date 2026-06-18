
#include "plane_audio.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static const char *const MIL_PREFIXES[] = {

    "RCH",
    "SAM",
    "PAT",
    "SHADO",
    "KNIFE",
    "GRIM",
    "SPAR",
    "CNV",
    "VV",
    "NAVY",
    "MC",
    "ARMY",
    "CG",

    "RRR",
    "NATO",
    "MAGMA",

    "CFC",
    "HUSKY",

    "COTAM",
    "GAF",
    NULL
};

typedef struct { uint32_t lo, hi; const char *note; } icao_range_t;
static const icao_range_t MIL_RANGES[] = {
    { 0xADF7C8, 0xAFFFFF, "US MIL"   },
    { 0x43C000, 0x43FFFF, "UK MIL"   },
    { 0x3F8000, 0x3FFFFF, "LUFTWAFFE"},
    { 0x3B7000, 0x3B7FFF, "FR MIL"   },
    { 0xC87F00, 0xC87FFF, "CAF"      },
    { 0, 0, NULL }
};

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

static bool callsign_is_ga(const char *cs)
{
    if (!cs || !*cs) return false;

    if (cs[0] == 'N' && isdigit((unsigned char)cs[1])) return true;

    if (cs[0] == 'G' && isalpha((unsigned char)cs[1]) && isalpha((unsigned char)cs[2])) return true;

    if (cs[0] == 'E' && cs[1] == 'I' && isalpha((unsigned char)cs[2])) return true;

    if (cs[0] == 'D' && isalpha((unsigned char)cs[1]) && isalpha((unsigned char)cs[2])) return true;

    if (cs[0] == 'F' && isalpha((unsigned char)cs[1]) && isalpha((unsigned char)cs[2])) return true;

    if (cs[0] == 'C' && isalpha((unsigned char)cs[1]) && isalpha((unsigned char)cs[2])
        && cs[1] != 'F') return true;
    return false;
}

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

static const char *const PHONETIC[26] = {
    "ALFA","BRAVO","CHARLIE","DELTA","ECHO","FOXTROT","GOLF","HOTEL",
    "INDIA","JULIETT","KILO","LIMA","MIKE","NOVEMBER","OSCAR","PAPA",
    "QUEBEC","ROMEO","SIERRA","TANGO","UNIFORM","VICTOR","WHISKEY","XRAY",
    "YANKEE","ZULU"
};
static const char *const DIGIT_WORD[10] = {
    "ZERO","ONE","TWO","THREE","FOUR","FIVE","SIX","SEVEN","EIGHT","NINER"
};

static void append_spoken_char(char *buf, size_t sz, size_t *pos, char c)
{
    const char *word = NULL;
    char lit[2] = {0, 0};

    if (isalpha((unsigned char)c)) word = PHONETIC[toupper((unsigned char)c) - 'A'];
    else if (isdigit((unsigned char)c)) word = DIGIT_WORD[c - '0'];
    else if (c == '-') return;
    else { lit[0] = c; word = lit; }

    size_t want = strlen(word) + 1;
    if (*pos + want >= sz) return;
    memcpy(buf + *pos, word, want - 1);
    *pos += want - 1;
    buf[(*pos)++] = ' ';
    buf[*pos]     = 0;
}

static void spell_string(char *out, size_t sz, size_t *pos, const char *s)
{
    for (; s && *s; s++) append_spoken_char(out, sz, pos, *s);
}

static void spell_icao(char *out, size_t sz, size_t *pos, uint32_t icao)
{
    static const char HEX[] = "0123456789ABCDEF";
    for (int shift = 20; shift >= 0; shift -= 4) {
        append_spoken_char(out, sz, pos, HEX[(icao >> shift) & 0xF]);
    }
}

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

        spell_icao(out, out_sz, &pos, icao);
    }

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
