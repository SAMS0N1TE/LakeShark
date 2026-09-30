"""Host checks for timing, ordering and restore width in the real P25 sweep.

The C functions are lifted verbatim out of scanner.c and linked against a
deterministic fake clock/radio, so every microsecond total below is an exact
consequence of the source under test rather than a tolerance band.
"""

import pathlib
import shutil
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
SCANNER = ROOT / 'components/lakeshark/apps/p25/scanner.c'

FUNCTIONS = (
    'static bool scanner_read_block(',
    'static unsigned scanner_elapsed_us(',
    'static void scanner_log_timing(',
    'uint32_t scanner_run_sweep(',
)


def extract(source, signature):
    start = source.index(signature)
    opening = source.index('{', start)
    depth = 0
    for pos in range(opening, len(source)):
        depth += (source[pos] == '{') - (source[pos] == '}')
        if depth == 0:
            return source[start:pos + 1]
    raise ValueError(signature)


PREAMBLE = r'''
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>

#define SCAN_IQ_SAMPLES 2
#define SCAN_WATERFALL_ROWS 2
#define MALLOC_CAP_SPIRAM 1
#define pdMS_TO_TICKS(n) (n)

typedef int ls_radio_err_t;
#define LS_RADIO_OK 0
#define LS_RADIO_ERR_TIMEOUT 1
#define LS_RADIO_ERR_INVALID (-1)
typedef struct { int unused; } ls_radio_session_t;

typedef struct {
    bool scanning;
    uint32_t start_freq, stop_freq, step_hz, peak_freq;
    int num_bins, peak_bin, wf_row, sweep_count;
    float power[3], waterfall[2][3], peak_power, noise_floor;
} scan_t;
static scan_t SCAN;
static uint32_t s_tune_freq_hz = 99;

enum { MODE_OK = 0, MODE_RETUNE_FAIL, MODE_READ_FAIL,
       MODE_RESTORE_WIDE, MODE_RESTORE_ERR };

static volatile bool running;
static int mode, alloc_fail, event_count, log_count, bins_seen;
static int64_t now;
static unsigned logged[6];
static char events[64];

static void event(char c) { events[event_count++] = c; events[event_count] = 0; }

/* Advances one microsecond per read, plus whatever the fakes charge, so the
   accumulated totals asserted by the driver are exact. */
static int64_t esp_timer_get_time(void) { return ++now; }

static void *fake_malloc(size_t n)
{ return alloc_fail ? NULL : calloc(n, 1); }
#define malloc(n) fake_malloc(n)

static void *heap_caps_malloc(size_t n, int cap)
{ (void)cap; return fake_malloc(n); }
static void heap_caps_free(void *p) { free(p); }

static void vTaskDelay(int ticks)
{ event(ticks == 1 ? 'y' : 'd'); now += ticks; }

static ls_radio_err_t ls_radio_iq_retune(ls_radio_session_t *s, uint32_t f,
                                         bool sweep, uint64_t *actual)
{
    (void)s;
    event(sweep ? 't' : 'R');
    now += 10;
    if (sweep) {
        *actual = f;
        if (mode == MODE_RETUNE_FAIL && bins_seen++ == 0)
            return LS_RADIO_ERR_TIMEOUT;
        return LS_RADIO_OK;
    }
    /* Restoration writes a full 64-bit value through the caller pointer. */
    *actual = mode == MODE_RESTORE_WIDE ? 0x100000000ULL : (uint64_t)f;
    return mode == MODE_RESTORE_ERR ? LS_RADIO_ERR_TIMEOUT : LS_RADIO_OK;
}

static ls_radio_err_t ls_radio_iq_read(ls_radio_session_t *s, uint8_t *iq,
                                       size_t n, int timeout, size_t *part)
{
    (void)s; (void)timeout;
    event('r');
    now += 20;
    if (mode == MODE_READ_FAIL) {
        running = false;
        *part = 0;
        return LS_RADIO_ERR_INVALID;
    }
    memset(iq, 127, n);
    *part = n;
    return LS_RADIO_OK;
}

static void sys_log(int level, const char *format, ...)
{
    (void)level;
    if (strncmp(format, "Sweep us ", 9) != 0) return;
    if (strlen(format) > 95) { fprintf(stderr, "log format too wide\n"); exit(1); }
    va_list args;
    va_start(args, format);
    for (int i = 0; i < 6; i++) logged[i] = va_arg(args, unsigned);
    va_end(args);
    log_count++;
}
'''

