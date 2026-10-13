"""Exercise production demod setters with an active LR owner and SDR fallback."""
import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]


def function(source, signature):
    start = source.index(signature)
    opening = source.index('{', start)
    depth = 0
    for pos in range(opening, len(source)):
        depth += (source[pos] == '{') - (source[pos] == '}')
        if not depth:
            return source[start:pos + 1]
    raise ValueError(signature)


class LrDemodGuardTest(unittest.TestCase):
    def test_lr_requests_preserve_dsp_preference_phase2_and_settings(self):
        compiler = shutil.which('gcc') or shutil.which('cc')
        if not compiler:
            raise RuntimeError('Host C compiler required')
        source = (ROOT/'components/lakeshark/apps/p25/app_p25.c').read_text()
        production = '\n'.join(function(source, signature) for signature in (
            'static void p25_apply_demod_mode(',
            'void p25_demod_set_preference(',
            'int p25_demod_get_preference(',
            'demod_mode_t p25_demod_get_active('))
        harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
typedef enum { DEMOD_C4FM, DEMOD_CQPSK, DEMOD_DIFF_4FSK, DEMOD_FSK4_TRACKING } demod_mode_t;
static struct {
    bool lr_iq_active, demod_invert, dsd_has_sync, dsd_nac_valid;
    float demod_gain;
    unsigned channel_generation, dsd_bch_ok_count, p25_tsbk_ok_count;
    uint64_t sync_active_until_us;
} P25;
static struct { int preference; demod_mode_t active; } s_demod_control;
static struct { demod_mode_t mode; } s_dsp;
static struct { int read_idx, write_idx; } s_ring;
static atomic_uint s_decode_tune_generation;
static int dsp_has_signal_lock, p2_calls, init_calls, settings_calls, select_calls, capture_calls, publishes;
static void p25_p2_enable(bool on) { (void)on; ++p2_calls; }
static int p25_demod_preference_normalize(int value) { return value; }
static uint64_t esp_timer_get_time(void) { return 1000; }
static void p25_demod_control_init(void *c, int preference, uint32_t now, uint32_t nids, uint32_t tsbks)
{ (void)c; (void)now; (void)nids; (void)tsbks; ++init_calls; s_demod_control.preference=preference; s_demod_control.active=preference; }
static void settings_set_p25_demod(int preference) { (void)preference; ++settings_calls; }
static const char *p25_demod_control_name(void *c) { (void)c; return "test"; }
static void sys_log(int color, const char *fmt, ...) { (void)color; (void)fmt; }
static float p25_demod_output_gain(demod_mode_t mode, bool inverted) { (void)mode; return inverted ? 9000 : -9000; }
static bool dsp_select_mode(void *dsp, demod_mode_t mode, float gain)
{ (void)dsp; (void)gain; ++select_calls; s_dsp.mode=mode; return true; }
#define P25_IQ_CAPTURE_CONFIG_CHANGED 1
static void p25_iq_capture_interrupt(int reason) { (void)reason; ++capture_calls; }
static void p25_publish_demod(void) { ++publishes; }
''' + production + r'''
int main(void)
{
    s_demod_control.preference=DEMOD_CQPSK;
    s_demod_control.active=DEMOD_CQPSK;
    s_dsp.mode=DEMOD_C4FM;
    s_ring.read_idx=3; s_ring.write_idx=17;
    P25.demod_gain=-9000;
    P25.dsd_has_sync=P25.dsd_nac_valid=true;
    P25.sync_active_until_us=12345;
    dsp_has_signal_lock=1;
    atomic_store(&s_decode_tune_generation, 9);
    P25.lr_iq_active=true;
    for (int mode=DEMOD_C4FM; mode<=DEMOD_FSK4_TRACKING; ++mode) {
        p25_demod_set_preference(mode);
        p25_apply_demod_mode(mode); /* Guard automatic/internal routes too. */
    }
    assert(p25_demod_get_preference()==DEMOD_CQPSK);
    assert(p25_demod_get_active()==DEMOD_C4FM);
    assert(s_dsp.mode==DEMOD_C4FM && P25.demod_gain==-9000);
    assert(!p2_calls && !init_calls && !settings_calls && !select_calls && !capture_calls && !publishes);
    assert(s_ring.read_idx==3 && atomic_load(&s_decode_tune_generation)==9);
    assert(P25.dsd_has_sync && P25.dsd_nac_valid && P25.sync_active_until_us==12345 && dsp_has_signal_lock==1);
    P25.lr_iq_active=false;
    assert(p25_demod_get_active()==DEMOD_CQPSK);
    p25_demod_set_preference(DEMOD_DIFF_4FSK);
    assert(p2_calls==1 && init_calls==1 && settings_calls==1 && select_calls==1);
    assert(capture_calls==1 && publishes==1);
    assert(s_dsp.mode==DEMOD_DIFF_4FSK && p25_demod_get_preference()==DEMOD_DIFF_4FSK);
    assert(s_ring.read_idx==17 && atomic_load(&s_decode_tune_generation)==10);
    assert(!P25.dsd_has_sync && !P25.dsd_nac_valid && !P25.sync_active_until_us && !dsp_has_signal_lock);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            work = pathlib.Path(directory)
            src, exe = work/'guard.c', work/'guard.exe'
            src.write_text(harness)
            subprocess.run([compiler, '-std=c11', '-Wall', '-Werror', str(src), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    unittest.main()
