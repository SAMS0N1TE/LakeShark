#ifndef NFC_CLASSIC_H
#define NFC_CLASSIC_H
#include <stdint.h>
#include <stddef.h>
typedef struct { uint32_t odd,even; } ls_crypto1_t;
void ls_crypto1_init(ls_crypto1_t *c,uint64_t key);
uint8_t ls_crypto1_byte(ls_crypto1_t *c,uint8_t in);
uint32_t ls_crypto1_word(ls_crypto1_t *c,uint32_t in);
uint8_t ls_crypto1_filter(uint32_t in);
uint32_t ls_crypto1_prng(uint32_t in,unsigned n);
uint8_t ls_nfc_odd_parity(uint8_t v);
uint16_t ls_nfc_crc(const uint8_t *p,size_t n);
unsigned ls_classic_first(unsigned sector);
unsigned ls_classic_sector(unsigned block);
unsigned ls_classic_count(unsigned sector);
/* Trailer key A is never readable. Key B is data only in specific access modes. */
uint16_t ls_classic_trailer_mask(const uint8_t data[16],unsigned key_b);
#endif
