/* See ls_extnodes.h. PSRAM table under a spinlock: apps write from the screen
   task, MAP reads from the same task, a copy keeps either side short. */
#include "ls_extnodes.h"
#include <string.h>
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
#define LOCK()   portENTER_CRITICAL(&s_lock)
#define UNLOCK() portEXIT_CRITICAL(&s_lock)
#else
#define EXT_RAM_BSS_ATTR
#define LOCK()
#define UNLOCK()
#endif

static EXT_RAM_BSS_ATTR ls_extnode_t s_nodes[LS_EXTNODES_MAX];
static int s_n;

void ls_extnode_put(const ls_extnode_t *n)
{
    if (!n) return;
    LOCK();
    int at = -1, oldest = 0;
    for (int i = 0; i < s_n; i++) {
        if (s_nodes[i].id == n->id && !strncmp(s_nodes[i].src, n->src, sizeof(n->src))) { at = i; break; }
        if (s_nodes[i].heard_us < s_nodes[oldest].heard_us) oldest = i;
    }
    if (at < 0) at = s_n < LS_EXTNODES_MAX ? s_n++ : oldest;
    s_nodes[at] = *n;
    s_nodes[at].name[sizeof(n->name) - 1] = 0;
    s_nodes[at].short_name[sizeof(n->short_name) - 1] = 0;
    s_nodes[at].src[sizeof(n->src) - 1] = 0;
    UNLOCK();
}

int ls_extnodes(ls_extnode_t *out, int max)
{
    if (!out || max <= 0) return 0;
    LOCK();
    const int n = s_n < max ? s_n : max;
    memcpy(out, s_nodes, sizeof(ls_extnode_t) * (size_t)n);
    UNLOCK();
    for (int i = 1; i < n; i++)              /* newest first */
        for (int j = i; j > 0 && out[j].heard_us > out[j - 1].heard_us; j--) {
            ls_extnode_t t = out[j]; out[j] = out[j - 1]; out[j - 1] = t;
        }
    return n;
}

void ls_extnodes_clear(const char *src)
{
    LOCK();
    int k = 0;
    for (int i = 0; i < s_n; i++)
        if (src && strncmp(s_nodes[i].src, src, sizeof(s_nodes[i].src))) s_nodes[k++] = s_nodes[i];
    s_n = k;
    UNLOCK();
}

/* ---- the SDK call: lap_map_node (API 2.6) ----------------------------- */
#include "../../../app-sdk/include/lakeshark_app.h"
#include "ls_lap_api.h"
#ifdef ESP_PLATFORM
#include "esp_timer.h"
#endif

int lap_map_node(const lap_map_node_t *node)
{
    if (!node || node->size < sizeof(lap_map_node_t) || node->version != 1) return LAP_RX_EINVAL;
    ls_extnode_t n;
    memset(&n, 0, sizeof(n));
    memcpy(n.src, "MT", 3);
    n.id = node->id;
    memcpy(n.name, node->name, sizeof(n.name) - 1);
    memcpy(n.short_name, node->short_name, sizeof(n.short_name) - 1);
    n.has_pos = node->has_pos && node->lat >= -90 && node->lat <= 90 && node->lon >= -180 && node->lon <= 180;
    n.lat = node->lat;
    n.lon = node->lon;
    n.rssi_tenths = node->rssi_tenths;
    n.snr_tenths = node->snr_tenths;
    n.hops = node->hops;
#ifdef ESP_PLATFORM
    n.heard_us = esp_timer_get_time();
#endif
    ls_extnode_put(&n);
    return 0;
}
