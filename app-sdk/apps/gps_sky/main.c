#include <lakeshark_app.h>
#include <lap_canvas.h>
#include <math.h>
#include <stdio.h>
static lap_canvas_t sky;
static int lease;
static bool present;
static void enter(void) {
  lap_caps_t caps;
  lap_caps(&caps, sizeof(caps));
  present = (caps.hardware & LAP_HW_GPS) != 0;
  lease = present ? lap_gps_acquire(0) : 0;
}
static void leave(void) {
  if (lease > 0)
    lap_gps_release(lease);
  lease = 0;
  lap_canvas_free(&sky);
}
static void draw(void) {
  lap_gps_status_t status;
  lap_gps_status(&status);
  lap_gps_satellite_t sats[48];
  int n = present ? lap_gps_satellites(sats, 48) : 0;
  char text[96];
  static const char *states[] = {"not fitted", "off", "acquiring", "fix",
                                 "stale"};
  snprintf(text, sizeof(text), "GPS %s  quality %u  antenna %u",
           states[status.state], status.quality, status.antenna);
  lap_put_str(0, 0, text, LAP_ATTR(LAP_CYAN, LAP_BLACK));
  if (status.fix_age_ms == UINT32_MAX)
    snprintf(text, sizeof(text), "Waiting for first fix; %d in sky", n);
  else
    snprintf(text, sizeof(text), "Fix age %lums; %u used / %u visible",
             (unsigned long)status.fix_age_ms, status.position.sats_used,
             status.position.sats_visible);
  lap_put_str(0, 1, text, LAP_ATTR(LAP_WHITE, LAP_BLACK));
  int w = lap_cols() < 36 ? lap_cols() : 36, h = lap_rows() - 4;
  if (h > 16)
    h = 16;
  if (h < 2 || !lap_canvas_init(&sky, w, h))
    return;
  lc_clear(&sky, LAP_BLACK);
  int cx = sky.w / 2, cy = sky.h / 2, r = cy - 2;
  if (r > cx - 2)
    r = cx - 2;
  lc_circle(&sky, cx, cy, r, LAP_WHITE);
  lc_circle(&sky, cx, cy, r / 2, LAP_BLUE);
  for (int i = 0; i < n; i++) {
    float a = sats[i].azimuth * .0174532925f,
          d = r * (90 - sats[i].elevation) / 90.0f;
    int x = cx + (int)(sinf(a) * d), y = cy - (int)(cosf(a) * d);
    lc_fill_circle(&sky, x, y, 1,
                   sats[i].used  ? LAP_GREEN
                   : sats[i].snr ? LAP_YELLOW
                                 : LAP_BLUE);
  }
  lap_canvas_show(&sky, 0, 3);
  snprintf(text, sizeof(text), "%d sats; green=used yellow=tracked", n);
  lap_put_str(0, lap_rows() - 1, text, LAP_ATTR(LAP_GREEN, LAP_BLACK));
}
LAP_APP(.id = "gps-sky", .name = "GPS SKY",
        .sub = "Acquisition, freshness and sky", .version = "1.0",
        .author = "LakeShark", .icon = LAP_ICON_STAR, .color = LAP_CYAN,
        .enter = enter, .leave = leave, .draw = draw);
