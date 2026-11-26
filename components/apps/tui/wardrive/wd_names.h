/* What WARDRIVE's screens call a Bluetooth device that has no name of its
   own: the name it gave before, else its company, service or maker
   (ls_ble_names.h: cache reads only, nothing touches the card here). */
#ifndef WD_NAMES_H
#define WD_NAMES_H
#include "wd_core.h"
#include "../ls_ble_names.h"
#include <stdbool.h>
#include <stdio.h>

/* The device's own name when it has one; else, for Bluetooth, what the
   names say (*guess true when it is who made it rather than its name);
   else NULL. buf holds the answer when it is not name. */
static inline const char *wd_label(uint8_t kind, const char *name, const uint8_t addr[6], uint8_t ad_have,
                                   uint8_t ble_type, uint16_t company, uint16_t uuid16, char *buf, size_t cap,
                                   bool *guess)
{
    if (guess) *guess = false;
    if (name && name[0]) return name;
    if (kind != WD_BLE || !cap) return NULL;
    const int r = ls_ble_names_label(addr, ble_type ? (uint8_t)(ble_type - 1) : LS_BLE_NAMES_TYPE_UNKNOWN,
                                     (ad_have & 1) ? company : -1, (ad_have & 2) ? uuid16 : -1, buf, cap);
    if (r == LS_BLE_LABEL_NONE) return NULL;
    if (guess) *guess = r == LS_BLE_LABEL_GUESS;
    return buf;
}
#define WD_DEV_LABEL(e, buf, guess) \
    wd_label((e)->kind, (e)->name, (e)->addr, (e)->ad_have, (e)->ble_type, (e)->company, (e)->uuid16, buf, sizeof(buf), guess)

/* A detail view's lines for a Bluetooth device: each callback gets a key
   ("NAME", "COMPANY", "SERVICE", "LOOKS LIKE", "MAKER") and its value, for
   the names known now. Returns how many. */
typedef void (*wd_names_kv_fn)(void *ctx, const char *key, const char *value);
static inline int wd_names_detail(const wd_dev_t *e, wd_names_kv_fn fn, void *ctx)
{
    if (e->kind != WD_BLE) return 0;
    char v[LS_BLE_NAMES_LEN];
    int n = 0;
    const uint8_t type = e->ble_type ? (uint8_t)(e->ble_type - 1) : LS_BLE_NAMES_TYPE_UNKNOWN;
    if (!e->name[0] && ls_ble_names_device(e->addr, type, v, sizeof(v))) { fn(ctx, "NAME", v); n++; }
    if ((e->ad_have & 1) && ls_ble_names_company(e->company, v, sizeof(v))) { fn(ctx, "COMPANY", v); n++; }
    if ((e->ad_have & 2) && ls_ble_names_service(e->uuid16, v, sizeof(v))) { fn(ctx, "SERVICE", v); n++; }
    if ((e->ad_have & 4) && ls_ble_names_appearance(e->appearance, v, sizeof(v))) { fn(ctx, "LOOKS LIKE", v); n++; }
    if (type != LS_BLE_NAMES_TYPE_UNKNOWN && !(type & 1) && ls_ble_names_maker(e->addr, v, sizeof(v))) { fn(ctx, "MAKER", v); n++; }
    return n;
}
#endif
