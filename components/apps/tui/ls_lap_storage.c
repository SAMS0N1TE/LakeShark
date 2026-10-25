#include "ls_lap_storage.h"
#include "ls_lap_manifest.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
static bool backup_name(const char *target, char out[256]) {
    const char *base = strrchr(target, '/');
    if (!base || !base[1])
        return false;
    int n = snprintf(out, 256, "%.*s/.lap-old-%s", (int)(base - target), target, base + 1);
    return n > 0 && n < 256;
}
bool ls_lap_replace(const char *incoming, const char *target) {
    char backup[256];
    struct stat st;
    if (!backup_name(target, backup))
        return false;
    /* Never destroy an unrecovered previous transaction. */
    if (stat(backup, &st) == 0)
        return false;
    bool exists = stat(target, &st) == 0;
    if (exists && (!S_ISREG(st.st_mode) || rename(target, backup)))
        return false;
    if (rename(incoming, target)) {
        if (exists)
            rename(backup, target);
        return false;
    }
    if (exists)
        remove(backup); /* stale backup is harmless and recovered next open */
    return true;
}
bool ls_lap_recover_dir(const char *dir) {
    DIR *d = opendir(dir);
    if (!d)
        return errno == ENOENT;
    struct dirent *e;
    bool ok = true;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, ".lap-old-", 9) || !ls_lap_plain_name(e->d_name + 9, 48))
            continue;
        char target[256], backup[256];
        struct stat st;
        int n = snprintf(target, sizeof(target), "%s/%s", dir, e->d_name + 9);
        if (n < 0 || n >= (int)sizeof(target) || !backup_name(target, backup)) {
            ok = false;
            continue;
        }
        if (stat(target, &st) == 0) {
            if (remove(backup))
                ok = false;
        } else if (rename(backup, target))
            ok = false;
    }
    closedir(d);
    return ok;
}
