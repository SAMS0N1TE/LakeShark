/* The FM receive loop's broadcast extras, as host builds of app_fm.c that
   exercise the LR path see them: SSTV off, weather speaker off, and SAME's
   alert sink (handed to the rx_same plugin, inert on the host). */
#include <stdbool.h>
#include <stdint.h>


bool fm_sstv_wanted(void) { return false; }
void fm_sstv_audio(const int16_t *pcm, int n) { (void)pcm; (void)n; }
void fm_wx_audio(void *dsp, int demod_samples, const int16_t *pcm, int n)
{
    (void)dsp; (void)demod_samples; (void)pcm; (void)n;
}
void fm_wx_on_same(const void *alert, void *user) { (void)alert; (void)user; }
