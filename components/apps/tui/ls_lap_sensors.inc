#include "ls_lap_context.h"
/* API 2.5 input/sensors. Included by sys so every runtime links this service.
 * UI owns leases/delivery; worker owns GPS acquisition and track/place IO.
 * The mailbox has one producer and one consumer; no retained app pointers. */
#include "../../lakeshark/core/ls_rlog.h"
#include "../../lakeshark/core/ls_track.h"
#include "ls_lap_sensors.h"
#include <dirent.h>
#include <stdatomic.h>
#if defined(ESP_PLATFORM) && !defined(LS_LAP_HOST_TEST)
#include "../../../main/ble_hid.h"
#include "ls_touch.h"
#define SENSOR_DEVICE 1
#else
#define SENSOR_DEVICE 0
static ls_lap_sensors_host_t sensor_host;
ls_lap_sensors_host_t *ls_lap_sensors_host(void) { return &sensor_host; }
#endif
static LS_LAP_POOL atomic_bool p4_gps_wanted_slots[LS_LAP_CONTEXTS];
#define p4_gps_wanted (p4_gps_wanted_slots[ls_lap_context_index()])

static LS_LAP_POOL atomic_int p4_gps_error_slots[LS_LAP_CONTEXTS];
#define p4_gps_error (p4_gps_error_slots[ls_lap_context_index()])

static LS_LAP_POOL int p4_gps_lease_slots[LS_LAP_CONTEXTS], sensor_ticket;
#define p4_gps_lease (p4_gps_lease_slots[ls_lap_context_index()])

static LS_LAP_POOL uint32_t p4_motion_fields_slots[LS_LAP_CONTEXTS], p4_motion_period_slots[LS_LAP_CONTEXTS], p4_motion_last_slots[LS_LAP_CONTEXTS], p4_motion_sequence_slots[LS_LAP_CONTEXTS];
#define p4_motion_fields (p4_motion_fields_slots[ls_lap_context_index()])
#define p4_motion_last (p4_motion_last_slots[ls_lap_context_index()])
#define p4_motion_period (p4_motion_period_slots[ls_lap_context_index()])
#define p4_motion_sequence (p4_motion_sequence_slots[ls_lap_context_index()])

static LS_LAP_POOL bool p4_motion_delivered_slots[LS_LAP_CONTEXTS];
#define p4_motion_delivered (p4_motion_delivered_slots[ls_lap_context_index()])

