#include "fm_mode_label.h"

#include <ctype.h>
#include <stddef.h>
#include <stdlib.h>

/*LS-700  AppFM's positional table omitted ACARS, shifting two labels and
  leaving the last mode without an initializer. Designated entries keep enum values
  attached to their names; the physical-length check makes stale tables safe.

  LS-731 named LISTEN as NFM because that is the actual modulation, and named
  SCAN as SWEEP to distinguish this band-power job from the channel scanner. */
static const char *const s_mode_labels[] = {
    [FM_MODE_LISTEN] = "NFM",
    [FM_MODE_SCAN]   = "SWEEP",
    [FM_MODE_POCSAG] = "POCSAG",
    [FM_MODE_WFM]    = "WFM",
    [FM_MODE_ACARS]  = "ACARS",
    [FM_MODE_FLEX]   = "FLEX",
};

/* LS-721: both console implementations carried a six-element positional
   token table after ACARS was inserted into fm_mode_t. Their FM_MODE_COUNT
   loops read past it, and later modes selected the wrong enum values. Keep the
   protocol spelling attached to designated enum entries and count it here. */
static const char *const s_mode_commands[] = {
    [FM_MODE_LISTEN] = "listen",
    [FM_MODE_SCAN]   = "scan",
    [FM_MODE_POCSAG] = "pocsag",
    [FM_MODE_WFM]    = "wfm",
    [FM_MODE_ACARS]  = "acars",
    [FM_MODE_FLEX]   = "flex",
};

#define MODE_LABEL_COUNT (sizeof(s_mode_labels) / sizeof(s_mode_labels[0]))
#define MODE_COMMAND_COUNT (sizeof(s_mode_commands) / sizeof(s_mode_commands[0]))

_Static_assert(MODE_LABEL_COUNT == FM_MODE_COUNT,
               "every FM mode needs an operator-facing label");
_Static_assert(MODE_COMMAND_COUNT == FM_MODE_COUNT,
               "every FM mode needs a console command name");

const char *fm_mode_label(fm_mode_t mode)
{
    size_t index = (size_t)mode;
    if (index >= MODE_LABEL_COUNT || s_mode_labels[index] == NULL)
        return "UNKNOWN";
    return s_mode_labels[index];
}

const char *fm_mode_command_name(fm_mode_t mode)
{
    size_t index = (size_t)mode;
    if (index >= MODE_COMMAND_COUNT || s_mode_commands[index] == NULL)
        return "unknown";
    return s_mode_commands[index];
}

bool fm_mode_parse(const char *text, fm_mode_t *mode)
{
    if (!text || !*text || !mode) return false;

    for (size_t i = 0; i < MODE_COMMAND_COUNT; ++i) {
        const char *candidate = s_mode_commands[i];
        const unsigned char *a = (const unsigned char *)text;
        const unsigned char *b = (const unsigned char *)candidate;
        while (*a && *b && tolower(*a) == tolower(*b)) { ++a; ++b; }
        if (!*a && !*b) {
            *mode = (fm_mode_t)i;
            return true;
        }
    }

    if ((tolower((unsigned char)text[0]) == 'n') &&
        (tolower((unsigned char)text[1]) == 'b') &&
        (tolower((unsigned char)text[2]) == 'f') &&
        (tolower((unsigned char)text[3]) == 'm') && text[4] == '\0') {
        *mode = FM_MODE_LISTEN;
        return true;
    }

    char *end = NULL;
    long numeric = strtol(text, &end, 0);
    if (end == text || *end != '\0' || numeric < 0 || numeric >= FM_MODE_COUNT)
        return false;
    *mode = (fm_mode_t)numeric;
    return true;
}
