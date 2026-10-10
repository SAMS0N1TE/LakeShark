#ifndef CALL_ARCHIVE_H
#define CALL_ARCHIVE_H
#include "call_store.h"
typedef enum { CALL_P25, CALL_FM, CALL_P25_P2, CALL_INPUTS } call_input_t;
typedef enum { CALL_OPT_P25, CALL_OPT_FM, CALL_OPT_MIN_MS, CALL_OPT_DAYS,
               CALL_OPT_FILTER, CALL_OPT_UTC_QUARTERS, CALL_OPTIONS } call_option_t;
typedef struct {
    uint64_t total, free, archive;
    bool valid, paused;
} call_archive_storage_t;
void call_archive_storage(call_archive_storage_t *out);
/* SD worker only; host tests supply the card query. */
bool call_archive_card_space(uint64_t *total, uint64_t *free);
void call_archive_init(void);
/* Decoder-owned entry points: no filesystem access or waiting. */
void call_archive_audio(call_input_t input, uint32_t hz, uint32_t tg,
                        uint32_t source, bool clear, const int16_t *pcm, unsigned n);
void call_archive_gate(call_input_t input, uint32_t hz, bool open);
void call_archive_end(call_input_t input, bool discard);
int call_archive_option(call_option_t option);
void call_archive_set_option(call_option_t option, int value);
const char *call_archive_talkgroups(void);
bool call_archive_set_talkgroups(const char *text);
void call_archive_refresh(void);
int call_archive_count(void);
bool call_archive_entry(int index, call_entry_t *entry);
bool call_archive_protect(const char *path,bool protect);
bool call_archive_delete(const char *path);
bool call_archive_busy(void);
unsigned call_archive_peak(void);
unsigned call_archive_drops(void);
unsigned call_archive_errors(void);
#ifdef CALL_ARCHIVE_HOST_TEST
void call_archive_test_load_options(bool (*read)(void *, int, int32_t *), void *context);
bool call_archive_test_pump(const char *root);
unsigned call_archive_test_wakes(void);
#endif
#endif
