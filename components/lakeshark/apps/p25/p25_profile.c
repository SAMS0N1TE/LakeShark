#include "p25_profile.h"

#include <limits.h>
#include <string.h>

#include "grant_follower.h"

enum {
    SEEN_VERSION        = 1u << 0,
    SEEN_SYSTEM         = 1u << 1,
    SEEN_SITE           = 1u << 2,
    SEEN_PREFERRED      = 1u << 3,
    SEEN_AUTO_FOLLOW    = 1u << 4,
    SEEN_ENCRYPTED_SKIP = 1u << 5,
    SEEN_SKIP_MS        = 1u << 6,
    SEEN_DEMOD          = 1u << 7,
    SEEN_CQPSK_TIMING   = 1u << 8,
    SEEN_CQPSK_CARRIER  = 1u << 9,
    SEEN_PHASE2_FOLLOW  = 1u << 10,
};

typedef enum {
    NUMBER_OK,
    NUMBER_INVALID,
    NUMBER_OVERFLOW,
} number_result_t;

const char *p25_profile_error_reason(p25_profile_error_t error)
{
    switch (error) {
    case P25_PROFILE_ERROR_NONE:                   return "ok";
    case P25_PROFILE_ERROR_ARGUMENT:               return "invalid parser argument or tune ranges";
    case P25_PROFILE_ERROR_INPUT_TOO_LONG:         return "profile exceeds maximum byte size";
    case P25_PROFILE_ERROR_LINE_TOO_LONG:          return "line exceeds maximum length";
    case P25_PROFILE_ERROR_SYNTAX:                 return "expected key=value with printable text";
    case P25_PROFILE_ERROR_UNKNOWN_FIELD:          return "unknown field";
    case P25_PROFILE_ERROR_DUPLICATE_FIELD:        return "singleton field appears more than once";
    case P25_PROFILE_ERROR_MISSING_FIELD:          return "required field is missing";
    case P25_PROFILE_ERROR_UNSUPPORTED_VERSION:    return "unsupported profile version";
    case P25_PROFILE_ERROR_INVALID_BOOLEAN:        return "boolean must be true or false";
    case P25_PROFILE_ERROR_INVALID_DEMOD:          return "unknown demod preference";
    case P25_PROFILE_ERROR_UNSAFE_LOOP_GAIN:       return "CQPSK loop gain is non-finite or outside safe bounds";
    case P25_PROFILE_ERROR_INVALID_NUMBER:         return "number must contain decimal digits only";
    case P25_PROFILE_ERROR_NUMBER_OVERFLOW:        return "number exceeds the destination integer";
    case P25_PROFILE_ERROR_VALUE_TOO_LONG:         return "text value is empty or too long";
    case P25_PROFILE_ERROR_FREQUENCY_OUT_OF_RANGE: return "frequency is outside the endpoint tune ranges";
    case P25_PROFILE_ERROR_DUPLICATE_CONTROL:      return "control frequency is duplicated";
    case P25_PROFILE_ERROR_CONTROL_CAPACITY:       return "too many control frequencies";
    case P25_PROFILE_ERROR_PREFERRED_NOT_FOUND:    return "preferred control is not in the control list";
    case P25_PROFILE_ERROR_MALFORMED_TALKGROUP:    return "talkgroup must be id|alias|enabled|priority";
    case P25_PROFILE_ERROR_MALFORMED_CONTROL:      return "control must be hz or hz|lat|lon|radius_m";
    case P25_PROFILE_ERROR_MALFORMED_COORDINATE:   return "latitude or longitude is not decimal degrees in range";
    case P25_PROFILE_ERROR_RADIUS_OUT_OF_RANGE:    return "site radius must be 500 to 200000 metres";
    case P25_PROFILE_ERROR_GEO_NEEDS_VERSION_2:    return "control coordinates require version=2";
    case P25_PROFILE_ERROR_DUPLICATE_TALKGROUP:    return "talkgroup ID is duplicated";
    case P25_PROFILE_ERROR_TALKGROUP_CAPACITY:     return "too many talkgroups";
    case P25_PROFILE_ERROR_PRIORITY_CAPACITY:      return "too many nonzero-priority talkgroups";
    default:                                       return "unknown profile error";
    }
}

