/* ls_packs: frequency data for where the board is (main/ls_packs.c).

   Packs are text files on the card (/sdcard/lakeshark/packs/<id>.txt; a LANG
   pack is <id>.bin, see LS_PK_LANG) listed
   in the signed catalog https://data.terminalbay.com/v1/catalog.txt (format:
   overhaul PACK_SPEC.md). THE USER INSTALLS packs, when and as many as they
   want; the position (GPS or manual) only picks among the INSTALLED ones, so
   everything below the install calls works with no network. Nothing is
   installed, removed or fetched for a new region by itself. Settings > Auto
   data ("Update installed packs on Wi-Fi", default on) only refreshes packs
   that are already on the card when the catalog has a newer date.

     browse and install:           n = ls_packs_catalog_regions(NULL, 0, 0);
                                   ls_packs_catalog_regions(rows, skip, cap);  country, state, id order
                                   ls_packs_install(ids, n) / ls_packs_remove(ids, n)
                                   ls_packs_install_around(lat, lon, km, kinds)   one-tap "install my area"
                                   ls_packs_refresh_catalog()                     fetch the list (needs Wi-Fi)
                                   ls_packs_status(&st) each frame for progress (state, n of m, got/total)
     use, offline:                 n = ls_packs_nearest(LS_PK_AIR, 80, out, 16);   RAM only
                                   n = ls_packs_freqs(&out[i], f, 32);             one SD seek
     by hand:                      copy pack files to /sdcard/lakeshark/packs; the service checks each one's
                                   sha256 against the signed catalog copy on the card and indexes it.

   ls_packs_nearest and ls_packs_status are safe in a UI frame; the install
   calls only queue work. ls_packs_freqs and ls_packs_scan read the card:
   call them from an action or a worker. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Kinds, as bits; the order is the catalog's. LS_PACK_KIND_NAMES[bit index]. */
#define LS_PK_P25  0x001u
#define LS_PK_DMR  0x002u
#define LS_PK_NXDN 0x004u
#define LS_PK_FM   0x008u   /* analogue land mobile */
#define LS_PK_AIR  0x010u
#define LS_PK_WX   0x020u
#define LS_PK_MAR  0x040u
#define LS_PK_HAM  0x080u
#define LS_PK_BC   0x100u   /* broadcast */
#define LS_PK_ALL  0x1ffu
#define LS_PK_COUNT 9
/* A translator language for an app (catalog kind LANG, id lang-<code>, file
   packs/<id>.bin, the bytes the app reads). It has no region: never picked by
   position, not in LS_PK_ALL, never installed by ls_packs_install_around;
   it is installed or not. Its region row: cc LS_PACK_LANG_CC, sub the code
   in capitals ("PT"), sorted after the global rows, before the countries. */
#define LS_PK_LANG 0x200u
#define LS_PACK_LANG_CC "@@"
const char *ls_pack_kind_name(unsigned index);

typedef enum {
    LS_PACKS_IDLE = 0,  /* nothing asked, nothing known for here */
    LS_PACKS_NO_FIX,    /* no position: no fix, no last fix, no HOME */
    LS_PACKS_NO_WIFI,   /* packs are missing and the network is down */
    LS_PACKS_FETCHING,  /* n of m */
    LS_PACKS_READY,     /* every pack for here is on the card */
    LS_PACKS_FAILED,    /* a download or the catalog failed; retried in 10 min */
    LS_PACKS_EMPTY,     /* no pack is installed: the user picks regions (ls_packs_install*) */
} ls_packs_state_t;

typedef struct {
    ls_packs_state_t state;
    uint8_t  n, m;          /* FETCHING: pack n of m (0 of 0: the catalog) */
    uint8_t  here, have;    /* installed packs in use at the position (here == have) */
    uint16_t installed, updatable;  /* packs on the card; of those, with a newer catalog date */
    uint32_t got, total;    /* bytes of the file on the wire */
} ls_packs_status_t;

