#include <string.h>
#include <limits.h>
#include "audio_wav.h"
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24; }
static uint16_t le16(const uint8_t *p) { return p[0] | (uint16_t)p[1]<<8; }
/* Bound chunks by both RIFF extent and actual file. Only signed 16-bit PCM
   reaches the codec; compressed or malformed headers must not divide by zero. */
bool is_wav(FILE *fp, wav_instance *out) {
    if (!fp || !out) return false;
    memset(out, 0, sizeof(*out));
    if (fseek(fp, 0, SEEK_END)) return false;
    long end = ftell(fp);
    if (end < 12 || fseek(fp, 0, SEEK_SET)) return false;
    uint8_t riff[12];
    if (fread(riff,1,12,fp)!=12 || memcmp(riff,"RIFF",4) || memcmp(riff+8,"WAVE",4)) return false;
    uint64_t limit=(uint64_t)le32(riff+4)+8;
    if (limit<12 || limit>(uint64_t)end || limit>LONG_MAX) return false;
    bool have_fmt=false;
    while ((uint64_t)ftell(fp)+8<=limit) {
        uint8_t chunk[8];
        if (fread(chunk,1,8,fp)!=8) return false;
        uint32_t size=le32(chunk+4);
        long start=ftell(fp);
        uint64_t next=(uint64_t)start+size+(size&1);
        if (next>limit) return false;
        if (!memcmp(chunk,"fmt ",4)) {
            uint8_t fmt[16];
            if (size<16 || fread(fmt,1,16,fp)!=16) return false;
            unsigned channels=le16(fmt+2), rate=le32(fmt+4), align=le16(fmt+12);
            if (le16(fmt)!=1 || (channels!=1 && channels!=2) || le16(fmt+14)!=16 ||
                rate<8000 || rate>48000 || align!=channels*2 || le32(fmt+8)!=rate*align) return false;
            out->header.NumChannels=channels; out->header.SampleRate=rate;
            out->header.BitsPerSample=16; out->header.BlockAlign=align;
            have_fmt=true;
        } else if (!memcmp(chunk,"data",4)) {
            if (!have_fmt || size%out->header.BlockAlign) return false;
            out->remaining=size;
            return true;
        }
        if (fseek(fp,(long)next,SEEK_SET)) return false;
    }
    return false;
}
DECODE_STATUS decode_wav(FILE *fp, decode_data *data, wav_instance *wav) {
    if (!fp || !data || !wav || !data->samples) return DECODE_STATUS_ERROR;
    data->frame_count=0;
    unsigned frame=wav->header.BlockAlign;
    if (wav->header.BitsPerSample!=16 || (wav->header.NumChannels!=1 && wav->header.NumChannels!=2) ||
        frame!=(unsigned)wav->header.NumChannels*2 || data->samples_capacity<frame) return DECODE_STATUS_ERROR;
    if (!wav->remaining) return DECODE_STATUS_DONE;
    size_t bytes=(data->samples_capacity/frame)*frame;
    if (bytes>wav->remaining) bytes=wav->remaining;
    if (fread(data->samples,1,bytes,fp)!=bytes) return DECODE_STATUS_ERROR;
    wav->remaining-=bytes;
    data->fmt.channels=wav->header.NumChannels;
    data->fmt.bits_per_sample=16; data->fmt.sample_rate=wav->header.SampleRate;
    data->frame_count=bytes/frame;
    return DECODE_STATUS_CONTINUE;
}