static bool fail(p25_profile_diagnostic_t *diagnostic, size_t line,
                 p25_profile_error_t error)
{
    if (diagnostic) {
        diagnostic->line = line;
        diagnostic->code = error;
        diagnostic->reason = p25_profile_error_reason(error);
    }
    return false;
}

static char *trim(char *text)
{
    while (*text == ' ' || *text == '\t') text++;
    char *end = text + strlen(text);
    while (end > text && (end[-1] == ' ' || end[-1] == '\t')) end--;
    *end = '\0';
    return text;
}

static number_result_t parse_u64(const char *text, uint64_t *value)
{
    uint64_t result = 0;
    if (!text || !*text) return NUMBER_INVALID;
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        if (*p < '0' || *p > '9') return NUMBER_INVALID;
        unsigned digit = (unsigned)(*p - '0');
        if (result > (UINT64_MAX - digit) / UINT64_C(10))
            return NUMBER_OVERFLOW;
        result = result * UINT64_C(10) + digit;
    }
    *value = result;
    return NUMBER_OK;
}

static bool parse_bool(const char *text, bool *value)
{
    if (strcmp(text, "true") == 0) {
        *value = true;
        return true;
    }
    if (strcmp(text, "false") == 0) {
        *value = false;
        return true;
    }
    return false;
}

static bool text_valid(const char *text, size_t capacity, char forbidden)
{
    size_t length = strlen(text);
    if (length == 0 || length >= capacity) return false;
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        if (*p < 0x20u || *p == 0x7fu || (forbidden && *p == (unsigned char)forbidden))
            return false;
    }
    return true;
}

static bool frequency_allowed(uint64_t hz,
                              const p25_profile_parse_config_t *config)
{
    for (size_t i = 0; i < config->tune_range_count; ++i) {
        if (hz >= config->tune_ranges[i].min_hz &&
            hz <= config->tune_ranges[i].max_hz)
            return true;
    }
    return false;
}

static p25_profile_error_t number_error(number_result_t result)
{
    return result == NUMBER_OVERFLOW ? P25_PROFILE_ERROR_NUMBER_OVERFLOW
                                     : P25_PROFILE_ERROR_INVALID_NUMBER;
}

static bool singleton_once(unsigned *seen, unsigned bit,
                           p25_profile_diagnostic_t *diagnostic, size_t line)
{
    if ((*seen & bit) != 0)
        return fail(diagnostic, line, P25_PROFILE_ERROR_DUPLICATE_FIELD);
    *seen |= bit;
    return true;
}

/* Degrees as a person writes them: an optional sign, digits, an
   optional point and up to seven decimals, returned times ten
   million.
   Fixed point throughout, deliberately. strtod would drag the locale
   into a file format, and in a locale with a comma decimal separator
   a coordinate would parse as a truncated integer and place a site
   hundreds of miles away - silently, and only on somebody else's
   machine. */
static bool parse_degrees_e7(const char *text, int32_t limit_deg, int32_t *out)
{
    if (!text || !*text || !out) return false;

    bool negative = false;
    if (*text == '+' || *text == '-') {
        negative = (*text == '-');
        text++;
    }
    if (!*text) return false;

    int64_t whole = 0;
    size_t digits = 0;
    while (*text >= '0' && *text <= '9') {
        whole = whole * 10 + (*text - '0');
        if (whole > 1000) return false;          /* far past any legal degree */
        text++;
        digits++;
    }
    if (!digits) return false;

    int64_t frac = 0;
    if (*text == '.') {
        text++;
        size_t places = 0;
        while (*text >= '0' && *text <= '9') {
            /* Past the seventh decimal the value cannot be represented, and
               a coordinate with more of them is a copy-paste, not a
               measurement. Consume and ignore rather than refuse. */
            if (places < 7) frac = frac * 10 + (*text - '0');
            places++;
            text++;
        }
        if (!places) return false;               /* a trailing point is a typo */
        while (places < 7 && places++ < 7) frac *= 10;
    }
    if (*text) return false;                     /* trailing rubbish */

    int64_t value = whole * 10000000 + frac;
    if (value > (int64_t)limit_deg * 10000000) return false;
    *out = (int32_t)(negative ? -value : value);
    return true;
}

