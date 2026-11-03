/* Real GT911 parser, fake I2C: contact IDs, bounds and atomic acknowledgements.
 */
#include "ls_i2c.h"
#include "ls_panel_ws.h"
#include "ls_touch.h"
#include "ls_test.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static uint8_t status, points[40];
static bool fail_read, fail_ack;
static unsigned frames, count;
static ls_touch_contact_t contacts[5];
esp_err_t ls_i2c_probe(ls_i2c_bus_id_t b, uint8_t a, int t) { return ESP_OK; }
esp_err_t ls_i2c_device(ls_i2c_bus_id_t b, uint8_t a, uint32_t hz,
                        i2c_master_dev_handle_t *out) {
  *out = (void *)1;
  return ESP_OK;
}
esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t d) { return ESP_OK; }
esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t dev,
                                      const uint8_t *tx, size_t nt, uint8_t *rx,
                                      size_t nr, int timeout) {
  assert(nt == 2);
  unsigned reg = (unsigned)tx[0] << 8 | tx[1];
  memset(rx, 0, nr);
  if (reg == 0x8140) {
    assert(nr == 10);
    memcpy(rx, "911", 3);
    rx[6] = 0xe8;
    rx[7] = 3;
    rx[8] = 0xd0;
    rx[9] = 7;
    return ESP_OK;
  }
  if (fail_read)
    return ESP_FAIL;
  if (reg == 0x814e) {
    assert(nr == 1);
    *rx = status;
    return ESP_OK;
  }
  assert(reg == 0x814f && nr <= 40);
  memcpy(rx, points, nr);
  return ESP_OK;
}
esp_err_t i2c_master_transmit(i2c_master_dev_handle_t dev, const uint8_t *tx,
                              size_t n, int timeout) {
  assert(n == 3 && tx[0] == 0x81 && tx[1] == 0x4e && tx[2] == 0);
  if (fail_ack)
    return ESP_FAIL;
  status = 0;
  return ESP_OK;
}
bool ls_panel_ws_size(int *w, int *h) {
  *w = 500;
  *h = 1000;
  return true;
}
bool ls_panel_ws_touch_map(ls_panel_ws_touch_map_t *m) {
  *m = (ls_panel_ws_touch_map_t){.mirror_x = true};
  return true;
}
void ls_touch_frame(const ls_touch_contact_t *p, unsigned n) {
  assert(n <= 5);
  memcpy(contacts, p, n * sizeof(*p));
  count = n;
  frames++;
}
static void point(unsigned at, unsigned id, unsigned x, unsigned y) {
  uint8_t *p = points + at * 8;
  p[0] = id;
  p[1] = x;
  p[2] = x >> 8;
  p[3] = y;
  p[4] = y >> 8;
}
LS_CASE(gt911_contact_ids_and_atomic_frames) {
  uint16_t x, y;
  bool down;
  assert(!ls_touch_multitouch());
  assert(ls_touch_init() == ESP_OK && ls_touch_multitouch());
  point(0, 7, 200, 400);
  point(1, 3, 800, 1600);
  status = 0x82;
  assert(ls_touch_read(&x, &y, &down) && down && count == 2 && frames == 1);
  assert(contacts[0].id == 7 && contacts[0].x == 399 && contacts[0].y == 200 &&
         contacts[1].id == 3 && contacts[1].x == 99);
  /* Reorder the report; identity follows the hardware ID, not array position.
   */
  point(0, 3, 820, 1620);
  point(1, 7, 220, 420);
  status = 0x82;
  assert(ls_touch_read(&x, &y, &down) && contacts[0].id == 3 &&
         contacts[1].id == 7 && frames == 2);
  status = 0x86;
  assert(!ls_touch_read(&x, &y, &down) && frames == 2);
  status = 0x82;
  point(1, 3, 200, 400);
  assert(!ls_touch_read(&x, &y, &down) && frames == 2);
  status = 0x81;
  fail_ack = true;
  assert(!ls_touch_read(&x, &y, &down) && frames == 2);
  fail_ack = false;
  fail_read = true;
  assert(!ls_touch_read(&x, &y, &down) && frames == 2);
  fail_read = false;
  status = 0x80;
  assert(ls_touch_read(&x, &y, &down) && !down && count == 0 && frames == 3);
  assert(ls_touch_read(&x, &y, &down) && !down && frames == 3);
  puts("GT911 complete contact frames: IDs, mapping, reorder, "
       "malformed/IO/ack, release PASS");
}
