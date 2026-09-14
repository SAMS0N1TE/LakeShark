#include "scan_sd_store.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#ifdef ESP_PLATFORM
#include "ls_sdcard.h"
#include <unistd.h>
#endif
#ifndef LS_SCAN_SD_ROOT
#define LS_SCAN_SD_ROOT "/sdcard/channels"
#endif

bool scan_sd_available(void)
{
#ifdef ESP_PLATFORM
    return ls_sdcard_mounted();
#else
    struct stat st;
    return stat(LS_SCAN_SD_ROOT, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

bool scan_sd_name(const char *name)
{
    if (!name || !*name || strlen(name) > 32) return false;
    for (const char *p = name; *p; ++p)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '-' || *p == '_')) return false;
    return true;
}

static uint32_t checksum(const void *data, size_t size)
{
    const unsigned char *p = data;
    uint32_t hash = 2166136261u;
    while (size--) hash = (hash ^ *p++) * 16777619u;
    return hash;
}

static int read_file(const char *path, scan_channel_t *rows)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    scan_store_hdr_t h; uint32_t crc;
    bool ok = fread(&h, sizeof(h), 1, f) == 1 && h.magic == SCANLIST_MAGIC &&
              h.ver == SCANLIST_VER && h.build == SCANLIST_BUILD && h.count <= SCAN_MAX_CHANNELS;
    size_t size = ok ? h.count * sizeof(*rows) : 0;
    if (ok) ok = fread(rows, 1, size, f) == size && fread(&crc, sizeof(crc), 1, f) == 1 &&
                 fgetc(f) == EOF && !ferror(f) && crc == checksum(rows, size);
    fclose(f);
    if (ok) for (int i = 0; i < h.count; ++i) {
        const scan_channel_t *c = rows + i;
        if (!memchr(c->name, 0, sizeof(c->name)) || !c->name[0] ||
            c->freq_hz < 1000000 || c->freq_hz > 2000000000 || c->mode > SCAN_MODE_WFM ||
            c->zone >= SCAN_MAX_ZONES || c->lat_e7 < -900000000 || c->lat_e7 > 900000000 ||
            c->lon_e7 < -1800000000 || c->lon_e7 > 1800000000 || c->radius_m > 500000) return -1;
    }
    return ok ? h.count : -1;
}

int scan_sd_read(const char *name, scan_channel_t *rows)
{
    if (!scan_sd_available() || !scan_sd_name(name) || !rows) return -1;
    char path[128];
    snprintf(path, sizeof(path), "%s/%s.bin", LS_SCAN_SD_ROOT, name);
    int count = read_file(path, rows);
    if (count < 0) {
        snprintf(path, sizeof(path), "%s/%s.bak", LS_SCAN_SD_ROOT, name);
        count = read_file(path, rows);
    }
    return count;
}

bool scan_sd_write(const char *name, const scan_channel_t *rows, int count)
{
    if (!scan_sd_available() || !scan_sd_name(name) || !rows || count < 0 || count > SCAN_MAX_CHANNELS) return false;
#ifdef _WIN32
    mkdir(LS_SCAN_SD_ROOT);
#else
    mkdir(LS_SCAN_SD_ROOT, 0775);
#endif
    char target[128], temp[128], backup[128];
    snprintf(target, sizeof(target), "%s/%s.bin", LS_SCAN_SD_ROOT, name);
    snprintf(temp, sizeof(temp), "%s/%s.new", LS_SCAN_SD_ROOT, name);
    snprintf(backup, sizeof(backup), "%s/%s.bak", LS_SCAN_SD_ROOT, name);
    FILE *f = fopen(temp, "wb");
    if (!f) return false;
    scan_store_hdr_t h = {SCANLIST_MAGIC, SCANLIST_VER, (uint16_t)count, SCANLIST_BUILD};
    size_t size = (size_t)count * sizeof(*rows);
    uint32_t crc = checksum(rows, size);
    bool ok = fwrite(&h, sizeof(h), 1, f) == 1 && fwrite(rows, 1, size, f) == size &&
              fwrite(&crc, sizeof(crc), 1, f) == 1 && fflush(f) == 0;
#ifdef ESP_PLATFORM
    if (ok) ok = fsync(fileno(f)) == 0;
#endif
    if (fclose(f)) ok = false;
    if (!ok) { remove(temp); return false; }
    struct stat st;
    bool had_target = stat(target, &st) == 0;
    if (had_target) {
        remove(backup);
        if (rename(target, backup)) { remove(temp); return false; }
    }
    if (rename(temp, target)) {
        if (had_target) rename(backup, target);
        remove(temp); return false;
    }
    return true;
}

void scan_sd_list(void)
{
    if (!scan_sd_available()) { puts("CHPROFILES ERROR SD not mounted"); return; }
    DIR *dir = opendir(LS_SCAN_SD_ROOT);
    if (dir) {
        struct dirent *entry;
        while ((entry = readdir(dir))) {
            size_t n = strlen(entry->d_name);
            if (n > 4 && !strcmp(entry->d_name+n-4, ".bin") && strcmp(entry->d_name, "active.bin"))
                printf("CHPROFILE %.*s\n", (int)n-4, entry->d_name);
        }
        closedir(dir);
    }
    puts("CHPROFILES END");
}
