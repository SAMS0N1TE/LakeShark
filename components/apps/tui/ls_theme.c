/* Generated-by-hand theme table - see ls_theme.h. */

/* Every theme's furniture grey sits at about 2.1:1 on its ground. */

#include "ls_theme.h"
#include <stddef.h>

const ls_tui_theme_t ls_theme_terminal_bay = {
    .name = "Terminal Bay",
    .desc = "deep black, cyan and steel - the default",
    .palette = {
        0x0000,  /* BLACK      */
        0xB249,  /* RED        */
        0x4C6B,  /* GREEN      */
        0xB447,  /* YELLOW     */
        0x3B74,  /* BLUE       */
        0x8AD4,  /* MAGENTA    */
        0x3C73,  /* CYAN       */
        0xAD97,  /* WHITE      */
        0x3A09,  /* BR_BLACK   */
        0xFB4D,  /* BR_RED     */
        0x6F0F,  /* BR_GREEN   */
        0xFE4A,  /* BR_YELLOW  */
        0x6DBF,  /* BR_BLUE    */
        0xDC5F,  /* BR_MAGENTA */
        0x5F1C,  /* BR_CYAN    */
        0xF7BF,  /* BR_WHITE   */
    },
};

const ls_tui_theme_t ls_theme_amber = {
    .name = "Amber",
    .desc = "1970s amber phosphor terminal",
    .palette = {
        0x0000,  /* BLACK      */
        0x89C2,  /* RED        */
        0x8B22,  /* GREEN      */
        0xB3C3,  /* YELLOW     */
        0x6A42,  /* BLUE       */
        0x9A83,  /* MAGENTA    */
        0xB425,  /* CYAN       */
        0xDD0A,  /* WHITE      */
        0x59E1,  /* BR_BLACK   was 0x4981, 1.7:1 */
        0xFBC6,  /* BR_RED     */
        0xFD88,  /* BR_GREEN   */
        0xFE4A,  /* BR_YELLOW  */
        0xCC47,  /* BR_BLUE    */
        0xFCCA,  /* BR_MAGENTA */
        0xFED1,  /* BR_CYAN    */
        0xFF9A,  /* BR_WHITE   */
    },
};

const ls_tui_theme_t ls_theme_phosphor = {
    .name = "Phosphor",
    .desc = "green screen",
    .palette = {
        0x0000,  /* BLACK      */
        0x2BC5,  /* RED        */
        0x2C66,  /* GREEN      */
        0x4D07,  /* YELLOW     */
        0x1B25,  /* BLUE       */
        0x3C4A,  /* MAGENTA    */
        0x2CCD,  /* CYAN       */
        0x7E51,  /* WHITE      */
        0x2264,  /* BR_BLACK   was 0x1A03, 1.8:1 */
        0x5F0B,  /* BR_RED     */
        0x6FEF,  /* BR_GREEN   */
        0xA7F1,  /* BR_YELLOW  */
        0x4ECF,  /* BR_BLUE    */
        0x7FF6,  /* BR_MAGENTA */
        0x8FF9,  /* BR_CYAN    */
        0xE7FC,  /* BR_WHITE   */
    },
};

const ls_tui_theme_t ls_theme_ice = {
    .name = "Ice",
    .desc = "cold blues and violet",
    .palette = {
        0x0000,  /* BLACK      */
        0x7A51,  /* RED        */
        0x3C51,  /* GREEN      */
        0x5C56,  /* YELLOW     */
        0x2AD3,  /* BLUE       */
        0x6AD6,  /* MAGENTA    */
        0x3C76,  /* CYAN       */
        0xB63B,  /* WHITE      */
        0x322B,  /* BR_BLACK   was 0x29C9, 1.8:1 */
        0xCC5F,  /* BR_RED     */
        0x6F1B,  /* BR_GREEN   */
        0x8E5F,  /* BR_YELLOW  */
        0x7E1F,  /* BR_BLUE    */
        0xB51F,  /* BR_MAGENTA */
        0x8F9F,  /* BR_CYAN    */
        0xF7DF,  /* BR_WHITE   */
    },
};

const ls_tui_theme_t ls_theme_synth = {
    .name = "Synthwave",
    .desc = "hot magenta, cyan and a violet dusk",
    .palette = {
        0x0000,  /* BLACK      */
        0xB0AC,  /* RED        */
        0x2CD3,  /* GREEN      */
        0xEB88,  /* YELLOW     */
        0x4013,  /* BLUE       */
        0xB817,  /* MAGENTA    */
        0x2D79,  /* CYAN       */
        0xD69B,  /* WHITE      */
        0x6095,  /* BR_BLACK   was 0x5011, 1.7:1 */
        0xF9F3,  /* BR_RED     */
        0x5FF7,  /* BR_GREEN   */
        0xFEA0,  /* BR_YELLOW  */
        0x94DF,  /* BR_BLUE    */
        0xFA3F,  /* BR_MAGENTA */
        0x5FFF,  /* BR_CYAN    */
        0xFFDF,  /* BR_WHITE   */
    },
};

const ls_tui_theme_t ls_theme_arcade = {
    .name = "Arcade",
    .desc = "saturated primaries on black, like a cabinet",
    .palette = {
        0x0000,  /* BLACK      */
        0xC000,  /* RED        */
        0x0600,  /* GREEN      */
        0xC600,  /* YELLOW     */
        0x001B,  /* BLUE       */
        0xC01B,  /* MAGENTA    */
        0x061B,  /* CYAN       */
        0xC618,  /* WHITE      */
        0x4208,  /* BR_BLACK   */
        0xF9E7,  /* BR_RED     */
        0x2FE7,  /* BR_GREEN   */
        0xFFE7,  /* BR_YELLOW  */
        0x64FF,  /* BR_BLUE    */
        0xFA3F,  /* BR_MAGENTA */
        0x2FFF,  /* BR_CYAN    */
        0xFFFF,  /* BR_WHITE   */
    },
};

