/* Bluetooth and MAC names off the card, for every app that shows a BLE
   device: company IDs, 16-bit services, characteristics and descriptors,
   vendor 128-bit UUIDs, appearances and MAC makers.

   Two sources, the user's first:
     1. /sdcard/lakeshark/ble-names.txt, a text file of names the owner
        found ("service 18f0 Thermal printer"). Any app can add a line with
        lap_ids_user_add(); the file is plain text and can be edited by hand.
     2. ble-ids.bin, the public registries packed small (Bluetooth SIG,
        Nordic's bluetooth-numbers-database, bleak, Wireshark, LakeShark's
        own list), built by app-sdk/tools/build_ble_ids.py. PACKS puts it at
        /sdcard/lakeshark/packs/ble-ids.bin; older BUGHUNT name files
        (bughunt-ids.bin, /sdcard/bughunt/ids.bin) still work.

   The file format (little endian), three small card reads a name at most:
     header   32 B: "BHID", u16 version 1, u16 tables, u32 pool_off,
              u32 pool_len, u32 built (YYYYMMDD), 12 B zero
     tables   16 B each: u8 kind, u8 key bytes, u16 every, u32 count,
              u32 rec_off, u32 idx_off
     indexes  per table: the key of every `every`-th record
     records  per table, sorted by key: key (1..4 B), u24 name offset
     pool     names, NUL ended, ASCII, at most 31 characters
   A 128-bit UUID's key is lap_ids_uuid_key(): FNV-1a 32 of its 16 bytes in
   the order they are written ("6e400001-b5a3-..." -> 6e 40 00 01 b5 ...).

   The engine never touches the card on the screen task: lap_ids_pump() once
   a frame runs one card job at a time. A name asked for the first time
   comes back NULL and turns up a frame or two later. No allocation. */
#ifndef LAP_IDS_H
#define LAP_IDS_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    LAP_IDS_COMPANY = 1,        /* Bluetooth company ID (manufacturer data) */
    LAP_IDS_SERVICE = 2,        /* 16-bit service UUID, SIG and member */
    LAP_IDS_OUI = 3,            /* MAC maker, 24-bit prefix */
    LAP_IDS_APPEARANCE = 4,
    LAP_IDS_CHAR = 5,           /* 16-bit characteristic UUID */
    LAP_IDS_DESC = 6,           /* 16-bit descriptor UUID */
    LAP_IDS_UUID128 = 7,        /* any 128-bit UUID, keyed by lap_ids_uuid_key */
    LAP_IDS_KINDS = 8,
    LAP_IDS_DEVICE = 9          /* an owner name for one device address (not a file table) */
};
#define LAP_IDS_TABLES 8
#define LAP_IDS_HEAD_MAX (32 + 16 * LAP_IDS_TABLES)
#define LAP_IDS_NAME 32
#define LAP_IDS_IDX_MAX (1u << 20)      /* one table's index, bytes: a real one is a few KB */

#define LAP_IDS_FILE1 "/sdcard/lakeshark/packs/ble-ids.bin"
#define LAP_IDS_FILE2 "/sdcard/lakeshark/packs/bughunt-ids.bin"
#define LAP_IDS_FILE3 "/sdcard/bughunt/ids.bin"
#define LAP_IDS_USER_FILE "/sdcard/lakeshark/ble-names.txt"
#define LAP_IDS_USER_MAX 256            /* user names held */
#define LAP_IDS_USER_BYTES 16384        /* the user file read, at most */

/* ---- the format (pure: no card, no lap calls; the bench tests these) ---- */
typedef struct {
    uint8_t kind, keyb, recb;
    uint16_t every;
    uint32_t count, rec_off, idx_off;
    uint32_t nidx;
    uint8_t *idx;               /* the caller fills it: nidx keys of keyb bytes */
} lap_ids_tab_t;
typedef struct {
    uint32_t pool_off, pool_len, built;
    int nt;
    lap_ids_tab_t t[LAP_IDS_TABLES];
} lap_ids_head_t;

/* Parse the header and table list. 0 bad, 1 good. */
int lap_ids_head(lap_ids_head_t *h, const uint8_t *b, uint32_t n);
lap_ids_tab_t *lap_ids_table(lap_ids_head_t *h, int kind);
/* Where the records that could hold key are: records [*first, *first + *n). */
bool lap_ids_block(const lap_ids_tab_t *t, uint32_t key, uint32_t *first, uint32_t *n);
/* Search a block read from rec_off + first * recb: the name's pool offset, or -1. */
long lap_ids_in_block(const lap_ids_tab_t *t, const uint8_t *recs, uint32_t n, uint32_t key);
/* A name from the pool or a file: made printable, ended. */
void lap_ids_name_fix(char *s, size_t cap);

