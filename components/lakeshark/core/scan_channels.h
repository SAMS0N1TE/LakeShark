#ifndef SCAN_CHANNELS_H
#define SCAN_CHANNELS_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SCAN_MAX_CHANNELS 64
#define SCAN_NAME_LEN     16
#define SCAN_MAX_ZONES    8

typedef enum {
    SCAN_MODE_P25 = 0,
    SCAN_MODE_NFM = 1,
    SCAN_MODE_WFM = 2,
} scan_mode_t;

enum {
    SCAN_FLAG_ENABLED  = 1 << 0,
    SCAN_FLAG_LOCKOUT  = 1 << 1,
    SCAN_FLAG_PRIORITY = 1 << 2,
};

typedef struct {
    char     name[SCAN_NAME_LEN];
    uint32_t freq_hz;
    uint8_t  mode;
    uint8_t  flags;
    uint8_t  zone;
    uint8_t  rsv;
} scan_channel_t;

#define SCANLIST_MAGIC 0x10058F51u
#define SCANLIST_VER   0x0001u
#define SCANLIST_BUILD 0x499858AFu

typedef struct {
    uint32_t magic;
    uint16_t ver;
    uint16_t count;
    uint32_t build;
} scan_store_hdr_t;

void                  scan_channels_init(void);
int                   scan_channels_count(void);
const scan_channel_t *scan_channel_get(int idx);

int   scan_channel_add(const char *name, uint32_t freq_hz, scan_mode_t mode, uint8_t zone);
bool  scan_channel_remove(int idx);
bool  scan_channel_set_lockout(int idx, bool on);
bool  scan_channel_set_priority(int idx, bool on);
bool  scan_channel_set_enabled(int idx, bool on);
/**/
bool  scan_channel_set_name(int idx, const char *name);
/**/
int   scan_channel_find_freq(uint32_t freq_hz);
/**/
int   scan_channel_find_freq_zone(uint32_t freq_hz, uint8_t zone);
void  scan_channels_clear(void);

bool  scan_channels_save(void);

/**/
/* Bulk edits: wrap them so the N individual saves coalesce into ONE commit.
   Every mutator still calls save() internally; batching only defers it.
   batch_end() MUST be reached - an unbalanced begin leaves the list unsaved
   in RAM, which looks exactly like a working edit until the next boot. */
void  scan_channels_batch_begin(void);
bool  scan_channels_batch_end(void);
bool  scan_channels_batching(void);

const char *scan_mode_name(uint8_t mode);
int         scan_mode_parse(const char *s);

#ifdef __cplusplus
}
#endif

#endif
