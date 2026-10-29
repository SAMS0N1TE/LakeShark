/* The SDR front end's saved settings (radio_frontend.c), held in RAM for
   lssim, which has no NVS. */
#include <stdbool.h>
#include <stdint.h>

uint32_t settings_get_rffe(void);
bool settings_set_rffe(uint32_t v);
uint64_t settings_get_rtl_ppm_table(void);
bool settings_set_rtl_ppm_table(uint64_t v);
void settings_hackrf_ppm_set(int ppm);

static uint32_t g_rffe;
static uint64_t g_ppm;
uint32_t settings_get_rffe(void) { return g_rffe; }
bool settings_set_rffe(uint32_t v) { g_rffe = v; return true; }
uint64_t settings_get_rtl_ppm_table(void) { return g_ppm; }
bool settings_set_rtl_ppm_table(uint64_t v) { g_ppm = v; return true; }
void settings_hackrf_ppm_set(int ppm) { (void)ppm; }