static uint32_t sensor_now(void) { return ls_lap_av_now(); }
static bool sensor_out(void *p, size_t n) {
  return ls_lap_api_inside() && ls_lap_api_readable(p, n);
}
static uint32_t sensor_age(uint32_t at, bool seen) {
  return seen ? sensor_now() - at : UINT32_MAX;
}
uint32_t ls_lap_input_hardware(void) {
  uint32_t bits = LAP_HW_NAVIGATION;
  if (ls_lap_sys_caps() & LAP_HW_DESKTOP)
    bits |= LAP_HW_MULTITOUCH | LAP_HW_GAMEPAD_DESKTOP;
#if SENSOR_DEVICE
#if CONFIG_LS_BLE_HEAD
  if (ls_lap_sys_caps() & LAP_HW_BLE)
    bits |= LAP_HW_HID_BLE | LAP_HW_GAMEPAD_BLE;
#endif
#if defined(LS_BOARD_PANEL_RUNTIME)
  if (ls_touch_multitouch())
    bits |= LAP_HW_MULTITOUCH; /* GT911 report IDs, five contacts */
#endif
#else
  uint32_t h = ls_lap_sys_host()->hardware;
  bits |= h & (LAP_HW_HID_BLE | LAP_HW_GAMEPAD_BLE | LAP_HW_GAMEPAD_USB |
               LAP_HW_MULTITOUCH);
#endif
  return bits;
}
int lap_gps_acquire(uint32_t flags) {
  if (!ls_lap_api_inside() || flags)
    return LAP_RX_EINVAL;
  if (!(ls_lap_sys_caps() & LAP_HW_GPS))
    return LAP_RX_ENODEV;
  if (!p4_gps_lease) {
    p4_gps_lease = ++sensor_ticket;
    atomic_store(&p4_gps_error, 0);
  }
  atomic_store(&p4_gps_wanted, true);
  if (wake_io)
    wake_io();
  return p4_gps_lease;
}
void lap_gps_release(int h) {
  if (!ls_lap_api_inside() || h <= 0 || h != p4_gps_lease)
    return;
  p4_gps_lease = 0;
  atomic_store(&p4_gps_wanted, false);
  if (wake_io)
    wake_io();
}
int lap_gps_status(lap_gps_status_t *out) {
  if (!sensor_out(out, sizeof(*out)))
    return LAP_RX_EINVAL;
  lap_gps_status_t v = {.size = sizeof(v),
                        .version = 1,
                        .leased = p4_gps_lease != 0,
                        .sentence_age_ms = UINT32_MAX,
                        .fix_age_ms = UINT32_MAX,
                        .sky_age_ms = UINT32_MAX,
                        .error = atomic_load(&p4_gps_error)};
  if (!(ls_lap_sys_caps() & LAP_HW_GPS)) {
    *out = v;
    return 0;
  }
#if SENSOR_DEVICE
  ls_gps_state_t g;
  ls_gps_get(&g);
  v.sentence_age_ms =
      sensor_age(g.last_sentence_us / 1000, g.last_sentence_us > 0);
  v.fix_age_ms = sensor_age(g.last_fix_us / 1000, g.last_fix_us > 0);
  v.sky_age_ms = sensor_age(g.last_gsv_us / 1000, g.last_gsv_us > 0);
  v.quality = g.quality;
  v.antenna = g.antenna;
  v.tracked = g.sats_tracked;
  v.position = (lap_gps_t){.fix = g.fix,
                           .lat = g.lat_deg,
                           .lon = g.lon_deg,
                           .alt_m = g.alt_m,
                           .speed_kmh = g.speed_kts * 1.852f,
                           .course_deg = g.course_deg,
                           .hdop = g.hdop,
                           .sats_used = g.sats_used,
                           .sats_visible = g.sats_visible,
                           .utc = g.utc_s};
  bool running = ls_gps_running();
#else
  v.quality = sensor_host.gps.quality;
  v.antenna = sensor_host.gps.antenna;
  v.position = ls_lap_sys_host()->g;
  v.tracked = sensor_host.gps.tracked;
  v.sentence_age_ms =
      sensor_age(sensor_host.sentence_at, sensor_host.sentence_seen);
  v.fix_age_ms = sensor_age(sensor_host.fix_at, sensor_host.fix_seen);
  v.sky_age_ms = sensor_age(sensor_host.sky_at, sensor_host.sky_seen);
  bool running = sensor_host.gps_running;
#endif
  if (!running)
    v.state = LAP_GPS_OFF;
  else if (v.position.fix && v.fix_age_ms < 3000 && v.sentence_age_ms < 3000)
    v.state = LAP_GPS_FIX;
  else if (v.fix_age_ms != UINT32_MAX)
    v.state = LAP_GPS_STALE;
  else
    v.state = LAP_GPS_ACQUIRING;
  v.position.fix = v.state == LAP_GPS_FIX;
  if (v.sky_age_ms >= 3000)
    v.position.sats_visible = v.tracked = 0;
  *out = v;
  return 0;
}
int lap_gps_satellites(lap_gps_satellite_t *out, int max) {
  if (max < 1 || max > 48 || !sensor_out(out, (size_t)max * sizeof(*out)))
    return LAP_RX_EINVAL;
  if (!(ls_lap_sys_caps() & LAP_HW_GPS))
    return LAP_RX_ENODEV;
#if SENSOR_DEVICE
  ls_gps_state_t g;
  ls_gps_get(&g);
  uint32_t age = sensor_age(g.last_gsv_us / 1000, g.last_gsv_us > 0);
  int n = g.sat_count < max ? g.sat_count : max;
  if (age >= 3000 || !ls_gps_running())
    return 0;
  for (int i = 0; i < n; i++)
    out[i] = (lap_gps_satellite_t){.size = sizeof(*out),
                                   .version = 1,
                                   .prn = g.sats[i].prn,
                                   .azimuth = g.sats[i].azimuth,
                                   .constellation = g.sats[i].sys,
                                   .elevation = g.sats[i].elevation,
                                   .snr = g.sats[i].snr,
                                   .used = g.sats[i].used,
                                   .age_ms = age};
#else
  uint32_t age = sensor_age(sensor_host.sky_at, sensor_host.sky_seen);
  int n = sensor_host.satellite_count < (unsigned)max
              ? (int)sensor_host.satellite_count
              : max;
  if (age >= 3000 || !sensor_host.gps_running)
    return 0;
  for (int i = 0; i < n; i++) {
    out[i] = sensor_host.satellites[i];
    out[i].size = sizeof(*out);
    out[i].version = 1;
    out[i].age_ms = age;
  }
#endif
  return n;
}
int lap_motion_subscribe(uint32_t fields, uint32_t period) {
  if (!ls_lap_api_inside() || (fields & ~31u))
    return LAP_RX_EINVAL;
  if (!fields && !period) {
    p4_motion_fields = 0;
    return 0;
  }
  if (!fields || period < 50 || period > 10000)
    return LAP_RX_ERANGE;
  if (!(ls_lap_sys_caps() & LAP_HW_IMU))
    return LAP_RX_ENODEV;
#if SENSOR_DEVICE
  if (!p4_sys.field) {
    if (!ls_field_start())
      return LAP_RX_EIO;
    p4_sys.field = true;
    ls_field_watch(true);
  }
#endif
  p4_motion_fields = fields;
  p4_motion_period = period;
  p4_motion_delivered = false;
  return 0;
}
static bool sensor_motion(lap_motion_sample_t *v) {
  memset(v, 0, sizeof(*v));
  v->size = sizeof(*v);
  v->version = 1;
  v->heading_sigma_deg = -1;
  v->motion.heading = v->motion.true_heading = -1;
  if (!(ls_lap_sys_caps() & LAP_HW_IMU))
    return false;
  v->fields = LAP_MOTION_ACCEL | LAP_MOTION_GYRO | LAP_MOTION_RAW;
#if SENSOR_DEVICE
  ls_field_sample_t f;
  ls_imu_sample_t raw;
  ls_compass_reading_t r;
  ls_field_compass_snapshot(&f, &raw);
  ls_compass_live(&r);
  if (!f.imu_valid || f.time_us <= 0)
    return false;
  v->age_ms = sensor_age(f.time_us / 1000, true);
  const ls_imu_sample_t *s = &f.imu;
  float a[3], g[3] = {-s->gx, s->gy, -s->gz};
  int ux = 0, uy = 1;
  ls_compass_gravity(s, a);
  ls_compass_view_up(&ux, &uy);
  v->motion.ax = a[0] * uy - a[1] * ux;
  v->motion.ay = a[0] * ux + a[1] * uy;
  v->motion.az = a[2];
  v->motion.gx = g[0] * uy - g[1] * ux;
  v->motion.gy = g[0] * ux + g[1] * uy;
  v->motion.gz = g[2];
  v->motion.mx = s->mx;
  v->motion.my = s->my;
  v->motion.mz = s->mz;
  v->motion.mag_valid = s->mag_valid;
  v->motion.temp_c = s->temp_c;
  v->motion.pitch = r.pitch;
  v->motion.roll = r.roll;
  memcpy(v->raw_accel, (float[]){raw.ax, raw.ay, raw.az}, sizeof(v->raw_accel));
  memcpy(v->raw_gyro, (float[]){raw.gx, raw.gy, raw.gz}, sizeof(v->raw_gyro));
  memcpy(v->raw_mag, (float[]){raw.mx, raw.my, raw.mz}, sizeof(v->raw_mag));
  if (r.calibrated)
    v->quality |= LAP_MOTION_CALIBRATED;
  if (r.interference)
    v->quality |= LAP_MOTION_DISTURBED;
  if (r.low_confidence)
    v->quality |= LAP_MOTION_LOW_CONFIDENCE;
  if (r.valid && !r.low_confidence) {
    v->fields |= LAP_MOTION_HEADING;
    v->motion.heading = r.magnetic;
    if (r.true_valid) {
      v->quality |= LAP_MOTION_TRUE_NORTH;
      v->motion.true_heading = r.true_deg;
    }
  }
  if (isfinite(r.sigma_deg) && r.sigma_deg >= 0)
    v->heading_sigma_deg = r.sigma_deg;
#else
  v->motion = ls_lap_sys_host()->m;
  v->age_ms = v->motion.age_ms;
  v->quality = sensor_host.motion_quality;
  v->heading_sigma_deg = sensor_host.sigma;
  memcpy(v->raw_accel, (float[]){v->motion.ax, v->motion.ay, v->motion.az},
         sizeof(v->raw_accel));
  memcpy(v->raw_gyro, (float[]){v->motion.gx, v->motion.gy, v->motion.gz},
         sizeof(v->raw_gyro));
  memcpy(v->raw_mag, (float[]){v->motion.mx, v->motion.my, v->motion.mz},
         sizeof(v->raw_mag));
  if (v->motion.heading >= 0)
    v->fields |= LAP_MOTION_HEADING;
#endif
  if (v->motion.mag_valid)
    v->fields |= LAP_MOTION_MAG;
  v->motion.age_ms = v->age_ms;
  return true;
}
int lap_motion_status(lap_motion_status_t *out) {
  if (!sensor_out(out, sizeof(*out)))
    return LAP_RX_EINVAL;
  lap_motion_sample_t v;
  bool have = sensor_motion(&v);
  *out = (lap_motion_status_t){.size = sizeof(*out),
                               .version = 1,
                               .available = have ? v.fields : 0,
                               .subscribed = p4_motion_fields,
                               .period_ms = p4_motion_period,
                               .age_ms = have ? v.age_ms : UINT32_MAX,
                               .sequence = p4_motion_sequence,
                               .quality = v.quality,
                               .heading_sigma_deg = v.heading_sigma_deg};
  return 0;
}
int lap_motion_next(lap_motion_sample_t *out) {
  if (!sensor_out(out, sizeof(*out)))
    return LAP_RX_EINVAL;
  if (!p4_motion_fields)
    return LAP_RX_EINVAL;
  if (!(ls_lap_sys_caps() & LAP_HW_IMU))
    return LAP_RX_ENODEV;
  if (p4_motion_delivered && sensor_now() - p4_motion_last < p4_motion_period)
    return 0;
  lap_motion_sample_t v;
  if (!sensor_motion(&v))
    return LAP_RX_EAGAIN;
  v.fields &= p4_motion_fields;
  v.sequence = ++p4_motion_sequence;
  p4_motion_last = sensor_now();
  p4_motion_delivered = true;
  *out = v;
  return 1;
}

