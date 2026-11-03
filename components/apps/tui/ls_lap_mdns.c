#include "ls_lap_context.h"
#include "ls_lap_net_audio.h"
#include "ls_lap_features.h"
#include "ls_lap_usage.h"
#if LAP_MDNS
#include <ctype.h>
#if LAP_DEVICE
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "mdns.h"
#endif
#if !LAP_DEVICE
#include <stdlib.h>
static int sim_error;
static bool sim_empty;
void ls_lap_mdns_script(const char *c) {
    if (!strncmp(c, "mdnserror=", 10))
        sim_error = atoi(c + 10);
    if (!strncmp(c, "mdnsempty=", 10))
        sim_empty = atoi(c + 10) != 0;
}
#endif
static atomic_flag lock = ATOMIC_FLAG_INIT;
static unsigned serial;
static LS_LAP_POOL atomic_int p4_canceled_slots[LS_LAP_CONTEXTS][2], p4_published_slots[LS_LAP_CONTEXTS][2];
#define p4_canceled (p4_canceled_slots[ls_lap_context_index()])
#define p4_published (p4_published_slots[ls_lap_context_index()])

static EXT_RAM_BSS_ATTR struct {
    int id, state, error;
    unsigned epoch;
    bool registered;
    lap_mdns_registration_t cfg;
    lap_mdns_service_t results[8];
    unsigned count, read;
#if LAP_DEVICE
    mdns_search_once_t *query;
#endif
} p4_slots_slots[LS_LAP_CONTEXTS][2];
#define p4_slots (p4_slots_slots[ls_lap_context_index()])