static bool parse_control(p25_profile_t *profile, const char *value,
                          const p25_profile_parse_config_t *config,
                          p25_profile_diagnostic_t *diagnostic, size_t line)
{
    /* Two shapes, and the short one is still the whole of version 1:

           control=851012500
           control=851012500|43.4406|-71.6498|25000

       The long one says where the site is and how far it reaches, so the
       receiver can pick it from a position instead of from a survey. All
       four parts or none; a half-filled coordinate is a mistake worth
       refusing rather than guessing at. */
    char *parts[4] = { value, NULL, NULL, NULL };
    size_t nparts = 1;
    for (size_t i = 1; i < 4; ++i) {
        char *separator = strchr(parts[i - 1], '|');
        if (!separator) break;
        *separator = '\0';
        parts[i] = separator + 1;
        nparts = i + 1;
    }
    if (nparts != 1 && nparts != 4)
        return fail(diagnostic, line, P25_PROFILE_ERROR_MALFORMED_CONTROL);
    if (nparts == 4 && strchr(parts[3], '|'))
        return fail(diagnostic, line, P25_PROFILE_ERROR_MALFORMED_CONTROL);
    for (size_t i = 0; i < nparts; ++i) parts[i] = trim(parts[i]);

    uint64_t hz;
    number_result_t nr = parse_u64(parts[0], &hz);
    if (nr != NUMBER_OK) return fail(diagnostic, line, number_error(nr));
    if (!frequency_allowed(hz, config))
        return fail(diagnostic, line, P25_PROFILE_ERROR_FREQUENCY_OUT_OF_RANGE);
    for (size_t i = 0; i < profile->control_count; ++i)
        if (profile->control_channels[i] == hz)
            return fail(diagnostic, line, P25_PROFILE_ERROR_DUPLICATE_CONTROL);
    if (profile->control_count >= P25_PROFILE_CONTROL_MAX)
        return fail(diagnostic, line, P25_PROFILE_ERROR_CONTROL_CAPACITY);

    int32_t lat_e7 = 0, lon_e7 = 0;
    uint64_t radius = 0;
    if (nparts == 4) {
        /* Coordinates are a version 2 thing. A version 1 file carrying them
           is refused outright: a reader that ignored them would follow the
           wrong site and say nothing about it. */
        if (profile->format_version < 2U)
            return fail(diagnostic, line, P25_PROFILE_ERROR_GEO_NEEDS_VERSION_2);
        if (!parse_degrees_e7(parts[1], 90, &lat_e7) ||
            !parse_degrees_e7(parts[2], 180, &lon_e7))
            return fail(diagnostic, line, P25_PROFILE_ERROR_MALFORMED_COORDINATE);
        nr = parse_u64(parts[3], &radius);
        if (nr != NUMBER_OK) return fail(diagnostic, line, number_error(nr));
        /* Metres. A radius under half a kilometre is almost certainly
           kilometres typed by mistake, and a site that can never be entered
           is worse than one that is always eligible. */
        if (radius < 500U || radius > 200000U)
            return fail(diagnostic, line, P25_PROFILE_ERROR_RADIUS_OUT_OF_RANGE);
    }

    const size_t at = profile->control_count++;
    profile->control_channels[at] = hz;
    profile->control_lat_e7[at] = lat_e7;
    profile->control_lon_e7[at] = lon_e7;
    profile->control_radius_m[at] = (uint32_t)radius;
    profile->control_has_geo[at] = (nparts == 4);
    return true;
}


static bool parse_demod(const char *value, int *preference)
{
    if (strcmp(value, "auto") == 0)          *preference = P25_DEMOD_AUTO;
    else if (strcmp(value, "c4fm") == 0)     *preference = DEMOD_C4FM;
    else if (strcmp(value, "cqpsk") == 0)    *preference = DEMOD_CQPSK;
    else if (strcmp(value, "diff_4fsk") == 0)*preference = DEMOD_DIFF_4FSK;
    else if (strcmp(value, "fsk4_tracking") == 0)
        *preference = DEMOD_FSK4_TRACKING;
    else return false;
    return true;
}

