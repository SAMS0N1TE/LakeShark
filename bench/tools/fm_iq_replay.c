/* fm_iq_replay: a capture of 8-bit offset-binary I/Q at 256 kSPS (FM_RTL_RATE)
   through the firmware's NFM demodulator and POCSAG decoders, at each baud
   and in both polarities. Usage: fm_iq_replay <file.iq>   */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fm_state.h"
#include "fm_dsp.h"
#include "pocsag.h"
fm_state_t FM;
/* The harness's log switch, so its own main is not linked in. */
int ls_shim_log_enabled;
int main(int argc, char **argv)
{
    FILE *f = fopen(argv[1], "rb");
    static uint8_t iq[1 << 21];
    size_t n = fread(iq, 1, sizeof(iq), f); fclose(f);
    static fm_dsp_t dsp; fm_dsp_init(&dsp);
    static fm_state_t outs[6];
    pocsag_ctx_t *p[6];
    const int bauds[3] = {512, 1200, 2400};
    for (int i = 0; i < 6; i++) p[i] = pocsag_create(&outs[i], bauds[i % 3]);
    static float demod[4096], neg[4096];
    size_t blocks = 0;
    for (size_t off = 0; off + 16384 <= n; off += 16384, blocks++) {
        int nd = fm_demod_iq(&dsp, iq + off, 16384, demod, 4096);
        for (int i = 0; i < nd; i++) neg[i] = -demod[i];
        for (int i = 0; i < 3; i++) { pocsag_process(p[i], demod, nd); pocsag_process(p[i + 3], neg, nd); }
    }
    printf("%zu bytes, %zu blocks\n", n, blocks);
    for (int i = 0; i < 6; i++)
        printf("baud %4d %s: frames %lu pages %lu cwerr %lu near_min %d near %lu synced %d\n",
               bauds[i % 3], i < 3 ? "normal  " : "inverted",
               (unsigned long)pocsag_n_frames(p[i]), (unsigned long)pocsag_n_pages(p[i]),
               (unsigned long)pocsag_n_cwerr(p[i]), pocsag_near_min(p[i]),
               (unsigned long)pocsag_n_near(p[i]), pocsag_synced(p[i]));
    return 0;
}
