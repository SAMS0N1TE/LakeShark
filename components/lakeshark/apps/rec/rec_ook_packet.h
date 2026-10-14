#ifndef REC_OOK_PACKET_H
#define REC_OOK_PACKET_H
#include <stddef.h>
#include <stdint.h>
typedef struct { size_t edge, remaining; } rec_ook_packet_t;
/* 20 us chips, MSB first; spaces >=4 ms are returned separately. */
size_t rec_ook_packet(rec_ook_packet_t *s,const int32_t *edges,size_t count,
                      uint8_t *out,size_t cap,uint32_t *gap_us);
#endif