/* Track records stay compact (64 KiB maximum), never a 400 KiB expanded copy.
 * A worker snapshot is immutable from DONE until close. Cancellation does not
 * reset BUSY; the worker must acknowledge before the slot can be reused. */
static LS_LAP_POOL atomic_int p4_nav_state_slots[LS_LAP_CONTEXTS];
#define p4_nav_state (p4_nav_state_slots[ls_lap_context_index()])
 /* 0 idle,1 queued,2 worker busy,3 complete */
static LS_LAP_POOL atomic_bool p4_nav_cancel_slots[LS_LAP_CONTEXTS];
#define p4_nav_cancel (p4_nav_cancel_slots[ls_lap_context_index()])

static LS_LAP_POOL int p4_nav_handle_slots[LS_LAP_CONTEXTS], p4_nav_kind_slots[LS_LAP_CONTEXTS], p4_nav_error_slots[LS_LAP_CONTEXTS];
#define p4_nav_error (p4_nav_error_slots[ls_lap_context_index()])
#define p4_nav_handle (p4_nav_handle_slots[ls_lap_context_index()])
#define p4_nav_kind (p4_nav_kind_slots[ls_lap_context_index()])

static LS_LAP_POOL unsigned p4_nav_count_slots[LS_LAP_CONTEXTS];
#define p4_nav_count (p4_nav_count_slots[ls_lap_context_index()])

