"""Function-level host checks for the P25 scanner IQ read loop."""

import pathlib
import shutil
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
SCANNER = ROOT / 'components/lakeshark/apps/p25/scanner.c'


def extract_read_block(source):
    marker = 'static bool scanner_read_block('
    start = source.index(marker)
    opening = source.index('{', start)
    depth = 0
    for pos in range(opening, len(source)):
        if source[pos] == '{':
            depth += 1
        elif source[pos] == '}':
            depth -= 1
            if depth == 0:
                return source[start:pos + 1]
    raise ValueError('scanner_read_block has no closing brace')


class ScannerReadBlockTest(unittest.TestCase):
    def test_read_outcomes(self):
        compiler = shutil.which('gcc') or shutil.which('cc')
        if compiler is None:
            self.skipTest('gcc or cc is required for the C harness')

        function = extract_read_block(SCANNER.read_text(encoding='utf-8'))
        harness = r'''
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct { int unused; } ls_radio_session_t;
typedef enum { LS_RADIO_OK, LS_RADIO_ERR_TIMEOUT, LS_RADIO_ERR_OTHER } ls_radio_err_t;
typedef struct { ls_radio_err_t error; size_t part; int cancel; } step_t;
static const step_t *steps;
static size_t step_count;
static size_t calls;
static volatile bool running;

static ls_radio_err_t ls_radio_iq_read(ls_radio_session_t *session, uint8_t *iq,
                                        size_t remaining, int timeout, size_t *part)
{
    (void)session; (void)iq; (void)remaining; (void)timeout;
    if (++calls > 20) exit(2);
    step_t step = steps[calls <= step_count ? calls - 1 : step_count - 1];
    *part = step.part;
    if (step.cancel) running = false;
    return step.error;
}

''' + function + r'''

#define OK(n) { LS_RADIO_OK, n, 0 }
#define ZERO OK(0)
#define TIMEOUT { LS_RADIO_ERR_TIMEOUT, 0, 0 }
#define OTHER { LS_RADIO_ERR_OTHER, 0, 0 }
#define CANCEL { LS_RADIO_OK, 0, 1 }

static void check(const char *name, const step_t *sequence, size_t count,
                  size_t bytes, bool expected, size_t expected_calls)
{
    uint8_t iq[8] = {0};
    ls_radio_session_t session = {0};
    steps = sequence; step_count = count; calls = 0; running = true;
    bool actual = scanner_read_block(&session, iq, bytes, &running);
    if (actual != expected || calls != expected_calls) {
        fprintf(stderr, "%s: result=%d calls=%zu, expected=%d calls=%zu\n",
                name, actual, calls, expected, expected_calls);
        exit(1);
    }
}

int main(void)
{
    const step_t zeros[] = { ZERO };
    const step_t timeouts[] = { TIMEOUT };
    const step_t mixed[] = { ZERO, TIMEOUT };
    const step_t partial[] = { OK(2), OK(2) };
    const step_t intermittent[] = { OK(1), ZERO, TIMEOUT, OK(1), OK(2) };
    const step_t other[] = { OTHER };
    const step_t cancel[] = { CANCEL };
    const step_t oversized[] = { OK(5) };
    check("zero-only OK", zeros, 1, 4, false, 10);
    check("TIMEOUT", timeouts, 1, 4, false, 10);
    check("mixed no progress", mixed, 2, 4, false, 10);
    check("partial reads", partial, 2, 4, true, 2);
    check("intermittent progress", intermittent, 5, 4, true, 5);
    check("other error", other, 1, 4, false, 1);
    check("cancellation", cancel, 1, 4, false, 1);
    check("oversized part", oversized, 1, 4, false, 1);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as temp:
            source = pathlib.Path(temp) / 'read_block.c'
            binary = pathlib.Path(temp) / 'read_block'
            source.write_text(harness, encoding='utf-8')
            compiled = subprocess.run(
                [compiler, '-std=c11', '-Wall', '-Wextra', '-Werror',
                 str(source), '-o', str(binary)],
                capture_output=True, text=True, timeout=15, check=False)
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            executed = subprocess.run(
                [str(binary)], capture_output=True, text=True,
                timeout=5, check=False)
            self.assertEqual(executed.returncode, 0, executed.stderr)


if __name__ == '__main__':
    unittest.main()
