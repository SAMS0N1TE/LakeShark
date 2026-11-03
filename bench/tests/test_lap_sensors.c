/* API 2.5 production queues/services, deterministic clock and on-card formats.
 */
#include "core/ls_rlog.h"
#include "core/ls_track.h"
#include "ls_lap_api.h"
#include "ls_lap_net_audio.h"
#include "ls_lap_sensors.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#define mkdir_one(p) _mkdir(p)
#else
#define mkdir_one(p) mkdir(p, 0777)
#endif
static uint32_t tick;
static uint32_t clock_now(void) { return tick; }
extern uint32_t (*ls_lap_api_host_clock)(void);
static void pump(void) { ls_lap_sys_service(); }
static void begin(void) {
  ls_lap_api_begin("sensor-test", 1, 0);
  ls_lap_sys_host()->hardware =
      LAP_HW_GPS | LAP_HW_IMU | LAP_HW_SD | LAP_HW_DESKTOP;
  ls_lap_sys_host()->motion = true;
  ls_lap_sys_host()->gps = true;
  ls_lap_sys_begin();
  ls_lap_api_callback(true);
}
static void gps(void) {
  lap_gps_status_t g;
  lap_gps_satellite_t sat[48];
  assert(lap_gps_status((void *)1) == LAP_RX_EINVAL);
  assert(lap_gps_acquire(1) == LAP_RX_EINVAL);
  assert(!lap_gps_status(&g) && g.state == LAP_GPS_OFF &&
         g.fix_age_ms == UINT32_MAX);
  int h = lap_gps_acquire(0);
  assert(h > 0 && lap_gps_acquire(0) == h);
  pump();
  assert(!lap_gps_status(&g) && g.state == LAP_GPS_ACQUIRING);
  assert(ls_lap_sensors_script("gpsfix=42.3,-71.2,18"));
  assert(ls_lap_sensors_script("satellite=3,1,45,180,38,1"));
  assert(ls_lap_sensors_script("satellite=7,3,30,90,20,0"));
  assert(!lap_gps_status(&g) && g.state == LAP_GPS_FIX && g.quality == 1 &&
         g.position.sats_used == 1);
  assert(lap_gps_satellites(sat, 48) == 2 && sat[0].snr == 38 && sat[0].used &&
         sat[1].constellation == 3);
  tick += 2999;
  assert(!lap_gps_status(&g) && g.state == LAP_GPS_FIX && g.fix_age_ms == 2999);
  tick++;
  assert(!lap_gps_status(&g) && g.state == LAP_GPS_STALE && !g.position.fix &&
         g.position.lat > 42);
  assert(lap_gps_satellites(sat, 48) == 0);
  lap_gps_release(h + 1);
  assert(lap_gps_acquire(0) == h);
  lap_gps_release(h);
  pump();
  assert(!lap_gps_status(&g) && g.state == LAP_GPS_OFF && !g.leased);
  int newer = lap_gps_acquire(0);
  assert(newer > h);
  lap_gps_release(h);
  pump();
  assert(!lap_gps_status(&g) && g.leased);
  lap_gps_release(newer);
  pump();
  ls_lap_sys_host()->hardware = 0;
  ls_lap_sys_host()->gps = false;
  assert(lap_gps_acquire(0) == LAP_RX_ENODEV && !lap_gps_status(&g) &&
         g.state == LAP_GPS_ABSENT);
  ls_lap_sys_host()->hardware =
      LAP_HW_GPS | LAP_HW_IMU | LAP_HW_SD | LAP_HW_DESKTOP;
}
static void motion(void) {
  lap_motion_status_t s;
  lap_motion_sample_t m;
  assert(lap_motion_subscribe(1, 49) == LAP_RX_ERANGE);
  assert(lap_motion_subscribe(128, 100) == LAP_RX_EINVAL);
  ls_lap_sys_host()->m = (lap_motion_t){
      .ax = .25f, .az = 1, .heading = -1, .true_heading = -1, .age_ms = 200};
  assert(!lap_motion_subscribe(31, 100));
  assert(lap_motion_next(&m) == 1 && !(m.fields & LAP_MOTION_MAG));
  assert(m.raw_accel[0] == .25f && m.heading_sigma_deg == -1 &&
         m.age_ms == 200);
  assert(lap_motion_next(&m) == 0);
  tick += 99;
  assert(lap_motion_next(&m) == 0);
  tick++;
  assert(lap_motion_next(&m) == 1 && m.sequence == 2);
  assert(ls_lap_sensors_script("motionquality=6,-1"));
  assert(!lap_motion_status(&s) &&
         s.quality == (LAP_MOTION_DISTURBED | LAP_MOTION_LOW_CONFIDENCE) &&
         s.period_ms == 100);
  ls_lap_sys_host()->motion = false;
  ls_lap_sys_host()->hardware &= ~LAP_HW_IMU;
  tick += 100;
  assert(lap_motion_next(&m) == LAP_RX_ENODEV);
  assert(!lap_motion_status(&s) && !s.available && s.age_ms == UINT32_MAX);
  assert(!lap_motion_subscribe(0, 0));
  assert(lap_motion_next(&m) == LAP_RX_EINVAL);
}
static void input(void) {
  lap_input_event2_t e;
  lap_input_device_state_t s;
  lap_input_device_t d[8];
  lap_input_state_t old;
  while (lap_input_next2(&e, sizeof(e)) > 0) {
  }
  assert(lap_input_next2(&e, sizeof(e) - 1) == LAP_RX_EINVAL);
  assert(ls_lap_sensors_script("device=100,4,1"));
  assert(ls_lap_sensors_script(
      "gamepad=100,-32767,32767,0,10,32767,0,2147483648,-1,1"));
  assert(lap_input_next2(&e, sizeof(e)) == 1 &&
         e.event.kind == LAP_INPUT_DEVICE_CONNECTED);
  assert(lap_input_next2(&e, sizeof(e)) == 1 &&
         e.event.kind == LAP_INPUT_GAMEPAD && e.device == 100 &&
         e.gamepad_buttons == 0x80000000u);
  assert(!lap_input_device_state(100, &s, sizeof(s)) && s.axes[0] == -32767 &&
         s.axes[4] == 32767 && s.dpad_x == -1);
  assert(ls_lap_sensors_script("contact=200,17,0,10,20"));
  assert(ls_lap_sensors_script("contact=200,42,0,30,40"));
  assert(ls_lap_sensors_script("contact=200,17,1,15,25"));
  assert(!lap_input_device_state(200, &s, sizeof(s)) && s.contact_count == 2 &&
         s.contacts[0].id == 17 && s.contacts[0].x == 15);
  assert(ls_lap_sensors_script("contact=200,17,2,15,25"));
  assert(!lap_input_device_state(200, &s, sizeof(s)) && s.contact_count == 1 &&
         s.contacts[0].id == 42);
  assert(lap_input_devices(d, 8) == 2);
  /* Shared held state is a union; one keyboard/mouse cannot release another. */
  ls_input_ev_t shared = {.kind = LS_IEV_KEY_DOWN,
                          .source = LS_ISRC_BLE,
                          .device = 300,
                          .transport = LAP_TRANSPORT_BLE,
                          .classes = 3,
                          .key = LAP_KEY_LEFT,
                          .clicks = 1};
  ls_lap_input_event(&shared, 0, 0);
  shared.device = 301;
  ls_lap_input_event(&shared, 0, 0);
  shared.device = 300;
  shared.kind = LS_IEV_KEY_UP;
  ls_lap_input_event(&shared, 0, 0);
  assert(lap_key_down(LAP_KEY_LEFT));
  shared.device = 301;
  ls_lap_input_event(&shared, 0, 0);
  assert(!lap_key_down(LAP_KEY_LEFT));
  shared.kind = LS_IEV_PTR_DOWN;
  shared.button = 0;
  shared.buttons = 1;
  shared.device = 300;
  ls_lap_input_event(&shared, 0, 0);
  shared.device = 301;
  ls_lap_input_event(&shared, 0, 0);
  shared.kind = LS_IEV_PTR_UP;
  shared.buttons = 0;
  shared.device = 300;
  ls_lap_input_event(&shared, 0, 0);
  assert(!lap_input_state(&old) && (old.buttons & LAP_BUTTON_LEFT));
  ls_lap_input_touch(0, 0, false);
  assert(!lap_input_state(&old) && (old.buttons & LAP_BUTTON_LEFT));
  shared.device = 301;
  ls_lap_input_event(&shared, 0, 0);
  assert(!lap_input_state(&old) && !old.buttons);
  shared.kind = LS_IEV_DEVICE_DISCONNECTED;
  ls_lap_input_event(&shared, 0, 0);
  shared.device = 300;
  ls_lap_input_event(&shared, 0, 0);
  for (int i = 0; i < 80; i++)
    ls_lap_sensors_script("gamepad=100,1,2,3,4,5,6,7,0,0");
  assert(!lap_input_state(&old) && old.dropped > 0);
  assert(!lap_input_device_state(100, &s, sizeof(s)) && s.axes[5] == 6);
  ls_lap_lifecycle(LAP_INPUT_FOCUS_LOST, 0, 0);
  assert(!lap_input_device_state(100, &s, sizeof(s)) && !s.gamepad_buttons &&
         !s.axes[5]);
  assert(!lap_input_device_state(200, &s, sizeof(s)) && !s.contact_count);
  ls_lap_lifecycle(LAP_INPUT_FOCUS_GAINED, 0, 0);
  ls_lap_sensors_script("device=100,4,0");
  assert(!lap_input_device_state(100, &s, sizeof(s)) && !s.connected);
  /* An old reader writes exactly the old 22-byte event, not the extension. */
  struct {
    lap_input_event_t event;
    uint32_t guard;
  } legacy = {.guard = 0x12345678};
  while (lap_input_next(&legacy.event) > 0) {
  }
  assert(legacy.guard == 0x12345678);
}
static void track_fixture(void) {
  mkdir_one("sensor-sd");
  mkdir_one("sensor-sd/lakeshark");
  FILE *f = fopen("sensor-sd/lakeshark/track.log", "wb");
  assert(f);
  ls_rlog_header_t h = {LS_RLOG_MAGIC, 1, 16, 4, 3, 1};
  ls_track_pt_t p[4] = {
      {.t = 30, .lat_e7 = 420000300, .lon_e7 = -710000000, .alt_m = 3},
      {.t = 0},
      {.t = 10, .lat_e7 = 420000100, .lon_e7 = -710000000, .alt_m = 1},
      {.t = 20, .lat_e7 = 420000200, .lon_e7 = -710000000, .alt_m = 2}};
  assert(fwrite(&h, sizeof(h), 1, f) == 1 && fwrite(p, sizeof(p), 1, f) == 1);
  fclose(f);
}
static void navigation(void) {
  track_fixture();
  lap_navigation_point_t page[4];
  int h = lap_navigation_open(LAP_NAV_TRACK);
  assert(h > 0 && lap_navigation_open(LAP_NAV_TRACK) == LAP_RX_EBUSY);
  assert(lap_navigation_read(h, 0, page, 4) == LAP_RX_EAGAIN);
  pump();
  assert(lap_navigation_read(h, 0, page, 4) == 3 && page[0].time == 10 &&
         page[2].time == 30);
  assert(lap_navigation_read(h, 3, page, 4) == 0);
  lap_navigation_close(h);
  assert(lap_navigation_read(h, 0, page, 4) == LAP_RX_EINVAL);
  assert(lap_navigation_open(LAP_NAV_TRACK) == LAP_RX_EBUSY);
  pump();
  h = lap_navigation_open(LAP_NAV_TRACK);
  lap_navigation_close(h);
  pump();
  mkdir_one("sensor-sd/maps");
  FILE *marks = fopen("sensor-sd/maps/marks.txt", "wb");
  assert(marks);
  fputs("M 42 -71 0 Camp\nT 1234 saved\nS 2 Sketch\nP 1 2\nM nan -70 0 bad\nM "
        "43 -72 1 Water\n",
        marks);
  fclose(marks);
  h = lap_navigation_open(LAP_NAV_WAYPOINT);
  assert(h > 0);
  pump();
  assert(lap_navigation_read(h, 0, page, 4) == 2 &&
         !strcmp(page[0].name, "Camp") && page[0].time == 1234 &&
         !strcmp(page[1].name, "Water"));
  lap_navigation_close(h);
  pump();
  remove("sensor-sd/maps/marks.txt");
  FILE *f = fopen("sensor-sd/lakeshark/track.log", "wb");
  assert(f);
  fputs("broken", f);
  fclose(f);
  h = lap_navigation_open(LAP_NAV_TRACK);
  pump();
  assert(lap_navigation_read(h, 0, page, 4) == LAP_RX_EIO);
  lap_navigation_close(h);
  pump();
  remove("sensor-sd/lakeshark/track.log");
  h = lap_navigation_open(LAP_NAV_TRACK);
  pump();
  assert(lap_navigation_read(h, 0, page, 4) == LAP_RX_ENODEV);
  lap_navigation_close(h);
  pump();
}
static void saved_place(void) {
  mkdir_one("sensor-sd/maps/installed");
  uint8_t header[80] = {0}, bits[8192] = {0};
  memcpy(header, "CCPLACE2", 8);
  memcpy(header + 16, "Dateline", 9);
  /* CRC32 from Python zlib over this independent stored-format fixture. */
  header[12] = 0x29;
  header[13] = 0x04;
  header[14] = 0x52;
  header[15] = 0x1b;
  bits[16] = bits[8176] = 1;
  FILE *f = fopen("sensor-sd/maps/installed/0000002a.ccpi", "wb");
  assert(f);
  assert(fwrite(header, 1, 80, f) == 80 && fwrite(bits, 1, 8192, f) == 8192);
  fclose(f);
  lap_navigation_point_t p;
  int h = lap_navigation_open(LAP_NAV_PLACE);
  assert(h > 0);
  pump();
  assert(lap_navigation_read(h, 0, &p, 1) == 1 && p.id == 42 && p.flags == 2 &&
         !strcmp(p.name, "Dateline") && (p.lon > 179 || p.lon < -179));
  lap_navigation_close(h);
  pump();
  f = fopen("sensor-sd/maps/installed/0000002a.ccpi", "r+b");
  assert(f);
  fseek(f, 80, SEEK_SET);
  fputc(1, f);
  fclose(f);
  h = lap_navigation_open(LAP_NAV_PLACE);
  pump();
  assert(lap_navigation_read(h, 0, &p, 1) == LAP_RX_EIO);
  lap_navigation_close(h);
  pump();
  remove("sensor-sd/maps/installed/0000002a.ccpi");
}
static void full_track(void) {
  FILE *f = fopen("sensor-sd/lakeshark/track.log", "wb");
  assert(f);
  ls_rlog_header_t header = {LS_RLOG_MAGIC, 1, 16, 4096, 4096, 2048};
  assert(fwrite(&header, sizeof(header), 1, f) == 1);
  for (unsigned i = 0; i < 4096; i++) {
    ls_track_pt_t p = {
        .t = i, .lat_e7 = 420000000 + (int)i, .lon_e7 = -710000000};
    assert(fwrite(&p, sizeof(p), 1, f) == 1);
  }
  fclose(f);
  lap_navigation_point_t p;
  int h = lap_navigation_open(LAP_NAV_TRACK);
  pump();
  assert(lap_navigation_read(h, 0, &p, 1) == 1 && p.time == 2048);
  assert(lap_navigation_read(h, 4095, &p, 1) == 1 && p.time == 2047);
  assert(lap_navigation_read(h, 4096, &p, 1) == 0);
  lap_navigation_close(h);
  pump();
  header.capacity = 4097;
  f = fopen("sensor-sd/lakeshark/track.log", "r+b");
  assert(f);
  assert(fwrite(&header, sizeof(header), 1, f) == 1);
  fclose(f);
  h = lap_navigation_open(LAP_NAV_TRACK);
  pump();
  assert(lap_navigation_read(h, 0, &p, 1) == LAP_RX_EIO);
  lap_navigation_close(h);
  pump();
  remove("sensor-sd/lakeshark/track.log");
}
int main(void) {
#ifdef _WIN32
  unsigned char *base = (unsigned char *)GetModuleHandleA(NULL);
  IMAGE_NT_HEADERS *nt =
      (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
  ls_lap_api_set_image(base, nt->OptionalHeader.SizeOfImage);
#else
  extern char __executable_start, _end;
  ls_lap_api_set_image(&__executable_start,
                       (uintptr_t)&_end - (uintptr_t)&__executable_start);
#endif
  ls_lap_api_host_clock = clock_now;
  tick = 100;
  begin();
  gps();
  motion();
  input();
  navigation();
  saved_place();
  full_track();
  int h = lap_gps_acquire(0);
  assert(h > 0);
  pump();
  ls_lap_sys_end();
  pump();
  assert(!ls_lap_sensors_host()->gps_running);
  ls_lap_api_callback(false);
  ls_lap_api_cleanup();
  puts("API 2.5 input/sensors: bounds, freshness, leases, rate, contacts, "
       "overflow, ring/cancel/unload PASS");
  return 0;
}