static bool parse_talkgroup(p25_profile_t *profile, char *value,
                            p25_profile_diagnostic_t *diagnostic, size_t line)
{
    char *parts[4];
    parts[0] = value;
    for (size_t i = 1; i < 4; ++i) {
        char *separator = strchr(parts[i - 1], '|');
        if (!separator)
            return fail(diagnostic, line, P25_PROFILE_ERROR_MALFORMED_TALKGROUP);
        *separator = '\0';
        parts[i] = separator + 1;
    }
    if (strchr(parts[3], '|'))
        return fail(diagnostic, line, P25_PROFILE_ERROR_MALFORMED_TALKGROUP);
    for (size_t i = 0; i < 4; ++i) parts[i] = trim(parts[i]);

    uint64_t id_value;
    number_result_t nr = parse_u64(parts[0], &id_value);
    if (nr != NUMBER_OK)
        return fail(diagnostic, line, number_error(nr));
    if (id_value == 0 || id_value > UINT16_MAX)
        return fail(diagnostic, line, P25_PROFILE_ERROR_MALFORMED_TALKGROUP);
    if (!text_valid(parts[1], P25_PROFILE_ALIAS_LEN, '|'))
        return fail(diagnostic, line, P25_PROFILE_ERROR_VALUE_TOO_LONG);

    bool enabled;
    if (!parse_bool(parts[2], &enabled))
        return fail(diagnostic, line, P25_PROFILE_ERROR_INVALID_BOOLEAN);

    uint64_t priority_value;
    nr = parse_u64(parts[3], &priority_value);
    if (nr != NUMBER_OK)
        return fail(diagnostic, line, number_error(nr));
    if (priority_value > UINT8_MAX)
        return fail(diagnostic, line, P25_PROFILE_ERROR_NUMBER_OVERFLOW);

    uint16_t id = (uint16_t)id_value;
    for (size_t i = 0; i < profile->talkgroup_count; ++i)
        if (profile->talkgroups[i].id == id)
            return fail(diagnostic, line, P25_PROFILE_ERROR_DUPLICATE_TALKGROUP);
    if (profile->talkgroup_count >= P25_PROFILE_TALKGROUP_MAX)
        return fail(diagnostic, line, P25_PROFILE_ERROR_TALKGROUP_CAPACITY);
    if (priority_value != 0) {
        size_t priority_count = 0;
        for (size_t i = 0; i < profile->talkgroup_count; ++i)
            if (profile->talkgroups[i].priority != 0) priority_count++;
        if (priority_count >= P25_PROFILE_PRIORITY_MAX)
            return fail(diagnostic, line, P25_PROFILE_ERROR_PRIORITY_CAPACITY);
    }

    p25_profile_talkgroup_t *tg = &profile->talkgroups[profile->talkgroup_count++];
    tg->id = id;
    memcpy(tg->alias, parts[1], strlen(parts[1]) + 1U);
    tg->enabled = enabled;
    tg->priority = (uint8_t)priority_value;
    return true;
}

