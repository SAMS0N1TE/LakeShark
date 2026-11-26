/* BUGHUNT: makers that make cameras first. The name file reader is the
   shared app-sdk/lib/lap_ids.c. */
#include "bh_ids.h"
#include <string.h>

static bool word_in(const char *hay, const char *w)
{
    const size_t n = strlen(w);
    for (const char *p = hay; *p; p++) {
        size_t i = 0;
        while (i < n && p[i] && (p[i] | 32) == (w[i] | 32)) i++;
        if (i == n) {
            const bool before = p == hay || !((p[-1] | 32) >= 'a' && (p[-1] | 32) <= 'z');
            const bool after = !((p[n] | 32) >= 'a' && (p[n] | 32) <= 'z');
            if (before && after) return true;
        }
    }
    return false;
}
bool bhid_camera_maker(const char *name)
{
    static const char *const CAM[] = {
        "Hikvision", "Dahua", "Wyze", "Reolink", "Amcrest", "Arlo", "Ring", "Blink", "Lorex", "Swann", "Uniview",
        "Hanwha Vision", "Vivotek", "Ezviz", "Zmodo", "Mobotix", "Avigilon", "Verkada", "GeoVision", "Tiandy", "Sunell",
        "Raysharp", "Milesight", "Axis Communications",
    };
    if (!name || !name[0]) return false;
    for (size_t i = 0; i < sizeof(CAM) / sizeof(CAM[0]); i++) if (word_in(name, CAM[i])) return true;
    return false;
}
