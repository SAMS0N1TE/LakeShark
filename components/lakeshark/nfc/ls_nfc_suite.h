#ifndef LS_NFC_SUITE_H
#define LS_NFC_SUITE_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
enum { LS_CARD_UNREAD,LS_CARD_ACTIVE,LS_CARD_READ,LS_CARD_AUTH_FAILED,LS_CARD_ERROR,LS_CARD_NAK };
typedef struct {
 bool busy,identified,reading;uint8_t uid[10],uid_len,sak,sectors;
 uint16_t atqa,blocks,read_blocks,current;uint32_t attempts,auth_ok,errors,naks;
 uint8_t block_state[256],sector_keys[40];
 char status[96];
} ls_nfc_suite_status_t;
bool ls_nfc_suite_start(bool read);
void ls_nfc_suite_stop(void);
bool ls_nfc_suite_busy(void);
void ls_nfc_suite_snapshot(ls_nfc_suite_status_t *out);
bool ls_nfc_suite_block(unsigned block,uint8_t out[16],uint16_t *known);
bool ls_nfc_suite_key(bool key_b,const char *hex);
bool ls_nfc_suite_save(void);
bool ls_nfc_suite_sector_key(unsigned sector,bool key_b,const char *hex);
/* Called only by the MIX-RF SPI owner. Exchange uses LSB-first packed bits. */
typedef struct {
 bool (*begin)(void);
 void (*end)(void);
 int (*exchange)(const uint8_t*,unsigned,uint8_t*,unsigned,unsigned*,bool);
} ls_nfc_suite_io_t;
void ls_nfc_suite_step(const ls_nfc_suite_io_t *io);
#ifdef __cplusplus
}
#endif
#endif