static bool parse_line(p25_profile_t *profile, char *line_text,
                       const p25_profile_parse_config_t *config,
                       unsigned *seen, p25_profile_diagnostic_t *diagnostic,
                       size_t line, size_t *preferred_line)
{
    char *key = trim(line_text);
    if (*key == '\0' || *key == '#' || *key == ';') return true;

    for (const unsigned char *p = (const unsigned char *)key; *p; ++p)
        if (*p < 0x20u || *p == 0x7fu)
            return fail(diagnostic, line, P25_PROFILE_ERROR_SYNTAX);

    char *equals = strchr(key, '=');
    if (!equals) return fail(diagnostic, line, P25_PROFILE_ERROR_SYNTAX);
    *equals = '\0';
    char *value = trim(equals + 1);
    key = trim(key);
    if (*key == '\0') return fail(diagnostic, line, P25_PROFILE_ERROR_SYNTAX);

    if (strcmp(key, "version") == 0) {
        if (!singleton_once(seen, SEEN_VERSION, diagnostic, line)) return false;
        uint64_t version;
        number_result_t nr = parse_u64(value, &version);
        if (nr != NUMBER_OK) return fail(diagnostic, line, number_error(nr));
        /* One or two. Version 1 is read exactly as it always was; version
           2 additionally permits coordinates on a control line. */
        if (version < P25_PROFILE_FORMAT_VERSION_MIN ||
            version > P25_PROFILE_FORMAT_VERSION)
            return fail(diagnostic, line, P25_PROFILE_ERROR_UNSUPPORTED_VERSION);
        profile->format_version = (uint16_t)version;
        return true;
    }
    if (strcmp(key, "system") == 0) {
        if (!singleton_once(seen, SEEN_SYSTEM, diagnostic, line)) return false;
        if (!text_valid(value, sizeof(profile->system_name), 0))
            return fail(diagnostic, line, P25_PROFILE_ERROR_VALUE_TOO_LONG);
        memcpy(profile->system_name, value, strlen(value) + 1U);
        return true;
    }
    if (strcmp(key, "site") == 0) {
        if (!singleton_once(seen, SEEN_SITE, diagnostic, line)) return false;
        if (!text_valid(value, sizeof(profile->site_name), 0))
            return fail(diagnostic, line, P25_PROFILE_ERROR_VALUE_TOO_LONG);
        memcpy(profile->site_name, value, strlen(value) + 1U);
        return true;
    }
    if (strcmp(key, "control") == 0)
        return parse_control(profile, value, config, diagnostic, line);
    if (strcmp(key, "preferred") == 0) {
        if (!singleton_once(seen, SEEN_PREFERRED, diagnostic, line)) return false;
        number_result_t nr = parse_u64(value, &profile->preferred_control_hz);
        if (nr != NUMBER_OK) return fail(diagnostic, line, number_error(nr));
        if (!frequency_allowed(profile->preferred_control_hz, config))
            return fail(diagnostic, line, P25_PROFILE_ERROR_FREQUENCY_OUT_OF_RANGE);
        *preferred_line = line;
        return true;
    }
    if (strcmp(key, "auto_follow") == 0) {
        if (!singleton_once(seen, SEEN_AUTO_FOLLOW, diagnostic, line)) return false;
        if (!parse_bool(value, &profile->auto_follow))
            return fail(diagnostic, line, P25_PROFILE_ERROR_INVALID_BOOLEAN);
        return true;
    }
    if (strcmp(key, "phase2_follow") == 0) {
        if (!singleton_once(seen, SEEN_PHASE2_FOLLOW, diagnostic, line)) return false;
        if (!parse_bool(value, &profile->phase2_follow))
            return fail(diagnostic, line, P25_PROFILE_ERROR_INVALID_BOOLEAN);
        profile->phase2_follow_set = true;
        return true;
    }
    if (strcmp(key, "encrypted_skip") == 0) {
        if (!singleton_once(seen, SEEN_ENCRYPTED_SKIP, diagnostic, line)) return false;
        if (!parse_bool(value, &profile->encrypted_skip_enabled))
            return fail(diagnostic, line, P25_PROFILE_ERROR_INVALID_BOOLEAN);
        return true;
    }
    if (strcmp(key, "encrypted_skip_ms") == 0) {
        if (!singleton_once(seen, SEEN_SKIP_MS, diagnostic, line)) return false;
        uint64_t skip_ms;
        number_result_t nr = parse_u64(value, &skip_ms);
        if (nr != NUMBER_OK) return fail(diagnostic, line, number_error(nr));
        if (skip_ms > UINT32_MAX)
            return fail(diagnostic, line, P25_PROFILE_ERROR_NUMBER_OVERFLOW);
        profile->encrypted_skip_ms = (uint32_t)skip_ms;
        return true;
    }
    if (strcmp(key, "demod") == 0) {
        if (!singleton_once(seen, SEEN_DEMOD, diagnostic, line)) return false;
        if (!parse_demod(value, &profile->demod_preference))
            return fail(diagnostic, line, P25_PROFILE_ERROR_INVALID_DEMOD);
        return true;
    }
    if (strcmp(key, "cqpsk_timing_gain") == 0) {
        if (!singleton_once(seen, SEEN_CQPSK_TIMING, diagnostic, line))
            return false;
        if (!p25_cqpsk_gain_parse(value, P25_CQPSK_TIMING_GAIN_MIN,
                                  P25_CQPSK_TIMING_GAIN_MAX,
                                  &profile->cqpsk.timing_gain))
            return fail(diagnostic, line,
                        P25_PROFILE_ERROR_UNSAFE_LOOP_GAIN);
        return true;
    }
    if (strcmp(key, "cqpsk_carrier_gain") == 0) {
        if (!singleton_once(seen, SEEN_CQPSK_CARRIER, diagnostic, line))
            return false;
        if (!p25_cqpsk_gain_parse(value, P25_CQPSK_CARRIER_GAIN_MIN,
                                  P25_CQPSK_CARRIER_GAIN_MAX,
                                  &profile->cqpsk.carrier_gain))
            return fail(diagnostic, line,
                        P25_PROFILE_ERROR_UNSAFE_LOOP_GAIN);
        return true;
    }
    if (strcmp(key, "tg") == 0)
        return parse_talkgroup(profile, value, diagnostic, line);

    return fail(diagnostic, line, P25_PROFILE_ERROR_UNKNOWN_FIELD);
}

