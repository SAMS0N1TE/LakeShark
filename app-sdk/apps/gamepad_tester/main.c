/* Gamepads, BLE keys/mouse and touch all use the focused input queue. */
#include <lakeshark_app.h>
#include <stdio.h>
static lap_input_device_t devices[LAP_INPUT_DEVICES_MAX];
static lap_input_device_state_t state;
static lap_motion_sample_t motion;
static unsigned events;
static int device_count, selected;
static bool imu;
static void enter(void) {
  lap_caps_t caps;
  lap_caps(&caps, sizeof(caps));
  imu = (caps.hardware & LAP_HW_IMU) != 0;
  if (imu)
    lap_motion_subscribe(LAP_MOTION_ACCEL | LAP_MOTION_GYRO | LAP_MOTION_MAG |
                             LAP_MOTION_RAW | LAP_MOTION_HEADING,
                         100);
  events = 0;
  selected = 0;
}
static void leave(void) {
  if (imu)
    lap_motion_subscribe(0, 0);
}
static void draw(void) {
  lap_input_event2_t event;
  while (lap_input_next2(&event, sizeof(event)) > 0)
    events++;
  device_count = lap_input_devices(devices, LAP_INPUT_DEVICES_MAX);
  if (selected >= device_count)
    selected = 0;
  char line[96];
  snprintf(line, sizeof(line), "%d devices  %u events  TAB next", device_count,
           events);
  lap_put_str(0, 0, line, LAP_ATTR(LAP_CYAN, LAP_BLACK));
  if (device_count > 0 && lap_input_device_state(devices[selected].id, &state,
                                                 sizeof(state)) == 0) {
    lap_put_str(0, 2, devices[selected].name, LAP_ATTR(LAP_WHITE, LAP_BLACK));
    snprintf(line, sizeof(line), "LX %d LY %d  RX %d RY %d", state.axes[0],
             state.axes[1], state.axes[2], state.axes[3]);
    lap_put_str(0, 4, line, LAP_ATTR(LAP_GREEN, LAP_BLACK));
    snprintf(line, sizeof(line), "LT %d RT %d buttons %08lx dpad %d,%d",
             state.axes[4], state.axes[5], (unsigned long)state.gamepad_buttons,
             state.dpad_x, state.dpad_y);
    lap_put_str(0, 5, line, LAP_ATTR(LAP_GREEN, LAP_BLACK));
    for (unsigned i = 0; i < state.contact_count && i < 4; i++) {
      snprintf(line, sizeof(line), "Contact %u: %d,%d", state.contacts[i].id,
               state.contacts[i].x, state.contacts[i].y);
      lap_put_str(0, 7 + i, line, LAP_ATTR(LAP_YELLOW, LAP_BLACK));
    }
  } else
    lap_put_str(0, 2, "Connect HID in LINK, or use the desktop",
                LAP_ATTR(LAP_WHITE, LAP_BLACK));
  if (imu && lap_motion_next(&motion) > 0) {
    lap_motion_status_t status;
    lap_motion_status(&status);
    snprintf(line, sizeof(line), "IMU %lums q=%lx heading %.1f +/- %.1f",
             (unsigned long)status.period_ms, (unsigned long)status.quality,
             (double)motion.motion.heading, (double)status.heading_sigma_deg);
    lap_put_str(0, 12, line, LAP_ATTR(LAP_CYAN, LAP_BLACK));
  }
}
static bool key(lap_key_t k, char ch) {
  (void)ch;
  if (k != LAP_KEY_TAB)
    return false;
  if (device_count)
    selected = (selected + 1) % device_count;
  return true;
}
LAP_APP(.id = "gamepad-tester", .name = "INPUT TEST",
        .sub = "Gamepads, keys and contacts", .version = "1.0",
        .author = "LakeShark", .icon = LAP_ICON_STAR, .color = LAP_GREEN,
        .enter = enter, .leave = leave, .draw = draw, .key = key);
