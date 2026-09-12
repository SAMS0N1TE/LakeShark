/* Real OP25 encoder + production C decoder shim. Synthetic voiced harmonics,
 * not intelligible speech or RF; verifies actual PCM, NOT speaker wiring. */
extern "C" {
#include "ls_test.h"
#include "imbe_shim.h"
}
#include "imbe.h"
#include "imbe_vocoder.h"
#include <cmath>
#include <cstdint>
#include <cstdio>
LS_CASE(real_imbe_encoder_and_decoder_produce_nonzero_bounded_pcm)
{
    imbe_vocoder encoder;
    int16_t input[160];
    struct { uint32_t before; int16_t pcm[160]; uint32_t after; } output;
    uint8_t bits[11];
    uint64_t energy=0; unsigned nonzero=0;
    output.before=0x12345678; output.after=0xabcdef01;
    imbe_shim_init();
    for (int frame=0; frame<60; ++frame) {
        for (int i=0; i<160; ++i) {
            double t=(frame*160+i)/8000.0;
            input[i]=(int16_t)(6000*std::sin(2*3.141592653589793*125*t)+
                3000*std::sin(2*3.141592653589793*250*t));
            output.pcm[i]=0;
        }
        encoder.encode_4400(input,bits);
        imbe_shim_decode_88(bits,output.pcm);
        LS_EQ_UINT(output.before,0x12345678); LS_EQ_UINT(output.after,0xabcdef01);
        for (int i=0; i<160; ++i) {
            int32_t s=output.pcm[i]; energy+=(uint64_t)((int64_t)s*s);
            if (s) ++nonzero;
        }
    }
    std::printf("Real IMBE: 60 x160 PCM samples; nonzero=%u energy=%llu\n",
        nonzero,(unsigned long long)energy);
    LS_CHECK(nonzero>160); LS_CHECK(energy>1000000);
}
