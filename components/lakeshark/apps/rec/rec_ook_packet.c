#include "rec_ook_packet.h"
#include <string.h>
size_t rec_ook_packet(rec_ook_packet_t *s,const int32_t *edges,size_t count,
                      uint8_t *out,size_t cap,uint32_t *gap_us)
{
    if(!s || !edges || !out || !cap || cap>255 || !gap_us)return 0;
    memset(out,0,cap);*gap_us=0;
    size_t bit=0;
    while(s->edge<count) {
        int64_t pulse=edges[s->edge];
        uint32_t us=(uint32_t)(pulse<0?-pulse:pulse);
        if(!s->remaining) {
            if(pulse<0 && us>=4000) {*gap_us=us;s->edge++;break;}
            s->remaining=(us+10u)/20u;if(!s->remaining)s->remaining=1;
        }
        size_t n=s->remaining;
        if(n>cap*8-bit)n=cap*8-bit;
        if(pulse>0)for(size_t i=0;i<n;i++)out[(bit+i)/8]|=(uint8_t)(0x80u>>((bit+i)%8));
        bit+=n;s->remaining-=n;
        if(!s->remaining)s->edge++;
        if(bit==cap*8)break;
    }
    return bit;
}
