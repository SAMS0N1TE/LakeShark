#include "ls_test.h"
#include "audio_wav.h"
#include <stdlib.h>
static void u32(unsigned char *p,unsigned v){for(int i=0;i<4;i++)p[i]=(unsigned char)(v>>(8*i));}
static FILE *fixture(int channels,int bits,int format,int odd_junk,int trailing) {
    unsigned char b[96]={0}; int fmt=odd_junk?22:12, data=fmt+24, length=data+16+(trailing?12:0);
    memcpy(b,"RIFF",4);u32(b+4,length-8);memcpy(b+8,"WAVE",4);
    if(odd_junk){memcpy(b+12,"JUNK",4);u32(b+16,1);b[20]='x';}
    memcpy(b+fmt,"fmt ",4);u32(b+fmt+4,16);b[fmt+8]=format;b[fmt+10]=channels;
    u32(b+fmt+12,44100);u32(b+fmt+16,44100*channels*(bits/8));b[fmt+20]=channels*(bits/8);b[fmt+22]=bits;
    memcpy(b+data,"data",4);u32(b+data+4,8);for(int i=0;i<8;i++)b[data+8+i]=i+1;
    if(trailing){memcpy(b+data+16,"LIST",4);u32(b+data+20,4);memcpy(b+data+24,"meta",4);}
    FILE *f=ls_test_tmpfile();LS_CHECK(f);fwrite(b,1,length,f);rewind(f);return f;
}
LS_CASE(pcm_stereo_stops_at_data_not_metadata){
    FILE *f=fixture(2,16,1,0,1);wav_instance w;LS_CHECK(is_wav(f,&w));
    uint8_t b[32];memset(b,0xcc,sizeof(b));decode_data d={.samples=b,.samples_capacity=sizeof(b)};
    LS_EQ_INT(decode_wav(f,&d,&w),DECODE_STATUS_CONTINUE);LS_EQ_INT(d.frame_count,2);LS_EQ_INT(b[7],8);LS_EQ_INT(b[8],0xcc);
    LS_EQ_INT(decode_wav(f,&d,&w),DECODE_STATUS_DONE);LS_EQ_INT(d.frame_count,0);fclose(f);
}
LS_CASE(odd_metadata_chunk_is_padded){FILE*f=fixture(1,16,1,1,0);wav_instance w;LS_CHECK(is_wav(f,&w));LS_EQ_INT(w.header.NumChannels,1);fclose(f);}
LS_CASE(reject_invalid_formats_without_division){
    const int cases[][3]={{0,16,1},{3,16,1},{2,0,1},{2,8,1},{2,24,1},{2,16,3}};
    for(unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);i++){FILE*f=fixture(cases[i][0],cases[i][1],cases[i][2],0,0);wav_instance w;LS_CHECK(!is_wav(f,&w));fclose(f);}
}
LS_CASE(reject_truncated_riff_and_oversized_chunk){
    FILE*f=fixture(2,16,1,0,0);wav_instance w;unsigned char x[4];u32(x,0xffffffffu);fseek(f,4,SEEK_SET);fwrite(x,1,4,f);LS_CHECK(!is_wav(f,&w));fclose(f);
    f=fixture(2,16,1,0,0);fseek(f,40,SEEK_SET);fwrite(x,1,4,f);LS_CHECK(!is_wav(f,&w));fclose(f);
}
LS_CASE(short_output_buffer_rejected){FILE*f=fixture(2,16,1,0,0);wav_instance w;LS_CHECK(is_wav(f,&w));uint8_t b[2];decode_data d={.samples=b,.samples_capacity=2};LS_EQ_INT(decode_wav(f,&d,&w),DECODE_STATUS_ERROR);fclose(f);}

LS_CASE(seek_is_frame_aligned_and_bounded_to_data) {
    FILE *f=fixture(2,16,1,1,1);wav_instance w;uint32_t actual=99;
    LS_CHECK(is_wav(f,&w));LS_CHECK(seek_wav(f,&w,UINT32_MAX,&actual));
    LS_EQ_INT(w.remaining,0);LS_EQ_INT(ftell(f),w.data_offset+w.data_bytes);
    LS_CHECK(seek_wav(f,&w,0,&actual));LS_EQ_INT(w.remaining,8);LS_EQ_INT(actual,0);
    uint8_t b[4];decode_data d={.samples=b,.samples_capacity=4};
    LS_EQ_INT(decode_wav(f,&d,&w),DECODE_STATUS_CONTINUE);LS_EQ_INT(b[0],1);fclose(f);
}

LS_CASE(seek_forward_and_backward_start_at_requested_pcm_frame) {
    FILE *f=fixture(2,16,1,0,0);unsigned char n[4];u32(n,36+3528);fseek(f,4,SEEK_SET);fwrite(n,1,4,f);
    u32(n,3528);fseek(f,40,SEEK_SET);fwrite(n,1,4,f);
    for(int frame=0;frame<882;frame++){int16_t pcm[2]={(int16_t)frame,(int16_t)-frame};fwrite(pcm,2,2,f);}
    wav_instance w;LS_CHECK(is_wav(f,&w));uint32_t actual;
    LS_CHECK(seek_wav(f,&w,10,&actual));LS_EQ_INT(actual,10);LS_EQ_INT(w.remaining,1764);
    int16_t pcm[2];decode_data d={.samples=(uint8_t*)pcm,.samples_capacity=4};decode_wav(f,&d,&w);LS_EQ_INT(pcm[0],441);
    LS_CHECK(seek_wav(f,&w,1,&actual));decode_wav(f,&d,&w);LS_EQ_INT(pcm[0],44);fclose(f);
}
