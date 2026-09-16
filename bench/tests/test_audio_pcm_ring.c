#include "ls_test.h"
#include "audio_pcm_ring.h"
#include <stdint.h>
#include <string.h>

LS_CASE(static_pcm_queue_keeps_its_full_even_capacity)
{
    /* ESP-IDF 5.4.3 static stream storage loses one byte to distinguish full
       and empty. The old 19200-byte argument gave 19199 usable bytes. */
    const size_t payload = 16000 * 600 / 1000 * 2;
    LS_EQ_INT(audio_pcm_storage_bytes(payload) - 1, payload);
    LS_EQ_INT(audio_pcm_write_bytes(512, 19199), 512);
    LS_EQ_INT(audio_pcm_write_bytes(512, 319), 318);
    LS_EQ_INT(audio_pcm_write_bytes(512, 1), 0);
}

LS_CASE(overflow_and_wrap_preserve_pcm_sample_bytes)
{
    enum {PAYLOAD=30};
    uint8_t ring[PAYLOAD+1], expected[PAYLOAD];
    size_t head=0,tail=0,queued=0,reference=0;
    const size_t storage=audio_pcm_storage_bytes(PAYLOAD);
    uint32_t random=12345;unsigned drops=0,wraps=0;
    for(unsigned turn=0;turn<10000;turn++) {
        random=random*1664525u+1013904223u;
        uint8_t pcm[22];
        for(unsigned i=0;i<sizeof(pcm);i+=2){pcm[i]=(uint8_t)turn;pcm[i+1]=(uint8_t)(turn>>8);}
        size_t want=((random>>16)%11+1)*2;
        size_t accepted=audio_pcm_write_bytes(want,storage-1-queued);
        if(accepted<want)drops++;
        LS_CHECK((accepted&1)==0);
        memcpy(expected+reference,pcm,accepted);reference+=accepted;
        for(size_t i=0;i<accepted;i++) {ring[head]=pcm[i];if(++head==storage){head=0;wraps++;}}
        queued+=accepted;
        size_t take=((random>>4)%7)*2;if(take>queued)take=queued;
        for(size_t i=0;i<take;i++){LS_EQ_INT(ring[tail],expected[i]);tail=(tail+1)%storage;}
        queued-=take;reference-=take;memmove(expected,expected+take,reference);
        LS_CHECK(queued==reference && !(queued&1));
    }
    LS_CHECK(drops>100 && wraps>100);
}
