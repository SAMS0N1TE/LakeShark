#include "adsb_app.h"
#include "uat_weather.h"
#include "esp_console.h"
#include "esp_timer.h"
#include <stdio.h>
#include <string.h>

static int uat_command(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "start")) {
        bool ok = adsb_uat_start();
        printf("UAT: %s\n", ok ? "start requested, RTL 978 MHz at 2083333 SPS" : "ADS-B app unavailable");
        return ok ? 0 : 1;
    }
    if (argc == 2 && !strcmp(argv[1], "stop")) {
        adsb_uat_stop();
        printf("UAT: stop requested, aircraft session releases both radios\n");
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "stats")) {
        uat_stats_t s;
        uat_weather_stats_t w;
        adsb_uat_stats(&s);
        uat_weather_stats(&w);
        printf("UAT: %s (%s) samples=%llu sync_dn=%lu sync_up=%lu short=%lu long=%lu up=%lu bad=%lu corrected=%lu duplicates=%lu\n",
            adsb_active_source_name(), adsb_receiver_status(), (unsigned long long)s.samples,
            (unsigned long)s.sync_down, (unsigned long)s.sync_up, (unsigned long)s.short_frames,
            (unsigned long)s.long_frames, (unsigned long)s.uplink_frames, (unsigned long)s.bad_frames,
            (unsigned long)s.corrected, (unsigned long)s.duplicates);
        printf("FIS-B: APDU=%lu text=%lu radar=%lu malformed=%lu unsupported=%lu truncated=%lu\n",
            (unsigned long)w.apdus, (unsigned long)w.text_records, (unsigned long)w.radar_blocks,
            (unsigned long)w.malformed, (unsigned long)w.unsupported, (unsigned long)w.truncated);
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "wx")) {
        uat_weather_text_t r;
        int count = 0;
        int64_t now = esp_timer_get_time();
        for (int i = 0; i < UAT_WX_TEXT_SLOTS; i++)
            if (uat_weather_text_get(i, now, &r)) {
                printf("[%llds received] %s\n", (long long)((now - r.received_us) / 1000000), r.text);
                count++;
            }
        if (!count) printf("UAT: no fresh text products\n");
        uat_ground_station_t g;
        for (int i = 0; i < UAT_WX_STATION_SLOTS; i++)
            if (uat_weather_station_get(i, now, &g))
                printf("Ground site=%u slot=%u position=%s %.5f %.5f\n", g.site, g.slot,
                    g.position_valid ? "valid" : "unavailable", (double)g.lat, (double)g.lon);
        return 0;
    }
    printf("uat start | stop | stats | wx\n");
    return 1;
}

void uat_console_register(void)
{
    const esp_console_cmd_t cmd = {.command = "uat", .help = "RTL UAT: start, stop, stats, wx",
        .hint = "start|stop|stats|wx", .func = uat_command, .argtable = NULL};
    esp_console_cmd_register(&cmd);
}