bool p25_profile_parse(p25_profile_t *dst,
                       p25_profile_parse_scratch_t *scratch,
                       const char *input, size_t input_len,
                       const p25_profile_parse_config_t *config,
                       p25_profile_diagnostic_t *diagnostic)
{
    if (diagnostic) {
        diagnostic->line = 0;
        diagnostic->code = P25_PROFILE_ERROR_NONE;
        diagnostic->reason = p25_profile_error_reason(P25_PROFILE_ERROR_NONE);
    }
    if (!dst || !scratch || !input || !config ||
        dst == &scratch->candidate || !config->tune_ranges ||
        config->tune_range_count == 0 ||
        config->tune_range_count > LS_RADIO_MAX_FREQUENCY_RANGES)
        return fail(diagnostic, 0, P25_PROFILE_ERROR_ARGUMENT);
    for (size_t i = 0; i < config->tune_range_count; ++i)
        if (config->tune_ranges[i].min_hz > config->tune_ranges[i].max_hz)
            return fail(diagnostic, 0, P25_PROFILE_ERROR_ARGUMENT);
    if (input_len > P25_PROFILE_MAX_FILE_BYTES)
        return fail(diagnostic, 0, P25_PROFILE_ERROR_INPUT_TOO_LONG);

    memset(&scratch->candidate, 0, sizeof(scratch->candidate));
    scratch->candidate.auto_follow = true;
    scratch->candidate.encrypted_skip_enabled = true;
    scratch->candidate.encrypted_skip_ms = P25_GRANT_DEFAULT_ENCRYPTED_SKIP_MS;
    scratch->candidate.demod_preference = P25_DEMOD_AUTO;
    p25_cqpsk_config_defaults(&scratch->candidate.cqpsk);

    unsigned seen = 0;
    size_t offset = 0;
    size_t line_number = 1;
    size_t preferred_line = 0;
    while (offset < input_len) {
        size_t start = offset;
        while (offset < input_len && input[offset] != '\n') offset++;
        size_t length = offset - start;
        if (offset < input_len) offset++;
        if (length > 0 && input[start + length - 1] == '\r') length--;
        if (length > P25_PROFILE_MAX_LINE_LEN)
            return fail(diagnostic, line_number, P25_PROFILE_ERROR_LINE_TOO_LONG);
        if (memchr(input + start, '\0', length) != NULL)
            return fail(diagnostic, line_number, P25_PROFILE_ERROR_SYNTAX);
        memcpy(scratch->line, input + start, length);
        scratch->line[length] = '\0';
        if (!parse_line(&scratch->candidate, scratch->line, config, &seen,
                        diagnostic, line_number, &preferred_line))
            return false;
        line_number++;
    }

    const unsigned required = SEEN_VERSION | SEEN_SYSTEM | SEEN_SITE;
    if ((seen & required) != required || scratch->candidate.control_count == 0)
        return fail(diagnostic, line_number, P25_PROFILE_ERROR_MISSING_FIELD);
    if ((seen & SEEN_PREFERRED) == 0)
        scratch->candidate.preferred_control_hz =
            scratch->candidate.control_channels[0];

    bool preferred_found = false;
    for (size_t i = 0; i < scratch->candidate.control_count; ++i)
        if (scratch->candidate.control_channels[i] ==
            scratch->candidate.preferred_control_hz)
            preferred_found = true;
    if (!preferred_found)
        return fail(diagnostic, preferred_line,
                    P25_PROFILE_ERROR_PREFERRED_NOT_FOUND);

    memcpy(dst, &scratch->candidate, sizeof(*dst));
    if (diagnostic) {
        diagnostic->line = 0;
        diagnostic->code = P25_PROFILE_ERROR_NONE;
        diagnostic->reason = p25_profile_error_reason(P25_PROFILE_ERROR_NONE);
    }
    return true;
}