static bool label(const char *s) {
    size_t n = strlen(s);
    if (n < 2 || s[0] != '_' || n > 31)
        return false;
    for (size_t i = 1; i < n; i++)
        if (!isalnum((unsigned char)s[i]) && s[i] != '-')
            return false;
    return true;
}
static int start(int index, lap_mdns_registration_t *cfg) {
    int e = ls_lap_av_access(LAP_HW_WIFI, true);
    if (e)
        return e;
    if (!label(cfg->service) || (strcmp(cfg->protocol, "_tcp") && strcmp(cfg->protocol, "_udp")))
        return LAP_IO_EINVAL;
    if (!av_take(&lock))
        return LAP_IO_EBUSY;
    int h = LAP_IO_EBUSY;
    if (!p4_slots[index].id) {
        memset(&p4_slots[index], 0, sizeof(p4_slots[index]));
        serial = serial % 0xfffffff + 1;
        p4_slots[index].id = h = serial * 2 + index;
        atomic_store(&p4_published[index], h);
        p4_slots[index].cfg = *cfg;
        p4_slots[index].epoch = atomic_load(&ls_lap_av_epoch);
    }
    av_give(&lock);
    ls_lap_av_wake();
    return h;
}
int lap_mdns_browse(const char *service, const char *protocol) {
    lap_mdns_registration_t cfg = {0};
    if (!ls_lap_av_string(cfg.service, sizeof(cfg.service), service) ||
        !ls_lap_av_string(cfg.protocol, sizeof(cfg.protocol), protocol))
        return LAP_IO_EINVAL;
    return start(0, &cfg);
}
int lap_mdns_register(const lap_mdns_registration_t *cfg) {
    if (!ls_lap_api_inside() || !ls_lap_api_readable(cfg, sizeof(*cfg)))
        return LAP_IO_EINVAL;
    lap_mdns_registration_t c = *cfg;
    if (c.size != sizeof(c) || c.version != 1 || !c.port || c.reserved ||
        !memchr(c.instance, 0, sizeof(c.instance)) || !memchr(c.service, 0, sizeof(c.service)) ||
        !memchr(c.protocol, 0, sizeof(c.protocol)) || !memchr(c.txt, 0, sizeof(c.txt)) || !c.instance[0])
        return LAP_IO_EINVAL;
    unsigned items = 1;
    for (const char *p = c.txt; *p; p++) {
        if ((unsigned char)*p < 32)
            return LAP_IO_EINVAL;
        if (*p == ';')
            items++;
    }
    if (items > 8)
        return LAP_IO_ELIMIT;
    return start(1, &c);
}
int lap_mdns_next(int h, lap_mdns_service_t *out) {
    int e = ls_lap_av_access(0, true);
    if (e)
        return e;
    if (!ls_lap_api_readable(out, sizeof(*out)))
        return LAP_IO_EINVAL;
    if (!av_take(&lock))
        return LAP_IO_EBUSY;
    int r = LAP_IO_ECLOSED;
    for (int i = 0; i < 2; i++)
        if (p4_slots[i].id == h && p4_slots[i].epoch == atomic_load(&ls_lap_av_epoch) &&
            atomic_load(&p4_canceled[i]) != h) {
            r = p4_slots[i].error ? p4_slots[i].error : p4_slots[i].state == LAP_OPERATION_PENDING ? LAP_IO_EAGAIN : 0;
            if (p4_slots[i].read < p4_slots[i].count) {
                *out = p4_slots[i].results[p4_slots[i].read++];
                r = 1;
            }
            break;
        }
    av_give(&lock);
    return r;
}
void lap_mdns_stop(int h) {
    if (!ls_lap_api_inside())
        return;
    if (h > 0)
        av_close(&p4_canceled[(unsigned)h % 2], &p4_published[(unsigned)h % 2], h);
    ls_lap_av_wake();
}
bool ls_lap_mdns_service(void) {
    if (!av_take(&lock))
        return true;
    bool busy = false;
#if LAP_DEVICE
    static bool initialized;
#endif
    for (int i = 0; i < 2; i++)
        if (p4_slots[i].id) {
            bool closing =
                p4_slots[i].epoch != atomic_load(&ls_lap_av_epoch) || atomic_load(&p4_canceled[i]) == p4_slots[i].id;
#if LAP_DEVICE
            if (closing && p4_slots[i].query) {
                mdns_result_t *result = NULL;
                if (!mdns_query_async_get_results(p4_slots[i].query, 0, &result, NULL)) {
                    busy = true;
                    continue;
                }
                mdns_query_results_free(result);
                mdns_query_async_delete(p4_slots[i].query);
                p4_slots[i].query = NULL;
            }
            if (closing && p4_slots[i].registered)
                mdns_service_remove_for_host(p4_slots[i].cfg.instance, p4_slots[i].cfg.service,
                                             p4_slots[i].cfg.protocol, NULL);
#endif
            if (closing) {
                atomic_store(&p4_published[i], 0);
                memset(&p4_slots[i], 0, sizeof(p4_slots[i]));
                continue;
            }
            if (p4_slots[i].state != LAP_OPERATION_PENDING)
                continue;
            busy = true;
#if LAP_DEVICE
            if (!initialized) {
                if (heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) < 16384) {
                    p4_slots[i].error = LAP_IO_EBUSY;
                    p4_slots[i].state = LAP_OPERATION_DONE;
                    continue;
                }
                if (mdns_init() != ESP_OK) {
                    p4_slots[i].error = LAP_IO_EIO;
                    p4_slots[i].state = LAP_OPERATION_DONE;
                    continue;
                }
                /* Unique per-board suffix; do not collide with rterm's host A name. */
                uint8_t mac[6] = {0};
                if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) {
                    mdns_free();
                    p4_slots[i].state = LAP_OPERATION_DONE;
                    p4_slots[i].error = LAP_IO_EIO;
                    continue;
                }
                char host[32];
                snprintf(host, sizeof(host), "ls-app-%02x%02x%02x", mac[3], mac[4], mac[5]);
                if (mdns_hostname_set(host) != ESP_OK) {
                    mdns_free();
                    p4_slots[i].state = LAP_OPERATION_DONE;
                    p4_slots[i].error = LAP_IO_EIO;
                    continue;
                }
                initialized = true;
            }
            if (i == 0) {
                if (!p4_slots[i].query) {
                    p4_slots[i].query = mdns_query_async_new(NULL, p4_slots[i].cfg.service, p4_slots[i].cfg.protocol,
                                                          MDNS_TYPE_PTR, 2000, 8, NULL);
                    if (!p4_slots[i].query) {
                        p4_slots[i].error = LAP_IO_EIO;
                        p4_slots[i].state = LAP_OPERATION_DONE;
                    }
                    continue;
                }
                mdns_result_t *result = NULL;
                if (!mdns_query_async_get_results(p4_slots[i].query, 0, &result, NULL))
                    continue;
                for (mdns_result_t *p = result; p && p4_slots[i].count < 8; p = p->next) {
                    lap_mdns_service_t *v = &p4_slots[i].results[p4_slots[i].count++];
                    v->size = sizeof(*v);
                    v->version = 1;
                    v->port = p->port;
                    v->ttl = p->ttl;
                    snprintf(v->instance, sizeof(v->instance), "%s",
                             p->instance_name ? p->instance_name : "");
                    snprintf(v->hostname, sizeof(v->hostname), "%s", p->hostname ? p->hostname : "");
                    for (mdns_ip_addr_t *a = p->addr; a; a = a->next)
                        if (a->addr.type == ESP_IPADDR_TYPE_V4) {
                            snprintf(v->ip, sizeof(v->ip), IPSTR, IP2STR(&a->addr.u_addr.ip4));
                            break;
                        }
                    size_t pos = 0;
                    for (size_t j = 0; j < p->txt_count; j++) {
                        int n = snprintf(v->txt + pos, sizeof(v->txt) - pos, "%s%s=%s", pos ? ";" : "",
                                         p->txt[j].key, p->txt[j].value ? p->txt[j].value : "");
                        if (n < 0 || (size_t)n >= sizeof(v->txt) - pos)
                            break;
                        pos += n;
                    }
                }
                mdns_query_results_free(result);
                mdns_query_async_delete(p4_slots[i].query);
                p4_slots[i].query = NULL;
            } else {
                if (mdns_service_exists(p4_slots[i].cfg.service, p4_slots[i].cfg.protocol, NULL))
                    p4_slots[i].error = LAP_IO_ECONFLICT;
                else {
                    char txt[256];
                    strcpy(txt, p4_slots[i].cfg.txt);
                    mdns_txt_item_t items[8];
                    unsigned count = 0;
                    for (char *p = txt; *p && count < 8;) {
                        char *end = strchr(p, ';');
                        if (end)
                            *end = 0;
                        char *eq = strchr(p, '=');
                        if (eq)
                            *eq = 0;
                        items[count++] = (mdns_txt_item_t){p, eq ? eq + 1 : ""};
                        if (!end)
                            break;
                        p = end + 1;
                    }
                    esp_err_t r = mdns_service_add(p4_slots[i].cfg.instance, p4_slots[i].cfg.service,
                                                   p4_slots[i].cfg.protocol, p4_slots[i].cfg.port, items, count);
                    p4_slots[i].registered = r == ESP_OK;
                    if (r != ESP_OK)
                        p4_slots[i].error = LAP_IO_EIO;
                }
            }
