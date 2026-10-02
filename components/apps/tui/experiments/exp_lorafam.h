/* Shared bounded node table for the three header monitors. */
#ifndef EXP_LORAFAM_H
#define EXP_LORAFAM_H
#include "../ls_experiments.h"
#include "ls_lora_lr20xx.h"
#include "lorafam_decode.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include <stdio.h>
#include <string.h>
/* A refused start: the chip that is not an LR2021 gets the shared sentence. */
static inline void lf_start_fail(char *why, size_t n, const char *what, esp_err_t err)
{
    if (err == ESP_ERR_NOT_SUPPORTED) snprintf(why, n, "%s", LS_EXP_NEEDS_LR2021);
    else snprintf(why, n, "%s: %s", what, esp_err_to_name(err));
}
#define LF_NODES 12
typedef struct { uint32_t network, id, count; uint16_t types; float rssi; uint8_t kind; } lf_node_t;
typedef struct {
    uint32_t frames, errors, bad, large, phr_count;
    unsigned preset, channel, fec;
    lr20xx_engine_packet_t packet;
    lorafam_mesh_t mesh;
    lorafam_zwave_t zwave;
    lorafam_phr_t phr;
    uint8_t protocol, mac_type;
    bool phr_ok, mac_ok;
    lf_node_t nodes[LF_NODES];
} lf_view_t;
static inline void lf_node_add(lf_view_t *v, uint32_t network, uint32_t id, uint8_t kind,
                               uint8_t type, float rssi)
{
    unsigned slot = LF_NODES;
    for (unsigned i = 0; i < LF_NODES; i++) {
        if (v->nodes[i].count && v->nodes[i].network == network && v->nodes[i].id == id && v->nodes[i].kind == kind) { slot = i; break; }
        if (!v->nodes[i].count && slot == LF_NODES) slot = i;
    }
    if (slot == LF_NODES) {
        slot = 0;
        for (unsigned i = 1; i < LF_NODES; i++) if (v->nodes[i].count < v->nodes[slot].count) slot = i;
        memset(&v->nodes[slot], 0, sizeof(v->nodes[slot]));
    }
    lf_node_t *n = &v->nodes[slot];
    n->network = network; n->id = id; n->kind = kind; n->count++; n->rssi = rssi;
    n->types |= (uint16_t)(1u << (type & 15));
}
#endif
