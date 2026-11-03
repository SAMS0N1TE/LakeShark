/* UI task only. One queue, legacy prefix untouched. Included by ls_lap_api.c.
 */
#if defined(ESP_PLATFORM) && !defined(LS_LAP_HOST_TEST)
#include "../../../main/ble_hid.h"
#endif
static void input25_clear(void) {
  for (unsigned i = 0; i < LAP_INPUT_DEVICES_MAX; i++) {
    uint32_t id = input_states[i].device;
    bool connected = input_states[i].connected;
    memset(&input_states[i], 0, sizeof(input_states[i]));
    input_states[i].device = id;
    input_states[i].connected = connected;
  }
}
static int input25_slot(uint32_t id, bool create) {
  int free_slot = -1;
  for (unsigned i = 0; i < LAP_INPUT_DEVICES_MAX; i++) {
    if (input_devices[i].id == id)
      return (int)i;
    if (!input_devices[i].connected)
      free_slot = (int)i;
  }
  if (!create || free_slot < 0)
    return -1;
  memset(&input_devices[free_slot], 0, sizeof(input_devices[free_slot]));
  memset(&input_states[free_slot], 0, sizeof(input_states[free_slot]));
  input_devices[free_slot].id = id;
  input_states[free_slot].device = id;
  return free_slot;
}
static bool input25_event(const ls_input_ev_t *ev, int ox, int oy) {
  if (ev->kind == LS_IEV_RELEASE_ALL)
    return false;
  uint32_t id = ev->device ? ev->device : 1u + ev->source;
  int slot = input25_slot(id, true);
  if (slot < 0) {
    runtime_input.state.dropped++;
    return ev->kind > LS_IEV_RESIZE;
  }
  lap_input_device_t *d = &input_devices[slot];
  lap_input_device_state_t *st = &input_states[slot];
  bool fresh = !d->connected;
  d->size = sizeof(*d);
  d->version = 1;
  d->transport = ev->transport                ? ev->transport
                 : ev->source == LS_ISRC_BLE  ? LAP_TRANSPORT_BLE
                 : ev->source == LS_ISRC_TERM ? LAP_TRANSPORT_DESKTOP
                                              : LAP_TRANSPORT_LOCAL;
  d->classes |= ev->classes                 ? ev->classes
                : ev->kind <= LS_IEV_KEY_UP ? LAP_DEVICE_KEYBOARD
                                            : LAP_DEVICE_MOUSE;
  d->connected = st->connected = ev->kind != LS_IEV_DEVICE_DISCONNECTED;
  snprintf(d->name, sizeof(d->name), "%s input %lu",
           d->transport == LAP_TRANSPORT_BLE       ? "BLE"
           : d->transport == LAP_TRANSPORT_DESKTOP ? "Desktop"
                                                   : "Local",
           (unsigned long)id);
  if (fresh && d->connected && ev->kind != LS_IEV_DEVICE_CONNECTED)
    input_push2((lap_input_event2_t){.event.kind = LAP_INPUT_DEVICE_CONNECTED,
                                     .device = id,
                                     .classes = d->classes,
                                     .transport = d->transport});
  lap_input_event2_t e = {
      .event = {.kind = ev->kind, .x = ev->px - ox, .y = ev->py - oy},
      .device = id,
      .contact = ev->contact,
      .classes = d->classes,
      .transport = d->transport};
  if (ev->kind == LS_IEV_DEVICE_CONNECTED && !fresh)
    return true;
  if (ev->kind == LS_IEV_DEVICE_DISCONNECTED) {
    memset(st, 0, sizeof(*st));
    st->device = id;
    /* Aggregate held state must never stick after removal. */
    memset(runtime_input.state.keys, 0, sizeof(runtime_input.state.keys));
    runtime_input.state.buttons = runtime_input.state.mods = 0;
    for (unsigned i = 0; i < LAP_INPUT_DEVICES_MAX; i++)
      if (input_states[i].connected) {
        for (unsigned k = 0; k < LAP_INPUT_KEY_WORDS; k++)
          runtime_input.state.keys[k] |= input_states[i].input.keys[k];
        runtime_input.state.buttons |= input_states[i].input.buttons;
        runtime_input.state.mods |= input_states[i].input.mods;
      }
  } else if (ev->kind == LS_IEV_GAMEPAD) {
    memcpy(st->axes, ev->axes, sizeof(st->axes));
    memcpy(e.axes, ev->axes, sizeof(e.axes));
    st->gamepad_buttons = e.gamepad_buttons = ev->gamepad_buttons;
    st->dpad_x = e.dpad_x = ev->dpad_x;
    st->dpad_y = e.dpad_y = ev->dpad_y;
  } else if (ev->kind >= LS_IEV_CONTACT_DOWN) {
    unsigned n = 0;
    while (n < st->contact_count && st->contacts[n].id != ev->contact)
      n++;
    if (ev->kind == LS_IEV_CONTACT_UP) {
      if (n >= st->contact_count)
        return true;
      st->contacts[n] = st->contacts[--st->contact_count];
    } else {
      /* A move cannot steal a contact that began outside the client. */
      if (n == st->contact_count && ev->kind != LS_IEV_CONTACT_DOWN)
        return true;
      if (n == st->contact_count && n < LAP_CONTACTS_MAX)
        st->contact_count++;
      if (n < LAP_CONTACTS_MAX)
        st->contacts[n] =
            (lap_input_contact_t){ev->contact, e.event.x, e.event.y, 0};
      else {
        runtime_input.state.dropped++;
        return true;
      }
    }
  } else {
    unsigned key =
        ev->key == LAP_KEY_CHAR
            ? LAP_KEY_CHAR((ev->ch >= 'A' && ev->ch <= 'Z') ? ev->ch + 'a' - 'A'
                                                            : ev->ch)
            : ev->key;
    if (key < 512 && ev->kind <= LS_IEV_KEY_UP) {
      if (ev->kind == LS_IEV_KEY_DOWN)
        st->input.keys[key / 32] |= 1u << (key % 32);
      else
        st->input.keys[key / 32] &= ~(1u << (key % 32));
    }
    st->input.mods = ev->mods;
    if (ev->kind >= LS_IEV_PTR_MOVE && ev->kind <= LS_IEV_WHEEL) {
      st->input.x = ev->px - ox;
      st->input.y = ev->py - oy;
      st->input.buttons = ev->buttons;
      if (ev->button < 8 && ev->kind == LS_IEV_PTR_DOWN)
        st->input.buttons |= 1u << ev->button;
      if (ev->button < 8 && ev->kind == LS_IEV_PTR_UP)
        st->input.buttons &= ~(1u << ev->button);
    }
    return false;
  }
  input_push2(e);
  return true;
}
static void input25_topology(void) {
#if defined(ESP_PLATFORM) && !defined(LS_LAP_HOST_TEST) && CONFIG_LS_BLE_HEAD
  if (input25_topology_frame == runtime_state.frame || !runtime_input.enabled || !runtime_input.focused)
    return;
  input25_topology_frame = runtime_state.frame;
  static EXT_RAM_BSS_ATTR hid_device_t peers[HID_CANDIDATES + HID_BONDS];
  size_t n = hid_list_devices(peers, HID_CANDIDATES + HID_BONDS, NULL);
  uint32_t present[HID_CONNECTIONS] = {0};
  unsigned used = 0;
  for (size_t i = 0; i < n && used < HID_CONNECTIONS; i++)
    if (peers[i].state == HID_READY) {
      uint32_t id = hid_input_device_id(peers[i].generation);
      present[used++] = id;
      ls_input_ev_t e = {.kind = LS_IEV_DEVICE_CONNECTED,
                         .device = id,
                         .source = LS_ISRC_BLE,
                         .transport = LAP_TRANSPORT_BLE,
                         .classes =
                             (peers[i].keyboard ? LAP_DEVICE_KEYBOARD : 0) |
                             (peers[i].mouse ? LAP_DEVICE_MOUSE : 0) |
                             (peers[i].gamepad ? LAP_DEVICE_GAMEPAD : 0)};
      input25_event(&e, 0, 0);
    }
  for (unsigned i = 0; i < LAP_INPUT_DEVICES_MAX; i++)
    if (input_devices[i].connected &&
        input_devices[i].transport == LAP_TRANSPORT_BLE) {
      unsigned j = 0;
      while (j < used && present[j] != input_devices[i].id)
        j++;
      if (j == used) {
        ls_input_ev_t e = {.kind = LS_IEV_DEVICE_DISCONNECTED,
                           .device = input_devices[i].id,
                           .source = LS_ISRC_BLE,
                           .transport = LAP_TRANSPORT_BLE};
        input25_event(&e, 0, 0);
      }
    }
#endif
}
int lap_input_next2(lap_input_event2_t *out, size_t size) {
  if (!inside() || size < sizeof(*out) ||
      !ls_lap_api_readable(out, sizeof(*out)))
    return LAP_RX_EINVAL;
  input25_topology();
  if (!runtime_input.count)
    return 0;
  *out = runtime_input.ring[runtime_input.head];
  runtime_input.head = (runtime_input.head + 1) % 64;
  runtime_input.count--;
  return 1;
}
int lap_input_devices(lap_input_device_t *out, int max) {
  if (!inside() || max < 1 || max > LAP_INPUT_DEVICES_MAX ||
      !ls_lap_api_readable(out, (size_t)max * sizeof(*out)))
    return LAP_RX_EINVAL;
  input25_topology();
  int n = 0;
  for (unsigned i = 0; i < LAP_INPUT_DEVICES_MAX && n < max; i++)
    if (input_devices[i].connected)
      out[n++] = input_devices[i];
  return n;
}
int lap_input_device_state(uint32_t id, lap_input_device_state_t *out,
                           size_t size) {
  if (!inside() || !id || size < sizeof(*out) ||
      !ls_lap_api_readable(out, sizeof(*out)))
    return LAP_RX_EINVAL;
  int slot = input25_slot(id, false);
  if (slot < 0)
    return LAP_RX_ENODEV;
  *out = input_states[slot];
  out->size = sizeof(*out);
  out->version = 1;
  out->input.size = sizeof(out->input);
  out->input.version = 1;
  out->input.dropped = runtime_input.state.dropped;
  return 0;
}