#else
            if (sim_error) {
                p4_slots[i].error = sim_error;
                p4_slots[i].state = LAP_OPERATION_DONE;
                continue;
            }
            if (i == 0) {
                lap_mdns_service_t *v = &p4_slots[i].results[0];
                v->size = sizeof(*v);
                v->version = 1;
                v->port = 8080;
                v->ttl = 120;
                strcpy(v->instance, "SIM service");
                strcpy(v->hostname, "sim.local");
                strcpy(v->ip, "192.0.2.3");
                strcpy(v->txt, "fixture=1");
                p4_slots[i].count = sim_empty ? 0 : 1;
            } else
                p4_slots[i].registered = true;
#endif
            p4_slots[i].state = LAP_OPERATION_DONE;
        }
#if LAP_DEVICE
    if (initialized && !p4_slots[0].id && !p4_slots[1].id) {
        mdns_free();
        initialized = false;
    }
#endif
    av_give(&lock);
    return busy;
}

#include "ls_lap_usage.h"
ls_lap_usage_t ls_lap_mdns_usage(unsigned s) {
    ls_lap_usage_t v={sizeof(p4_slots_slots[s]),0};
    for(int i=0;i<2;i++) v.handles+=atomic_load(&p4_published_slots[s][i])>0;
    return v;
}
#else
int lap_mdns_browse(const char *service, const char *protocol) { return LAP_IO_ENOTSUP; }
int lap_mdns_register(const lap_mdns_registration_t *cfg) { return LAP_IO_ENOTSUP; }
int lap_mdns_next(int h, lap_mdns_service_t *out) { return LAP_IO_ENOTSUP; }
void lap_mdns_stop(int h) {}
bool ls_lap_mdns_service(void) { return false; }
ls_lap_usage_t ls_lap_mdns_usage(unsigned slot) { (void)slot; return (ls_lap_usage_t){0}; }
#if !LAP_DEVICE
void ls_lap_mdns_script(const char *command) { (void)command; }
#endif
#endif
