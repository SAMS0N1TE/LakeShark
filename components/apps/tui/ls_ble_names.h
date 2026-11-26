/* Bluetooth names for the firmware's own screens (WARDRIVE, SWEEP, FIND):
   the same names the store apps read through app-sdk/lib/lap_ids.h.

     /sdcard/lakeshark/packs/ble-ids.bin   companies, services, appearances,
                                           MAC makers (PACKS installs it;
                                           older bughunt-ids.bin paths too)
     /sdcard/lakeshark/ble-names.txt       the owner's names, "kind key name"
     /sdcard/lakeshark/ble-devices.txt     remembered device names,
                                           "aa:bb:cc:dd:ee:ff name"

   Every call here reads a RAM cache under a short lock and never touches the
   card: a name not cached yet comes back false and is looked up by the name
   worker (a small task, PSRAM stack), so it turns up a frame or two later.
   The worker loads the two text files at first use and reads them again when
   their size or time changes, so a name an app added shows up here. It goes
   to sleep when nothing has asked for a name for a while.

   Addresses are in shown order here ("aa" first, as printed). The address
   type is the radio's: 0 public, 1 random (2/3 their identity forms),
   LS_BLE_NAMES_TYPE_UNKNOWN when the caller never heard it. Tables live in
   PSRAM. */
#ifndef LS_BLE_NAMES_H
#define LS_BLE_NAMES_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LS_BLE_NAMES_TYPE_UNKNOWN 0xFF
#define LS_BLE_NAMES_LEN 32                 /* a name, ended: at most 31 characters */

/* What ls_ble_names_label found. */
enum {
    LS_BLE_LABEL_NONE = 0,                  /* nothing: show the address */
    LS_BLE_LABEL_NAME = 1,                  /* the device's own name, remembered or the owner's */
    LS_BLE_LABEL_GUESS = 2,                 /* who made it or what it offers: company, service, maker */
};

/* Names from the cache; false: not known, or not known yet (asked for now). */
bool ls_ble_names_company(uint16_t company, char *out, size_t cap);
bool ls_ble_names_service(uint16_t uuid16, char *out, size_t cap);
bool ls_ble_names_appearance(uint16_t appearance, char *out, size_t cap);
/* The maker of a public address by its first three bytes; false for a
   locally administered (random) one. */
bool ls_ble_names_maker(const uint8_t mac[6], char *out, size_t cap);
/* The device's own name: an owner name ("device aa:.. name" in
   ble-names.txt) or one remembered in ble-devices.txt. False for a private
   address (type random, top bits not 11). */
bool ls_ble_names_device(const uint8_t mac[6], uint8_t type, char *out, size_t cap);
/* The best words for a device with no name of its own in hand: its
   remembered name, else its company (manufacturer data), else its service,
   else its maker (only for an address known to be public). company /
   uuid16 < 0: the advert had none. */
int  ls_ble_names_label(const uint8_t mac[6], uint8_t type, int company, int uuid16, char *out, size_t cap);

/* Remember a name a device gave (its advert's local name). Same rules as
   lap_ids_device_learn: public or static random addresses only, the owner's
   names win, and a line "aa:bb:cc:dd:ee:ff name" is appended to
   ble-devices.txt only when the name is new or changed. Rate limited: a
   device's name changes at most once a minute, and appends come from a
   small bucket (8, one more every 3 s); a name past it is held in RAM only.
   For a feed or worker task, not the screen: the append itself is done by
   the name worker. 1 kept, 0 not. */
int  ls_ble_names_learn(const uint8_t mac[6], uint8_t type, const char *name);

/* 0 no name file, 1 loading, 2 ready; out gets a line for an info screen. */
int  ls_ble_names_state(char *out, size_t cap);

/* ---- the worker and the bench ---- */
/* One pass of the worker: file checks and loads, lookups, appends. The
   board's name task runs it; the bench calls it. Never on the screen task. */
void ls_ble_names_work(void);
/* Bench only: other files (NULL keeps one), and a clean start. */
void ls_ble_names_test_paths(const char *ids, const char *user, const char *devices);
void ls_ble_names_reset(void);

#ifdef __cplusplus
}
#endif
#endif
