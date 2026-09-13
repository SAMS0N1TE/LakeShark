/* SPDX-License-Identifier: GPL-3.0-or-later
 * Crypto1 forward cipher adapted from Flipper Zero firmware helpers/crypto1.c,
 * revision 7f0b6e1c14431708cfde75ae1ba13df59e868041 (algorithm from Proxmark3).
 * Adaptation removes allocation/Furi dependencies and attack/recovery routines.
 * Source: https://github.com/flipperdevices/flipperzero-firmware
 * Distributed under the repository's GNU GPL v3-or-later; see LICENSE.
 */
#include "nfc_classic.h"
static unsigned parity(uint32_t v){v^=v>>16;v^=v>>8;v^=v>>4;return (0x6996>>(v&15))&1;}
uint8_t ls_nfc_odd_parity(uint8_t v){return parity(v)^1;}
uint8_t ls_crypto1_filter(uint32_t v){
 unsigned f=(0xf22c0>>(v&15))&16;
 f|=(0x6c9c0>>((v>>4)&15))&8;f|=(0x3c8b0>>((v>>8)&15))&4;
 f|=(0x1e458>>((v>>12)&15))&2;f|=(0x0d938>>((v>>16)&15))&1;
 return (0xec57e80aU>>f)&1;
}
void ls_crypto1_init(ls_crypto1_t *c,uint64_t key){
 c->odd=c->even=0;
 for(int i=47;i>0;i-=2){c->odd=(c->odd<<1)|((key>>((i-1)^7))&1);c->even=(c->even<<1)|((key>>(i^7))&1);}
}
static uint8_t bit(ls_crypto1_t *c,unsigned in){
 uint8_t out=ls_crypto1_filter(c->odd);
 uint32_t next=(c->even<<1)|parity((!!in)^(c->odd&0x29ce5c)^(c->even&0x870804));
 c->even=c->odd;c->odd=next;return out;
}
uint8_t ls_crypto1_byte(ls_crypto1_t *c,uint8_t in){uint8_t out=0;for(unsigned i=0;i<8;i++)out|=bit(c,(in>>i)&1)<<i;return out;}
uint32_t ls_crypto1_word(ls_crypto1_t *c,uint32_t in){uint32_t out=0;for(unsigned i=0;i<32;i++)out|=(uint32_t)bit(c,(in>>(i^24))&1)<<(i^24);return out;}
static uint32_t swap(uint32_t v){return (v>>24)|((v>>8)&0xff00)|((v<<8)&0xff0000)|(v<<24);}
uint32_t ls_crypto1_prng(uint32_t v,unsigned n){v=swap(v);while(n--)v=(v>>1)|((v>>16^v>>18^v>>19^v>>21)<<31);return swap(v);}
uint16_t ls_nfc_crc(const uint8_t *p,size_t n){uint16_t crc=0x6363;while(n--){uint8_t v=*p++^(uint8_t)crc;v^=v<<4;crc=(crc>>8)^((uint16_t)v<<8)^((uint16_t)v<<3)^(v>>4);}return crc;}
unsigned ls_classic_first(unsigned s){return s<32?s*4:128+(s-32)*16;}
unsigned ls_classic_sector(unsigned b){return b<128?b/4:32+(b-128)/16;}
unsigned ls_classic_count(unsigned s){return s<32?4:16;}
uint16_t ls_classic_trailer_mask(const uint8_t t[16],unsigned key_b){
 unsigned c1=t[7]>>4,c2=t[8]&15,c3=t[8]>>4;
 if(key_b || ((t[6]&15)^c1)!=15 || ((t[6]>>4)^c2)!=15 || ((t[7]&15)^c3)!=15)return 0x03c0;
 unsigned access=((c1>>3)<<2)|((c2>>3)<<1)|(c3>>3);
 return access<=2?0xffc0:0x03c0;
}
