
#ifndef RADIO_STREAM_H
#define RADIO_STREAM_H
#ifdef __cplusplus
extern "C" {
#endif

#define STREAM_PACKET_SIZE   4096
#define STREAM_BUFFER_BYTES  (STREAM_PACKET_SIZE * 2)
void radio_stream_start(void);
#ifdef __cplusplus
}
#endif
#endif