DRIVER = r'''
static void reset(int next_mode)
{
    memset(&SCAN, 0, sizeof(SCAN));
    SCAN.start_freq = 100; SCAN.stop_freq = 120; SCAN.step_hz = 10;
    SCAN.num_bins = 2;
    running = true; mode = next_mode; now = 0;
    event_count = log_count = bins_seen = alloc_fail = 0;
    events[0] = 0;
    memset(logged, 0, sizeof(logged));
}

static int failures;

static void fail(const char *label, const char *what)
{
    fprintf(stderr, "FAIL %s: %s (events=%s)\n", label, what, events);
    failures++;
}

static void expect_u32(const char *label, const char *what,
                       uint32_t got, uint32_t want)
{
    if (got == want) return;
    fprintf(stderr, "FAIL %s: %s got=%u want=%u (events=%s)\n",
            label, what, got, want, events);
    failures++;
}

static void expect_events(const char *label, const char *want)
{ if (strcmp(events, want)) fail(label, want); }

static void expect_timing(const char *label, unsigned r, unsigned d,
                          unsigned c, unsigned y, unsigned t, unsigned x)
{
    const unsigned want[6] = { r, d, c, y, t, x };
    const char *names[6] = { "retune", "read", "compute", "yield",
                             "total", "cancelled" };
    for (int i = 0; i < 6; i++)
        expect_u32(label, names[i], logged[i], want[i]);
    if (logged[4] < logged[0] + logged[1] + logged[2] + logged[3])
        fail(label, "total smaller than the sum of its parts");
}

int main(void)
{
    ls_radio_session_t session = {0};

    reset(MODE_OK);
    expect_u32("success", "return", scanner_run_sweep(&session, &running), 99);
    expect_events("success", "tdrrytdrryRd");
    expect_u32("success", "log count", (uint32_t)log_count, 1);
    expect_timing("success", 33, 82, 2, 27, 157, 0);
    expect_u32("success", "scanning cleared", SCAN.scanning ? 1 : 0, 0);
    expect_u32("success", "sweep counted", (uint32_t)SCAN.sweep_count, 1);

    reset(MODE_RETUNE_FAIL);
    expect_u32("retune fail", "return",
               scanner_run_sweep(&session, &running), 99);
    expect_events("retune fail", "ttdrryRd");
    expect_u32("retune fail", "log count", (uint32_t)log_count, 1);
    expect_timing("retune fail", 33, 41, 1, 19, 103, 0);
    if (!(SCAN.power[0] < -98.0f)) fail("retune fail", "bin not marked bad");

    reset(MODE_READ_FAIL);
    expect_u32("cancel", "return", scanner_run_sweep(&session, &running), 0);
    expect_events("cancel", "tdr");
    expect_u32("cancel", "log count", (uint32_t)log_count, 1);
    expect_timing("cancel", 11, 21, 0, 6, 42, 1);
    expect_u32("cancel", "no restore retune",
               (uint32_t)(strchr(events, 'R') != NULL), 0);
    expect_u32("cancel", "sweep not counted", (uint32_t)SCAN.sweep_count, 0);

    reset(MODE_RESTORE_WIDE);
    expect_u32("wide restore", "return",
               scanner_run_sweep(&session, &running), 0);
    expect_events("wide restore", "tdrrytdrryRd");
    expect_timing("wide restore", 33, 82, 2, 27, 157, 0);

    reset(MODE_RESTORE_ERR);
    expect_u32("restore error", "return",
               scanner_run_sweep(&session, &running), 0);
    expect_u32("restore error", "log count", (uint32_t)log_count, 1);
    expect_timing("restore error", 33, 82, 2, 27, 157, 0);

    reset(MODE_OK);
    alloc_fail = 1;   /* both heap_caps_malloc and malloc fail */
    expect_u32("oom", "return", scanner_run_sweep(&session, &running), 99);
    expect_u32("oom", "log count", (uint32_t)log_count, 0);
    expect_u32("oom", "events", (uint32_t)event_count, 0);
    expect_u32("oom", "scanning cleared", SCAN.scanning ? 1 : 0, 0);

    return failures ? 1 : 0;
}
'''


class ScannerTimingTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.compiler = shutil.which('gcc') or shutil.which('cc')
        cls.source = SCANNER.read_text(encoding='utf-8')
        cls.functions = '\n\n'.join(
            extract(cls.source, marker) for marker in FUNCTIONS)

    def build(self, body, temp):
        if self.compiler is None:
            self.skipTest('gcc or cc is required for the C harness')
        path = pathlib.Path(temp) / 'scan.c'
        binary = pathlib.Path(temp) / 'scan.exe'
        path.write_text(body, encoding='utf-8')
        built = subprocess.run(
            [self.compiler, '-std=c11', '-Wall', '-Wextra', '-Werror',
             str(path), '-lm', '-o', str(binary)],
            capture_output=True, text=True, timeout=60, check=False)
        return built, binary

    def test_sweep_timing_order_and_restore(self):
        with tempfile.TemporaryDirectory() as temp:
            built, binary = self.build(PREAMBLE + self.functions + DRIVER, temp)
            self.assertEqual(built.returncode, 0, built.stderr)
            run = subprocess.run([str(binary)], capture_output=True,
                                 text=True, timeout=30, check=False)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)

    def test_a_32_bit_restore_output_no_longer_compiles(self):
        """The pre-fix source passed a uint32_t* to a uint64_t* parameter."""
        self.assertIn('uint64_t restored_hz', self.functions,
                      'restore output must stay 64-bit wide')
        regressed = self.functions.replace('uint64_t restored_hz',
                                           'uint32_t restored_hz')
        self.assertNotEqual(regressed, self.functions)
        with tempfile.TemporaryDirectory() as temp:
            built, _ = self.build(PREAMBLE + regressed + DRIVER, temp)
            self.assertNotEqual(built.returncode, 0,
                                'narrow restore output must not compile')
            self.assertIn('restored_hz', built.stderr)


if __name__ == '__main__':
    unittest.main()
