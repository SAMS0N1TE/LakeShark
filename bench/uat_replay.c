/* Host replay of 978 MHz complex unsigned 8-bit IQ at 2083333 SPS. */
#include "uat_stream.h"
#include "uat_weather.h"
#include "adsb_state.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static uat_stream_t stream;
static int64_t now(void) { return (int64_t)(stream.stats.samples * 1000000 / UAT_SAMPLE_RATE); }
int64_t esp_timer_get_time(void) { return now(); }
static void down(const uat_fix_t *f, void *user)
{
    (void)user;
    adsb_state_update_uat(f, now());
    printf("UAT %06lX q=%u type=%u pos=%d %.5f %.5f\n", (unsigned long)f->address,
        f->qualifier, f->type, f->position, f->lat, f->lon);
}
static void up(const uint8_t *p, void *user) { (void)user; uat_weather_uplink(p, now()); }
int main(int argc, char **argv)
{
    if (argc != 2) { fprintf(stderr, "usage: uat_replay FILE.cu8 (978 MHz, 2083333 SPS)\n"); return 2; }
    FILE *fp = fopen(argv[1], "rb");
    if (!fp) { perror(argv[1]); return 1; }
    uint8_t buffer[8192];
    adsb_state_init();
    uat_weather_reset();
    uat_stream_init(&stream, down, up, NULL);
    clock_t started = clock();
    size_t n;
    while ((n = fread(buffer, 1, sizeof(buffer), fp))) uat_stream_feed(&stream, buffer, n);
    bool failed = ferror(fp) != 0 || stream.have_i;
    fclose(fp);
    if (stream.have_i) fprintf(stderr, "truncated IQ pair\n");
    uat_weather_text_t text;
    for (int i = 0; i < UAT_WX_TEXT_SLOTS; i++)
        if (uat_weather_text_get(i, now(), &text)) printf("WX %s\n", text.text);
    uat_weather_stats_t w;
    uat_weather_stats(&w);
    printf("samples=%llu short=%lu long=%lu up=%lu bad=%lu corrected=%lu text=%lu blocks=%lu CPU=%.3fs\n",
        (unsigned long long)stream.stats.samples, (unsigned long)stream.stats.short_frames,
        (unsigned long)stream.stats.long_frames, (unsigned long)stream.stats.uplink_frames,
        (unsigned long)stream.stats.bad_frames, (unsigned long)stream.stats.corrected,
        (unsigned long)w.text_records, (unsigned long)w.radar_blocks,
        (double)(clock() - started) / CLOCKS_PER_SEC);
    return failed ? 1 : 0;
}