/* A vacuum fluorescent display, which is a real thing and not a colour scheme. */

const ls_tui_theme_t ls_theme_vfd = {
    .name = "VFD",
    .desc = "one blue-green glow, the way a real tube does it",
    .palette = {
        0x0000,  /* BLACK      true black behind the glass          */
        0xE307,  /* RED        the glow through a red filter        */
        0x2ED1,  /* GREEN                                           */
        0xCECD,  /* YELLOW     through an amber filter              */
        0x2C77,  /* BLUE                                            */
        0x9BF8,  /* MAGENTA                                         */
        0x2DD4,  /* CYAN       the glow at reading brightness       */
        0xAFFE,  /* WHITE                                           */
        0x1269,  /* BR_BLACK   an unlit segment, faintly there (was 0x0924, 1.3:1) */
        0xFC4B,  /* BR_RED                                          */
        0x7FF6,  /* BR_GREEN                                        */
        0xF7F3,  /* BR_YELLOW                                       */
        0x6E5F,  /* BR_BLUE                                         */
        0xCD5F,  /* BR_MAGENTA                                      */
        0x6FFD,  /* BR_CYAN    full drive - the colour it is known by */
        0xDFFF,  /* BR_WHITE                                        */
    },
};

/* Daylight. */

const ls_tui_theme_t ls_theme_daylight = {
    .name = "Daylight",
    .desc = "black ink on a white ground, for reading in the sun",
    .palette = {
        0xFFFF,  /* BLACK      the ground: white, every pixel lit    */
        0xB0C2,  /* RED         6.8:1                                */
        0x13A5,  /* GREEN       5.8:1                                */
        0x82C0,  /* YELLOW      6.2:1, ochre - yellow cannot be ink  */
        0x2B3C,  /* BLUE        5.1:1                                */
        0x9914,  /* MAGENTA     6.6:1                                */
        0x03B1,  /* CYAN        5.3:1, the status row                */
        0x4A6A,  /* WHITE       8.5:1, slate: the grey that reads    */
        0xB5B7,  /* BR_BLACK    2.0:1, the grey that recedes         */
        0x7061,  /* BR_RED     11.7:1                                */
        0x0A43,  /* BR_GREEN   10.6:1                                */
        0x51C0,  /* BR_YELLOW  10.8:1, values                        */
        0x1A12,  /* BR_BLUE     9.5:1                                */
        0x608D,  /* BR_MAGENTA 11.6:1                                */
        0x024A,  /* BR_CYAN    10.1:1                                */
        0x0000,  /* BR_WHITE   21:1, the ink: black                  */
    },
};

static const ls_tui_theme_t *const THEMES[] = {
    &ls_theme_terminal_bay, &ls_theme_amber,
    &ls_theme_phosphor, &ls_theme_ice,
    &ls_theme_vfd,
    &ls_theme_synth, &ls_theme_arcade,
};

int ls_tui_theme_count(void) { return (int)(sizeof(THEMES) / sizeof(THEMES[0])); }

const ls_tui_theme_t *ls_tui_theme_at(int index)
{
    if (index < 0 || index >= ls_tui_theme_count()) return NULL;
    return THEMES[index];
}

/* See ls_theme.h for all three. */
int ls_tui_theme_index(const ls_tui_theme_t *theme)
{
    if (!theme) return -1;
    for (int i = 0; i < ls_tui_theme_count(); i++)
        if (THEMES[i] == theme) return i;
    return -1;
}

const ls_tui_theme_t *ls_tui_theme_next(const ls_tui_theme_t *cur)
{
    const int at = ls_tui_theme_index(cur);
    return THEMES[at < 0 ? 0 : (at + 1) % ls_tui_theme_count()];
}

const ls_tui_theme_t *ls_tui_theme_effective(const ls_tui_theme_t *chosen,
                                             bool daylight)
{
    if (daylight) return &ls_theme_daylight;
    if (!chosen || chosen == &ls_theme_daylight) return THEMES[0];
    return chosen;
}

/* Compare ignoring case and spaces. Returns 0 when `a` runs out, so the same
   walk answers both "equal" and "is a prefix of". */
static int loose_cmp(const char *a, const char *b, int *a_exhausted)
{
    while (*a || *b) {
        while (*a == ' ') a++;
        while (*b == ' ') b++;
        if (!*a) { *a_exhausted = 1; return *b ? 1 : 0; }
        if (!*b) return -1;
        char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
        char cb = (*b >= 'A' && *b <= 'Z') ? (char)(*b + 32) : *b;
        if (ca != cb) return ca < cb ? -1 : 1;
        a++; b++;
    }
    *a_exhausted = 1;
    return 0;
}

const ls_tui_theme_t *ls_tui_theme_by_name(const char *name)
{
    if (!name || !*name) return NULL;

    const ls_tui_theme_t *prefix_hit = NULL;
    int prefix_count = 0;

    for (int i = 0; i < ls_tui_theme_count(); i++) {
        const ls_tui_theme_t *t = ls_tui_theme_at(i);
        if (!t || !t->name) continue;
        int exhausted = 0;
        int cmp = loose_cmp(name, t->name, &exhausted);
        if (cmp == 0) return t;                 /* exact, ignoring case/space */
        if (exhausted) { prefix_hit = t; prefix_count++; }
    }
    /* An ambiguous prefix is not an answer. */
    return prefix_count == 1 ? prefix_hit : NULL;
}