typedef struct {
    char     id[16];        /* unique within its pack */
    char     name[32];
    char     pack[32];      /* the pack it came from */
    char     extra[72];     /* k=v;k=v (lic, own, svc, grp, ident, n, ...): ls_pack_extra */
    uint8_t  kind;          /* bit index: 1u << kind is its LS_PK_* */
    float    lat, lon, radius_km;
    float    dist_km;       /* from the position at the time of the call */
    uint32_t offset;        /* of its line in the pack (a hint for ls_packs_freqs) */
} ls_pack_sys_t;

typedef struct {
    uint64_t hz;
    char     role[8];       /* ctl, ch, twr, gnd, atis, wx, mar, ... */
    char     mode[8];       /* P25, P25P2, DMR, NXDN, NFM, FM, WFM, AM */
    char     label[32];
} ls_pack_freq_t;

/* One catalog row for a region picker. cc/sub: country ("US"), US state ("FL");
   "--" for global packs; sub is empty outside the US; LS_PACK_LANG_CC and the
   language for LS_PK_LANG rows. Rows come sorted by cc, sub, id, so a UI
   groups country -> state by watching those change. */
typedef struct {
    char     id[32];
    char     cc[3], sub[4];
    uint16_t kinds;         /* LS_PK_* bits */
    uint32_t bytes;
    float    box[4];        /* west south east north */
    uint8_t  installed;     /* on the card and verified */
    uint8_t  updatable;     /* installed, and the catalog has a newer date */
} ls_pack_region_t;

/* Catalog rows from `skip`, at most cap, into out. With out NULL: how many
   rows there are (0: no signed catalog on the card yet; call
   ls_packs_refresh_catalog). Any task, cheap. */
int  ls_packs_catalog_regions(ls_pack_region_t *out, int skip, int cap);
/* Queue downloads / deletions by pack id (at most 64 each); joins saved Wi-Fi
   for a download. Return how many were queued. Progress: ls_packs_status. */
int  ls_packs_install(const char *const *ids, int n);
int  ls_packs_remove(const char *const *ids, int n);
/* Install every pack of the kinds (0: all) covering lat/lon within km. lat NaN:
   the current position (and the GPS is asked to start). LANG packs never. */
bool ls_packs_install_around(double lat, double lon, float km, uint16_t kinds);
bool ls_packs_refresh_catalog(void);
/* The GET DATA FOR HERE button: ls_packs_install_around(here, 50 km, kinds). */
bool ls_packs_get_here(uint16_t kinds);
void ls_packs_status(ls_packs_status_t *out);
/* "fetching 2 of 5", "ready: 4 packs", "no Wi-Fi", "no fix", ... Returns the state. */
ls_packs_state_t ls_packs_status_text(char *out, size_t cap);

/* Nearest systems of the kinds within within_km, nearest first, from the RAM
   list (the LS_PACKS_NEAR nearest of each kind around the position). */
int ls_packs_nearest(uint16_t kinds, float within_km, ls_pack_sys_t *out, int cap);
#define LS_PACKS_NEAR 32
/* Every system of the kinds within within_km, in pack order, for long lists.
   Reads the packs off the card; return false from fn to stop. */
typedef bool (*ls_packs_scan_fn)(const ls_pack_sys_t *sys, void *ctx);
void ls_packs_scan(uint16_t kinds, float within_km, ls_packs_scan_fn fn, void *ctx);
/* A system's frequencies, in pack order. Returns how many (at most cap). */
int ls_packs_freqs(const ls_pack_sys_t *sys, ls_pack_freq_t *out, int cap);
/* One value out of sys->extra ("grp", "lic", ...). False when absent. */
bool ls_pack_extra(const ls_pack_sys_t *sys, const char *key, char *out, size_t cap);

/* Boot: the location service, the pack task, the loc.key / packs.state values
   and loc.tap / packs.here actions. Then the `fetch` and `packs` console verbs. */
void ls_services_start(void);
void ls_services_console_register(void);

#ifdef __cplusplus
}
#endif
