/* Run the sub-GHz decoder over Flipper .sub RAW captures.
 *
 *     subdecode <file.sub> [more.sub ...]
 *
 * RAW_Data is already what the decoder wants: signed durations in
 * microseconds, mark positive and space negative. Real captures are the only
 * thing that says whether a decoder reads real transmitters, as opposed to
 * reading a fixture written by the same hand that wrote the decoder.
 */
#include "subghz_pwm.h"
#include "subghz_nrz.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_EDGES 65536

static int32_t g_pulse[MAX_EDGES];

/* RAW_Data lines repeat and continue one capture, so every one is appended. */
static int load_sub(const char *path, uint32_t *freq_out)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    int n = 0;
    char line[8192];
    *freq_out = 0;
    while (fgets(line, sizeof(line), f)) {
        if (!strncmp(line, "Frequency:", 10)) {
            *freq_out = (uint32_t)strtoul(line + 10, NULL, 10);
            continue;
        }
        if (strncmp(line, "RAW_Data:", 9)) continue;
        const char *p = line + 9;
        while (*p && n < MAX_EDGES) {
            char *end = NULL;
            const long v = strtol(p, &end, 10);
            if (end == p) break;
            g_pulse[n++] = (int32_t)v;
            p = end;
        }
    }
    fclose(f);
    return n;
}

static const char *base(const char *path)
{
    const char *a = strrchr(path, '/');
    const char *b = strrchr(path, '\\');
    const char *s = a > b ? a : b;
    return s ? s + 1 : path;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: subdecode <file.sub> [...]\n");
        return 2;
    }

    int decoded = 0, read_ok = 0;
    for (int i = 1; i < argc; i++) {
        uint32_t freq = 0;
        const int n = load_sub(argv[i], &freq);
        if (n <= 0) {
            printf("%-34s  unreadable\n", base(argv[i]));
            continue;
        }
        read_ok++;

        subghz_pwm_t pwm;
        subghz_nrz_t nrz;
        char what[96] = "";
        if (subghz_pwm_decode(g_pulse, n, &pwm)) {
            subghz_pwm_format(&pwm, what, sizeof(what));
            decoded++;
            printf("%-30s %9.4f  PWM  %s x%u\n",
                   base(argv[i]), freq / 1e6, what, pwm.repeats);
        } else if (subghz_nrz_decode(g_pulse, n, &nrz)) {
            subghz_nrz_format(&nrz, what, sizeof(what));
            decoded++;
            printf("%-30s %9.4f  NRZ  %s\n", base(argv[i]), freq / 1e6, what);
        } else {
            printf("%-30s %9.4f  %5d edges  -\n",
                   base(argv[i]), freq / 1e6, n);
        }
    }
    printf("\n%d of %d captures decoded\n", decoded, read_ok);
    return 0;
}
