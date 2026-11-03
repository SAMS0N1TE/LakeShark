#include <lakeshark_app.h>
#include <lap_canvas.h>
#include <math.h>
#include <stdio.h>
static lap_canvas_t canvas;
static lap_navigation_point_t points[512], page[32];
static unsigned count, first;
static int handle, kind = LAP_NAV_TRACK, error;
static bool loading, sd;
static void open_kind(void) {
  if (handle > 0)
    lap_navigation_close(handle);
  handle = 0;
  count = first = 0;
  error = 0;
  loading = true;
}
static void enter(void) {
  lap_caps_t c;
  lap_caps(&c, sizeof(c));
  sd = (c.hardware & LAP_HW_SD) != 0;
  open_kind();
}
static void leave(void) {
  if (handle > 0)
    lap_navigation_close(handle);
  handle = 0;
  lap_canvas_free(&canvas);
}
static double longitude_delta(double a, double b) {
  double d = a - b;
  if (d > 180)
    d -= 360;
  if (d < -180)
    d += 360;
  return d;
}
static void draw(void) {
  if (loading && !handle) {
    if (!sd) {
      error = LAP_RX_ENODEV;
      loading = false;
    } else {
      int h = lap_navigation_open(kind);
      if (h > 0)
        handle = h;
      else if (h != LAP_RX_EBUSY) {
        error = h;
        loading = false;
      }
    }
  }
  if (loading && handle > 0) {
    int n = lap_navigation_read(handle, first, page, 32);
    if (n >= 0) {
      /* Bound the drawing copy while still paging the complete ring. */
      for (int i = 0; i < n; i++)
        if (count < 512 &&
            (kind != LAP_NAV_TRACK || (first + (unsigned)i) % 8 == 0))
          points[count++] = page[i];
      first += (unsigned)n;
      if (!n)
        loading = false;
    } else if (n != LAP_RX_EAGAIN) {
      error = n;
      loading = false;
    }
  }
  char text[96];
  snprintf(text, sizeof(text), "%s %u points %s  T track W waypoints P places",
           kind == LAP_NAV_TRACK      ? "TRACK"
           : kind == LAP_NAV_WAYPOINT ? "WAYPOINTS"
                                      : "PLACES",
           count, loading ? "loading" : "");
  lap_put_str(0, 0, text, LAP_ATTR(LAP_CYAN, LAP_BLACK));
  if (error) {
    snprintf(text, sizeof(text), "Saved data unavailable (%d)", error);
    lap_put_str(0, 2, text, LAP_ATTR(LAP_YELLOW, LAP_BLACK));
    return;
  }
  if (!count) {
    lap_put_str(0, 2, "No saved points", LAP_ATTR(LAP_WHITE, LAP_BLACK));
    return;
  }
  int w = lap_cols(), h = lap_rows() - 3;
  if (h < 2 || !lap_canvas_init(&canvas, w, h))
    return;
  double lat = points[0].lat, lon = points[0].lon,
         scale = cosf((float)(lat * .017453292519943)), extent = 1e-5;
  for (unsigned i = 0; i < count; i++) {
    double dx = longitude_delta(points[i].lon, lon) * scale,
           dy = points[i].lat - lat;
    if (dx < 0)
      dx = -dx;
    if (dy < 0)
      dy = -dy;
    if (dx > extent)
      extent = dx;
    if (dy > extent)
      extent = dy;
  }
  lc_clear(&canvas, LAP_BLACK);
  int lastx = 0, lasty = 0;
  for (unsigned i = 0; i < count; i++) {
    int x = canvas.w / 2 + (int)(longitude_delta(points[i].lon, lon) * scale /
                                 extent * (canvas.w / 2 - 2));
    int y = canvas.h / 2 -
            (int)((points[i].lat - lat) / extent * (canvas.h / 2 - 2));
    if (i && kind == LAP_NAV_TRACK)
      lc_line(&canvas, lastx, lasty, x, y, LAP_GREEN);
    else
      lc_fill_circle(&canvas, x, y, 1, LAP_YELLOW);
    lastx = x;
    lasty = y;
  }
  lap_canvas_show(&canvas, 0, 2);
  if (kind == LAP_NAV_PLACE)
    lap_put_str(0, lap_rows() - 1, "Places show tile-cover centres",
                LAP_ATTR(LAP_YELLOW, LAP_BLACK));
}
static bool key(lap_key_t k, char ch) {
  if (k != LAP_KEY_CHAR)
    return false;
  if (ch == 't' || ch == 'T')
    kind = LAP_NAV_TRACK;
  else if (ch == 'w' || ch == 'W')
    kind = LAP_NAV_WAYPOINT;
  else if (ch == 'p' || ch == 'P')
    kind = LAP_NAV_PLACE;
  else
    return false;
  open_kind();
  return true;
}
LAP_APP(.id = "track-viewer", .name = "TRACK VIEW",
        .sub = "Saved tracks, waypoints, places", .version = "1.0",
        .author = "LakeShark", .icon = LAP_ICON_STAR, .color = LAP_GREEN,
        .enter = enter, .leave = leave, .draw = draw, .key = key);
