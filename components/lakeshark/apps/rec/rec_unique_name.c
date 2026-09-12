/**/

#include "rec_unique_name.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static int path_exists(const char *dir, const char *name, const char *ext)
{
    char path[192];
    int w = snprintf(path, sizeof(path), "%s/%s%s", dir, name, ext);

    if (w < 0 || (size_t)w >= sizeof(path)) return 1;
    struct stat st;
    return stat(path, &st) == 0;
}

int rec_pick_unique_name(const char *dir, const char *base, const char *ext,
                         char *out, size_t out_len)
{
    if (!dir || !base || !ext || !out || out_len == 0) return -1;
    if (!*base) return -1;

    size_t bl = strlen(base);
    if (bl + 1 > out_len) return -1;

    if (!path_exists(dir, base, ext)) {
        memcpy(out, base, bl + 1);
        return 0;
    }

    for (int i = 1; i <= REC_UNIQUE_MAX_SUFFIX; i++) {
        char cand[80];
        int w = snprintf(cand, sizeof(cand), "%s-%d", base, i);
        if (w < 0 || (size_t)w >= sizeof(cand)) return -1;
        if ((size_t)w + 1 > out_len) return -1;
        if (!path_exists(dir, cand, ext)) {
            memcpy(out, cand, (size_t)w + 1);
            return 0;
        }
    }
    return -1;
}
