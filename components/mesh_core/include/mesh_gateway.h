#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MESH_MAX_ROUTES 16

typedef struct {
    uint16_t addr;
    uint16_t next_hop;
    uint8_t  hops;
    uint8_t  lq;
    float    rssi;
    float    snr;
    bool     is_nm;
    bool     is_gw;
} mesh_route_t;

typedef struct {
    bool     radio_ok;
    int      state;
    uint16_t node_addr;
    uint16_t nm;
    uint32_t nodes;
    bool     synced;
    uint32_t uptime_s;
    uint32_t route_count;
    uint32_t last_rx_age_s;
    uint32_t rx_total;
    mesh_route_t routes[MESH_MAX_ROUTES];
} mesh_snapshot_t;

/* Bring up the SX1262 + LoRaMesher as a network-manager/gateway. Call once. */
void mesh_gateway_start(void);

/* Read-only mesh state for the GUI (network status + routing table). */
void mesh_gateway_get_snapshot(mesh_snapshot_t *out);

const char *mesh_gateway_state_name(int state);

/* Clear the per-node RSSI history. */
void mesh_gateway_node_db_reset(void);

/* Log the current routing table (a refresh action for the GUI). */
void mesh_gateway_rescan(void);

#define MESH_RSSI_HISTORY 120
uint32_t mesh_gateway_get_rssi_history(uint16_t addr, float *out, uint32_t max);

#ifdef __cplusplus
}
#endif