static LS_LAP_POOL ls_track_pt_t *p4_nav_track_slots[LS_LAP_CONTEXTS];
#define p4_nav_track (p4_nav_track_slots[ls_lap_context_index()])

#if SENSOR_DEVICE
static EXT_RAM_BSS_ATTR lap_navigation_point_t p4_nav_named_slots[LS_LAP_CONTEXTS][48];
#define p4_nav_named (p4_nav_named_slots[ls_lap_context_index()])

#else
static LS_LAP_POOL lap_navigation_point_t p4_nav_named_slots[LS_LAP_CONTEXTS][48];
#define p4_nav_named (p4_nav_named_slots[ls_lap_context_index()])

#endif
static void nav_free(void) {
  stage_free(p4_nav_track);
  p4_nav_track = NULL;
  p4_nav_count = 0;
}
static uint32_t nav_u32(const uint8_t *p) {
  return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
         (uint32_t)p[3] << 24;
}
static void nav_track_load(void) {
  FILE *f = fopen(LS_LAP_SD_ROOT "/lakeshark/track.log", "rb");
  if (!f) {
    p4_nav_error = LAP_RX_ENODEV;
    return;
  }
  uint8_t b[24];
  ls_rlog_header_t h = {0};
  if (fread(b, 1, sizeof(b), f) == sizeof(b)) {
    h = (ls_rlog_header_t){nav_u32(b),      nav_u32(b + 4),  nav_u32(b + 8),
                           nav_u32(b + 12), nav_u32(b + 16), nav_u32(b + 20)};
  }
  if (h.magic != LS_RLOG_MAGIC || h.version != 1 ||
      h.rec_size != sizeof(ls_track_pt_t) || !h.capacity ||
      h.capacity > LS_TRACK_CAPACITY || h.count > h.capacity ||
      h.head >= h.capacity)
    p4_nav_error = LAP_RX_EIO;
  else if (h.count &&
           !(p4_nav_track = stage_alloc((size_t)h.count * sizeof(*p4_nav_track))))
    p4_nav_error = LAP_RX_ENOMEM;
  else {
    for (unsigned i = 0; i < h.count; i++) {
      unsigned slot = (h.head + h.capacity - h.count + i) % h.capacity;
      if (atomic_load(&p4_nav_cancel))
        break;
      if (fseek(f, 24L + (long)slot * sizeof(ls_track_pt_t), SEEK_SET) ||
          fread(p4_nav_track + i, sizeof(*p4_nav_track), 1, f) != 1 ||
          p4_nav_track[i].lat_e7 < -900000000 || p4_nav_track[i].lat_e7 > 900000000 ||
          p4_nav_track[i].lon_e7 < -1800000000 ||
          p4_nav_track[i].lon_e7 > 1800000000) {
        p4_nav_error = LAP_RX_EIO;
        break;
      }
      p4_nav_count++;
    }
    /* Ring append during snapshot is visible as a retry, never reordered
     * geometry. */
    FILE *check = fopen(LS_LAP_SD_ROOT "/lakeshark/track.log", "rb");
    if (!check || fread(b, 1, sizeof(b), check) != sizeof(b) ||
        nav_u32(b) != h.magic || nav_u32(b + 4) != h.version ||
        nav_u32(b + 8) != h.rec_size || nav_u32(b + 12) != h.capacity ||
        nav_u32(b + 16) != h.count || nav_u32(b + 20) != h.head)
      p4_nav_error = LAP_RX_EBUSY;
    if (check)
      fclose(check);
  }
  fclose(f);
}
static uint32_t nav_crc(const uint8_t *p, size_t n) {
  uint32_t c = ~0u;
  while (n--) {
    c ^= *p++;
    for (int i = 0; i < 8; i++)
      c = (c >> 1) ^ ((c & 1) ? 0xedb88320u : 0);
  }
  return ~c;
}
static void nav_places_load(void) {
  DIR *d = opendir(LS_LAP_SD_ROOT "/maps/installed");
  if (!d) {
    p4_nav_error = LAP_RX_ENODEV;
    return;
  }
  uint8_t *bits = stage_alloc(8192);
  if (!bits) {
    closedir(d);
    p4_nav_error = LAP_RX_ENOMEM;
    return;
  }
  struct dirent *e;
  while (p4_nav_count < 32 && (e = readdir(d)) && !atomic_load(&p4_nav_cancel)) {
    size_t n = strlen(e->d_name);
    if (n != 13 || strcmp(e->d_name + 8, ".ccpi"))
      continue;
    bool valid = true;
    for (int i = 0; i < 8; i++)
      if (!((e->d_name[i] >= '0' && e->d_name[i] <= '9') ||
            (e->d_name[i] >= 'a' && e->d_name[i] <= 'f') ||
            (e->d_name[i] >= 'A' && e->d_name[i] <= 'F')))
        valid = false;
    if (!valid)
      continue;
    char path[192];
    snprintf(path, sizeof(path), LS_LAP_SD_ROOT "/maps/installed/%.13s",
             e->d_name);
    FILE *f = fopen(path, "rb");
    if (!f)
      continue;
    uint8_t h[80];
    bool ok = fread(h, 1, 80, f) == 80 && fread(bits, 1, 8192, f) == 8192 &&
              fgetc(f) == EOF && !ferror(f);
    fclose(f);
    if (!ok || memcmp(h, "CCPLACE2", 8) || nav_u32(h + 8) > 1 ||
        !memchr(h + 16, 0, 64) || nav_crc(bits, 8192) != nav_u32(h + 12)) {
      p4_nav_error = LAP_RX_EIO;
      break;
    }
    /* The stored place is a tile cover, not an exact named coordinate.
     * Report its circular-mean longitude and Mercator centre, flags=2. */
    double sx = 0, sy = 0, y = 0, count = 0;
    for (unsigned id = 0; id < 65536; id++)
      if (bits[id / 8] & (1u << (id % 8))) {
        double a = ((id / 256 + .5) / 256.0 * 2 - 1) * 3.141592653589793;
        sx += cos(a);
        sy += sin(a);
        y += id % 256 + .5;
        count++;
      }
    if (!count)
      continue;
    lap_navigation_point_t *p = &p4_nav_named[p4_nav_count++];
    memset(p, 0, sizeof(*p));
    p->id = (uint32_t)strtoul(e->d_name, NULL, 16);
    p->kind = LAP_NAV_PLACE;
    p->flags = 2 | (nav_u32(h + 8) ? 4 : 0);
    p->lon = atan2(sy, sx) * 57.295779513;
    p->lat = atan(sinh(3.141592653589793 * (1 - 2 * y / count / 256))) *
             57.295779513;
    memcpy(p->name, h + 16, 64);
  }
  stage_free(bits);
  closedir(d);
}
static void nav_waypoints_load(void) {
  FILE *f = fopen(LS_LAP_SD_ROOT "/maps/marks.txt", "rb");
  if (!f) {
    p4_nav_error = LAP_RX_ENODEV;
    return;
  }
  char line[512];
  unsigned bytes = 0;
  lap_navigation_point_t *last = NULL;
  while (p4_nav_count < 48 && fgets(line, sizeof(line), f) &&
         !atomic_load(&p4_nav_cancel)) {
    bytes += (unsigned)strlen(line);
    if (bytes > 256 * 1024) {
      p4_nav_error = LAP_RX_ERANGE;
      break;
    }
    if (!strchr(line, '\n') && !feof(f)) {
      int c;
      while ((c = fgetc(f)) != EOF && c != '\n') {
        if (++bytes > 256 * 1024)
          break;
      }
      if (bytes > 256 * 1024) {
        p4_nav_error = LAP_RX_ERANGE;
        break;
      }
      continue;
    }
    if (line[0] == 'S')
      last = NULL;
    if (line[0] == 'M') {
      double lat, lon;
      int icon, used = 0;
      last = NULL;
      if (sscanf(line + 1, "%lf %lf %d %n", &lat, &lon, &icon, &used) != 3 ||
          !isfinite(lat) || !isfinite(lon) || fabs(lat) > 90 || fabs(lon) > 180)
        continue;
      last = &p4_nav_named[p4_nav_count++];
      memset(last, 0, sizeof(*last));
      last->id = p4_nav_count;
      last->kind = LAP_NAV_WAYPOINT;
      last->lat = lat;
      last->lon = lon;
      size_t n = strcspn(line + 1 + used, "\r\n");
      if (n >= sizeof(last->name))
        n = sizeof(last->name) - 1;
      memcpy(last->name, line + 1 + used, n);
      last->name[n] = 0;
    } else if (line[0] == 'T' && last) {
      unsigned stamp;
      if (sscanf(line + 1, "%u", &stamp) == 1)
        last->time = stamp;
    }
  }
  if (ferror(f))
    p4_nav_error = LAP_RX_EIO;
  fclose(f);
}
int lap_navigation_open(int kind) {
  if (!ls_lap_api_inside() || kind < LAP_NAV_TRACK || kind > LAP_NAV_PLACE)
    return LAP_RX_EINVAL;
  if (p4_nav_handle || atomic_load(&p4_nav_state))
    return LAP_RX_EBUSY;
  if (!(ls_lap_sys_caps() & LAP_HW_SD))
    return LAP_RX_ENODEV;
  p4_nav_handle = ++sensor_ticket;
  p4_nav_kind = kind;
  p4_nav_count = 0;
  p4_nav_error = 0;
  atomic_store(&p4_nav_cancel, false);
  atomic_store(&p4_nav_state, 1);
  if (wake_io)
    wake_io();
  return p4_nav_handle;
}
int lap_navigation_read(int h, uint32_t first, lap_navigation_point_t *out,
                        int max) {
  if (max < 1 || max > 64 || !sensor_out(out, (size_t)max * sizeof(*out)) ||
      h <= 0 || h != p4_nav_handle)
    return LAP_RX_EINVAL;
  if (atomic_load(&p4_nav_state) != 3)
    return LAP_RX_EAGAIN;
  if (p4_nav_error)
    return p4_nav_error;
  if (first >= p4_nav_count)
    return 0;
  unsigned n = p4_nav_count - first;
  if (n > (unsigned)max)
    n = max;
  for (unsigned i = 0; i < n; i++) {
    if (p4_nav_kind == LAP_NAV_TRACK) {
      const ls_track_pt_t *p = p4_nav_track + first + i;
      out[i] = (lap_navigation_point_t){.id = first + i + 1,
                                        .time = p->t,
                                        .lat = p->lat_e7 / 1e7,
                                        .lon = p->lon_e7 / 1e7,
                                        .altitude_m = p->alt_m,
                                        .kind = LAP_NAV_TRACK,
                                        .flags = p->flags};
    } else
      out[i] = p4_nav_named[first + i];
    out[i].size = sizeof(*out);
    out[i].version = 1;
  }
  return n;
}
void lap_navigation_close(int h) {
  if (!ls_lap_api_inside() || h <= 0 || h != p4_nav_handle)
    return;
  p4_nav_handle = 0;
  atomic_store(&p4_nav_cancel, true);
  if (wake_io)
    wake_io();
}
void ls_lap_sensors_begin(void) {
  p4_gps_lease = 0;
  p4_motion_fields = p4_motion_period = p4_motion_sequence = 0;
  p4_motion_delivered = false;
  atomic_store(&p4_gps_wanted, false);
  atomic_store(&p4_gps_error, 0);
#if !SENSOR_DEVICE
  memset(&sensor_host, 0, sizeof(sensor_host));
  sensor_host.sigma = -1;
#endif
}
void ls_lap_sensors_end(void) {
  p4_gps_lease = 0;
  p4_motion_fields = 0;
  atomic_store(&p4_gps_wanted, false);
  p4_nav_handle = 0;
  atomic_store(&p4_nav_cancel, true);
  if (wake_io)
    wake_io();
}
bool ls_lap_sensors_service(void) {
  bool want=false;
  for(unsigned i=0;i<LS_LAP_CONTEXTS;i++) want=atomic_load(&p4_gps_wanted_slots[i])||want;
#if SENSOR_DEVICE
  esp_err_t e = ls_gps_app_demand(want);
  atomic_store(&p4_gps_error, e == ESP_OK                  ? 0
                           : e == ESP_ERR_NOT_SUPPORTED ? LAP_RX_ENODEV
                                                        : LAP_RX_EIO);
#else
  sensor_host.gps_running = want && (ls_lap_sys_caps() & LAP_HW_GPS);
#endif
  int expected = 1;
  if (atomic_compare_exchange_strong(&p4_nav_state, &expected, 2)) {
    if (!atomic_load(&p4_nav_cancel)) {
      if (p4_nav_kind == LAP_NAV_TRACK)
        nav_track_load();
      else if (p4_nav_kind == LAP_NAV_PLACE)
        nav_places_load();
      else
        nav_waypoints_load();
    }
    atomic_store(&p4_nav_state, 3);
  }
  if (atomic_load(&p4_nav_state) == 3 && atomic_load(&p4_nav_cancel)) {
    nav_free();
    atomic_store(&p4_nav_state, 0);
  }
  return want || atomic_load(&p4_nav_state) == 1;
}
bool ls_lap_sensors_script(const char *c) {
#if !SENSOR_DEVICE
  ls_lap_sys_host_t *h = ls_lap_sys_host();
  int a, b, d, x, y;
  unsigned id, contact, buttons;
  double lat, lon;
  float alt, sigma;
  if (sscanf(c, "gpsfix=%lf,%lf,%f", &lat, &lon, &alt) == 3 && isfinite(lat) &&
      isfinite(lon) && isfinite(alt) && fabs(lat) <= 90 && fabs(lon) <= 180) {
    h->g.lat = lat;
    h->g.lon = lon;
    h->g.alt_m = alt;
    h->g.fix = true;
    h->g.hdop = 1;
    sensor_host.gps.quality = 1;
    sensor_host.sentence_at = sensor_host.fix_at = sensor_now();
    sensor_host.sentence_seen = sensor_host.fix_seen = true;
    return true;
  }
  if (!strcmp(c, "gpslost")) {
    h->g.fix = false;
    sensor_host.sentence_at = sensor_now();
    sensor_host.sentence_seen = true;
    return true;
  }
  if (sscanf(c, "gpsantenna=%d", &a) == 1 && a >= 0 && a <= 3) {
    sensor_host.gps.antenna = a;
    return true;
  }
  if (sscanf(c, "satellite=%u,%d,%d,%d,%d,%d", &id, &a, &b, &d, &x, &y) == 6 &&
      id > 0 && id < 256 && a >= 0 && a <= 5 && b >= 0 && b <= 90 && d >= 0 &&
      d < 360 && x >= 0 && x < 100 && (y == 0 || y == 1)) {
    unsigned n = 0;
    while (n < sensor_host.satellite_count &&
           !(sensor_host.satellites[n].prn == id &&
             sensor_host.satellites[n].constellation == a))
      n++;
    if (n == 48)
      return true;
    if (n == sensor_host.satellite_count)
      sensor_host.satellite_count++;
    sensor_host.satellites[n] = (lap_gps_satellite_t){.prn = id,
                                                      .constellation = a,
                                                      .elevation = b,
                                                      .azimuth = d,
                                                      .snr = x,
                                                      .used = y};
    sensor_host.sky_at = sensor_now();
    sensor_host.sky_seen = true;
    h->g.sats_visible = sensor_host.satellite_count;
    h->g.sats_used = 0;
    sensor_host.gps.tracked = 0;
    for (unsigned i = 0; i < sensor_host.satellite_count; i++) {
      h->g.sats_used += sensor_host.satellites[i].used;
      sensor_host.gps.tracked += sensor_host.satellites[i].snr > 0;
    }
    return true;
  }
  if (sscanf(c, "motionquality=%d,%f", &a, &sigma) == 2 && a >= 0 && a <= 15 &&
      isfinite(sigma) && sigma >= -1) {
    sensor_host.motion_quality = a;
    sensor_host.sigma = sigma;
    return true;
  }
  ls_input_ev_t e = {.source = LS_ISRC_TERM,
                     .transport = LAP_TRANSPORT_DESKTOP};
  if (sscanf(c, "device=%u,%d,%d", &id, &a, &b) == 3 && id && a > 0 && a < 16 &&
      (b == 0 || b == 1)) {
    e.device = id;
    e.classes = a;
    e.kind = b ? LS_IEV_DEVICE_CONNECTED : LS_IEV_DEVICE_DISCONNECTED;
    ls_lap_input_event(&e, 0, 0);
    return true;
  }
  int axes[6];
  if (sscanf(c, "gamepad=%u,%d,%d,%d,%d,%d,%d,%u,%d,%d", &id, &axes[0],
             &axes[1], &axes[2], &axes[3], &axes[4], &axes[5], &buttons, &a,
             &b) == 10 &&
      id && abs(a) <= 1 && abs(b) <= 1) {
    e.device = id;
    e.kind = LS_IEV_GAMEPAD;
    e.classes = LAP_DEVICE_GAMEPAD;
    e.gamepad_buttons = buttons;
    e.dpad_x = a;
    e.dpad_y = b;
    for (unsigned i = 0; i < 6; i++)
      e.axes[i] = axes[i] < (i < 4 ? -32767 : 0) ? (i < 4 ? -32767 : 0)
                  : axes[i] > 32767              ? 32767
                                                 : axes[i];
    ls_lap_input_event(&e, 0, 0);
    return true;
  }
  if (sscanf(c, "contact=%u,%u,%d,%d,%d", &id, &contact, &a, &x, &y) == 5 &&
      id && contact <= 65535 && a >= 0 && a <= 2) {
    e.device = id;
    e.contact = contact;
    e.kind = a == 0   ? LS_IEV_CONTACT_DOWN
             : a == 1 ? LS_IEV_CONTACT_MOVE
                      : LS_IEV_CONTACT_UP;
    e.classes = LAP_DEVICE_TOUCH;
    e.px = x;
    e.py = y;
    ls_lap_input_event(&e, 0, 0);
    return true;
  }
#else
  (void)c;
#endif
  return false;
}
