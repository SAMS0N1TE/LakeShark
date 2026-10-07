/* Radio fixtures exercised together through the production simulator. */
#include "ls_test.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define popen _popen
#define pclose _pclose
#endif

static char output[32768];

static void run(const char *app, const char *options, bool landscape,
                const char *tone)
{
#ifdef _WIN32
    _putenv_s("LSSIM_DMR", "1");
    _putenv_s("LSSIM_TONE", tone);
    _putenv_s("LSSIM_RS41", "1");
    _putenv_s("LSSIM_LORA", "lr2021");
#else
    setenv("LSSIM_DMR", "1", 1);
    setenv("LSSIM_TONE", tone, 1);
    setenv("LSSIM_RS41", "1", 1);
    setenv("LSSIM_LORA", "lr2021", 1);
#endif
    char command[1200];
    snprintf(command, sizeof(command), "\"%s\" %s -o \"%s\" -d %s %s 2>&1",
             LSSIM_EXE, app, RADIO_RENDER, landscape ? "-l" : "", options);
#ifdef _WIN32
    /* cmd.exe needs native separators and outer quotes around this command. */
    for (char *c = command + 1; *c && *c != '"'; ++c)
        if (*c == '/') *c = '\\';
    char wrapped[1204];
    snprintf(wrapped, sizeof(wrapped), "\"%s\"", command);
    FILE *pipe = popen(wrapped, "r");
#else
    FILE *pipe = popen(command, "r");
#endif
    LS_CHECK(pipe != NULL);
    if (!pipe) return;
    size_t used = 0;
    char line[512];
    while (fgets(line, sizeof(line), pipe)) {
        size_t n = strlen(line);
        if (used + n < sizeof(output)) {
            memcpy(output + used, line, n);
            used += n;
        }
    }
    output[used] = 0;
    int rc = pclose(pipe);
    LS_CHECK_MSG(rc == 0, "simulator failed (%d): %s", rc, output);
#ifdef _WIN32
    _putenv_s("LSSIM_DMR", "");
    _putenv_s("LSSIM_TONE", "");
    _putenv_s("LSSIM_RS41", "");
    _putenv_s("LSSIM_LORA", "");
#else
    unsetenv("LSSIM_DMR");
    unsetenv("LSSIM_TONE");
    unsetenv("LSSIM_RS41");
    unsetenv("LSSIM_LORA");
#endif
}

LS_CASE(fm_tone_fixture_survives_with_dmr_enabled)
{
    for (int wide = 0; wide < 2; ++wide) {
        run("fm", "", wide, "13");
        LS_CHECK(strstr(output, "TONE"));
        LS_CHECK(strstr(output, "100.0 Hz"));
        run("fm", "", wide, "258");
        LS_CHECK(strstr(output, "D754I"));
    }
}

LS_CASE(dmr_fixture_survives_with_fm_tone_enabled)
{
    for (int wide = 0; wide < 2; ++wide) {
        run("p25", "-k 6", wide, "13");
        LS_CHECK(strstr(output, "TG 2051"));
        LS_CHECK(strstr(output, "SRC 1234567"));
        LS_CHECK(strstr(output, "CLEAR"));
        LS_CHECK(strstr(output, "TG 16777215"));
        LS_CHECK(strstr(output, "SRC 1234568"));
        LS_CHECK(strstr(output, "ENCRYPTED / MUTED"));
    }
}

LS_CASE(sonde_fixture_survives_with_dmr_and_fm_tone_enabled)
{
    for (int wide = 0; wide < 2; ++wide) {
        run("experiments", "-k h", wide, "13");
        LS_CHECK(strstr(output, "SONDES / RX ONLY"));
        LS_CHECK(strstr(output, "N1234560"));
        run("map", "", wide, "13");
        LS_CHECK(strstr(output, "N123456"));
        LS_CHECK(strstr(output, "RS41"));
        LS_CHECK(strstr(output, "UAL442"));
    }
}