/* A UUID as written: "180f", "0x180F", "0000180f-0000-1000-8000-00805f9b34fb"
   or 32 hex digits. be[] gets the 16 bytes in written order. *bits 16 or
   128 (a SIG-base 128-bit UUID comes back as 16). False: not a UUID. */
bool lap_ids_parse_uuid(const char *s, uint8_t be[16], int *bits);
/* The 16 bytes of a 16-bit UUID on the SIG base. */
void lap_ids_uuid16_be(uint16_t u, uint8_t be[16]);
/* A 128-bit UUID on the SIG base: its 16-bit value, else -1. */
int lap_ids_uuid_short(const uint8_t be[16]);
/* The UUID128 table's key: FNV-1a 32 over the 16 bytes. */
uint32_t lap_ids_uuid_key(const uint8_t be[16]);
/* "6e400001-b5a3-f393-e0a9-e50e24dcca9e", or "180f" for a SIG-base one. */
void lap_ids_uuid_text(const uint8_t be[16], char *out, size_t cap);

/* ---- user names: "kind key name" lines ---------------------------------
   kind: company, service (or uuid), char, desc, maker (or oui), appearance,
         device.
   key:  a number (company 0x004c or 76, appearance 0x0341), a UUID (16 or
         128-bit, any kind word), a MAC prefix (maker e0:6e:41) or a whole
         address (device e0:6e:41:00:10:62).
   A 128-bit UUID files under LAP_IDS_UUID128 whatever the kind word.
   '#' starts a comment; blank lines are skipped. */
typedef struct {
    uint8_t kind;
    uint32_t key;
    uint8_t mac[6];             /* LAP_IDS_DEVICE: the address, shown order */
    char name[LAP_IDS_NAME];
} lap_ids_user_t;
/* 1 a name, 0 blank or comment, -1 not understood. */
int lap_ids_parse_line(const char *line, size_t len, lap_ids_user_t *out);
/* The kind's word in the file: "service", "char", ... */
const char *lap_ids_kind_word(int kind);

/* ---- the engine (card reads through lap_sd_*) --------------------------- */
void lap_ids_open(void);
void lap_ids_close(void);
void lap_ids_pump(void);                /* once a frame */
/* A name, or NULL: not known yet (asked for now) or not known at all. */
const char *lap_ids_get(int kind, uint32_t key);
/* A service, characteristic or descriptor (kind LAP_IDS_SERVICE, _CHAR or
   _DESC) from its 16 bytes: SIG-base ones by their 16-bit value (a
   characteristic or descriptor missing from its own table falls back to the
   service table, where member UUIDs live), others by their 128-bit key. */
const char *lap_ids_attr(int kind, const uint8_t be[16]);
/* The maker of a public MAC address (NULL for random ones). */
const char *lap_ids_maker(const uint8_t mac_be[6]);      /* mac_be: shown order, aa first */
/* 0 no file, 1 loading, 2 ready; text for an info line. */
int lap_ids_state(char *out, size_t cap);
/* User names loaded (and added since). */
int lap_ids_user_count(void);
/* Name something: held at once (it wins over the file) and a line appended
   to LAP_IDS_USER_FILE. key_text as in the file. 0 ok, -1 not understood,
   -2 the table is full, -3 the card refused the write. */
int lap_ids_user_add(const char *kind_word, const char *key_text, const char *name);

/* ---- remembered device names ---------------------------------------------
   A name a device gave once (its advert's local name, its Device Name
   characteristic) kept by address, so it is still named when it advertises
   without one. Held in RAM (LAP_IDS_DEVICES, oldest replaced) and appended
   to LAP_IDS_DEVICE_FILE as "aa:bb:cc:dd:ee:ff name", which loads at open;
   a later line for the same address wins. Resolvable private addresses
   (random, top bits 01) change every few minutes and are never kept. Owner
   names (lap_ids_user_add with kind "device") win over learned ones. */
#define LAP_IDS_DEVICE_FILE "/sdcard/lakeshark/ble-devices.txt"
#define LAP_IDS_DEVICES 384
/* addr as the radio reports it (little endian: addr[5] is shown first),
   addr_type as lap_ble reports it (0 public, 1 random). */
const char *lap_ids_device(const uint8_t addr[6], uint8_t addr_type);
/* Remember a name heard from the device. Writes only when the name is new
   or changed. 1 kept (new or changed), 0 same as held or not kept. */
int lap_ids_device_learn(const uint8_t addr[6], uint8_t addr_type, const char *name);
int lap_ids_device_count(void);

#ifdef __cplusplus
}
#endif
#endif
