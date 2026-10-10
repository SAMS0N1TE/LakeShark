#ifndef LS_BLE_AD_H
#define LS_BLE_AD_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LS_BLE_AD_MAX 1650
/* Reentrant, allocation-free views of copied AD bytes. No NimBLE storage.
 * Views are borrowed: consume/copy them before the ring callback returns.
 * The length includes the type byte. Zero terminates padding; unknown types
 * are exposed too (flags, names, UUID lists, service and manufacturer data). */
typedef struct { uint8_t type; const uint8_t *data; size_t len; } ls_ble_ad_field_t;
static inline int ls_ble_ad_next(const uint8_t *ad, size_t len, size_t *off,
                                 ls_ble_ad_field_t *field)
{
    if ((!ad && len) || len > LS_BLE_AD_MAX || *off > len) return -1;
    if (*off == len) return 0;
    size_t n = ad[*off];
    if (!n) { *off = len; return 0; }
    if (n > len - *off - 1) return -1;
    field->type = ad[*off + 1];
    field->data = ad + *off + 2;
    field->len = n - 1;
    *off += n + 1;
    return 1;
}
static inline bool ls_ble_ad_valid(const uint8_t *ad, size_t len)
{
    size_t off = 0; ls_ble_ad_field_t field; int rc;
    while ((rc = ls_ble_ad_next(ad, len, &off, &field)) > 0) {}
    return rc == 0;
}
/* Validate the entire envelope, so a name before a truncated tail is ignored.
 * Complete names take precedence over shortened names, irrespective of order. */
static inline const uint8_t *ls_ble_ad_name(const uint8_t *ad, size_t len,
                                           size_t *name_len)
{
    const uint8_t *name = NULL; bool complete = false;
    size_t off = 0; ls_ble_ad_field_t field; int rc;
    *name_len = 0;
    while ((rc = ls_ble_ad_next(ad, len, &off, &field)) > 0) {
        if (field.type == 9 || (field.type == 8 && !complete)) {
            name = field.data; *name_len = field.len;
            complete = field.type == 9;
        }
    }
    if (rc < 0) { *name_len = 0; return NULL; }
    return name;
}
#endif
