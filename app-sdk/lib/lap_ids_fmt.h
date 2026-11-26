/* lap_ids internals shared by the app engine (lap_ids.c) and the firmware's
   name service (components/apps/tui/ls_ble_names.c), so both read and write
   ble-devices.txt by the same rules. Pure: no card, no lap calls. Apps use
   lap_ids.h. */
#ifndef LAP_IDS_FMT_H
#define LAP_IDS_FMT_H
#include "lap_ids.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Hex bytes with optional ':' or '-' between, at most max: how many, or -1. */
int lap_ids_mac_bytes(const char *s, size_t n, uint8_t *out, int max);
/* "aa:bb:cc:dd:ee:ff", shown order. */
void lap_ids_mac_text(const uint8_t m[6], char *out, size_t cap);
/* One ble-devices.txt line, "aa:bb:cc:dd:ee:ff name" (no newline needed):
   the address in shown order and the name, made printable. */
bool lap_ids_device_line(const char *l, size_t len, uint8_t mac[6], char *name, size_t cap);
/* An address whose name may be kept: public (type 0), or static random (top
   bits 11). Resolvable and non-resolvable private addresses change. be:
   shown order. */
bool lap_ids_keepable(const uint8_t be[6], uint8_t type);
/* A name as heard: trimmed and made printable. False: nothing left. */
bool lap_ids_heard_name(const char *in, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
#endif
