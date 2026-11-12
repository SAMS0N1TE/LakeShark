/* Nodes of meshes the firmware does not run itself (the MESHTASTIC app),
   published by apps so MAP can draw them beside MeshCore's peers. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define LS_EXTNODES_MAX 48

typedef struct {
    char src[4];            /* "MT" */
    uint32_t id;
    char name[24];
    char short_name[6];
    double lat, lon;
    int64_t heard_us;       /* esp_timer time of the last packet */
    int16_t rssi_tenths, snr_tenths;
    uint8_t hops;
    bool has_pos;
} ls_extnode_t;

/* Insert or update by (src, id). Copies. */
void ls_extnode_put(const ls_extnode_t *n);
/* A copy of the table, newest first; returns how many. */
int  ls_extnodes(ls_extnode_t *out, int max);
void ls_extnodes_clear(const char *src);
