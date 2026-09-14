/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "cell_report.h"
#include <string.h>
static void put32(uint8_t *p,uint32_t v)
{for(int i=0;i<4;i++)p[i]=(uint8_t)(v>>(8*i));}
bool cell_report_encode(const cell_report_record_t *r,char *text,size_t size)
{
    if(!r || !text || size<49 || r->band>=5 || r->pci < -1 || r->pci>503 ||
       !strchr("QRUDSTLMPCF",r->kind) || !r->kind)return false;
    uint8_t b[32];
    put32(b,r->boot);put32(b+4,r->sequence);put32(b+8,r->epoch);put32(b+12,r->frequency_khz);
    put32(b+16,(uint32_t)r->lat_e5);put32(b+20,(uint32_t)r->lon_e5);
    b[24]=r->changed;b[25]=r->changed>>8;b[26]=(uint16_t)r->pci;b[27]=(uint16_t)r->pci>>8;
    b[28]=(uint8_t)r->rise;b[29]=r->band;b[30]=r->kind;b[31]=r->flags;
    const char *table="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    memcpy(text,"CW1:",4);unsigned o=4;
    for(unsigned i=0;i<32;i+=3) {
        uint32_t v=(uint32_t)b[i]<<16;
        if(i+1<32)v|=(uint32_t)b[i+1]<<8;
        if(i+2<32)v|=b[i+2];
        text[o++]=table[v>>18];text[o++]=table[(v>>12)&63];
        text[o++]=i+1<32?table[(v>>6)&63]:'=';text[o++]=i+2<32?table[v&63]:'=';
    }
    text[o]=0;return true;
}
