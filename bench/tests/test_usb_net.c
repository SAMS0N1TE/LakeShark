/* USB Ethernet adapters: descriptor parsing, device routing beside the SDR
   dongles, CDC notifications and the ECM/RNDIS frame path. Descriptors are
   modelled on lsusb -v of the real devices. */
#include "ls_test.h"
#include "usb_net_desc.h"

#include <stdint.h>
#include <string.h>

/* RTL8153 (0bda:8153) configuration 2: CDC-ECM. */
static const uint8_t rtl8153_ecm[] = {
    0x09, 0x02, 0x50, 0x00, 0x02, 0x02, 0x00, 0xa0, 0x32,
    0x09, 0x04, 0x00, 0x00, 0x01, 0x02, 0x06, 0x00, 0x05,
    0x05, 0x24, 0x00, 0x10, 0x01,                         /* CDC header */
    0x05, 0x24, 0x06, 0x00, 0x01,                         /* union 0 -> 1 */
    0x0d, 0x24, 0x0f, 0x03, 0x00, 0x00, 0x00, 0x00, 0xea, 0x05, 0x00, 0x00, 0x00,
    0x07, 0x05, 0x83, 0x03, 0x10, 0x00, 0x08,             /* notify IN, 16 */
    0x09, 0x04, 0x01, 0x00, 0x00, 0x0a, 0x00, 0x00, 0x00, /* data alt 0, idle */
    0x09, 0x04, 0x01, 0x01, 0x02, 0x0a, 0x00, 0x00, 0x00, /* data alt 1 */
    0x07, 0x05, 0x81, 0x02, 0x00, 0x02, 0x00,
    0x07, 0x05, 0x02, 0x02, 0x00, 0x02, 0x00,
};

/* RTL8153 configuration 1: the vendor protocol (r8152), not ours. */
static const uint8_t rtl8153_vendor[] = {
    0x09, 0x02, 0x27, 0x00, 0x01, 0x01, 0x00, 0xa0, 0x32,
    0x09, 0x04, 0x00, 0x00, 0x03, 0xff, 0xff, 0x00, 0x00,
    0x07, 0x05, 0x81, 0x02, 0x00, 0x02, 0x00,
    0x07, 0x05, 0x02, 0x02, 0x00, 0x02, 0x00,
    0x07, 0x05, 0x83, 0x03, 0x02, 0x00, 0x08,
};

/* RTL2832U RTL-SDR (0bda:2838), its only configuration. */
static const uint8_t rtl2832u[] = {
    0x09, 0x02, 0x22, 0x00, 0x02, 0x01, 0x04, 0x80, 0xfa,
    0x09, 0x04, 0x00, 0x00, 0x01, 0xff, 0xff, 0xff, 0x05,
    0x07, 0x05, 0x81, 0x02, 0x00, 0x02, 0x00,
    0x09, 0x04, 0x01, 0x00, 0x00, 0xff, 0xff, 0xff, 0x05,
};

/* Android USB tethering: RNDIS on the wireless-controller class, with an IAD. */
static const uint8_t android_rndis[] = {
    0x09, 0x02, 0x4b, 0x00, 0x02, 0x01, 0x00, 0x80, 0xfa,
    0x08, 0x0b, 0x00, 0x02, 0xe0, 0x01, 0x03, 0x07,
    0x09, 0x04, 0x00, 0x00, 0x01, 0xe0, 0x01, 0x03, 0x05,
    0x05, 0x24, 0x00, 0x10, 0x01,
    0x05, 0x24, 0x01, 0x00, 0x01,
    0x04, 0x24, 0x02, 0x00,
    0x05, 0x24, 0x06, 0x00, 0x01,
    0x07, 0x05, 0x81, 0x03, 0x08, 0x00, 0x09,
    0x09, 0x04, 0x01, 0x00, 0x02, 0x0a, 0x00, 0x00, 0x06,
    0x07, 0x05, 0x82, 0x02, 0x00, 0x02, 0x00,
    0x07, 0x05, 0x01, 0x02, 0x00, 0x02, 0x00,
};

/* An ECM gadget on SuperSpeed: endpoint companions after each endpoint, no
   union descriptor (the data interface is the next one), full-speed sizes. */
static const uint8_t ecm_ss_no_union[] = {
    0x09, 0x02, 0x52, 0x00, 0x02, 0x01, 0x00, 0x80, 0xfa,
    0x09, 0x04, 0x04, 0x00, 0x01, 0x02, 0x06, 0x00, 0x00,
    0x0d, 0x24, 0x0f, 0x07, 0x00, 0x00, 0x00, 0x00, 0xea, 0x05, 0x00, 0x00, 0x00,
    0x07, 0x05, 0x85, 0x03, 0x10, 0x00, 0x09,
    0x06, 0x30, 0x00, 0x00, 0x10, 0x00,
    0x09, 0x04, 0x05, 0x00, 0x00, 0x0a, 0x00, 0x00, 0x00,
    0x09, 0x04, 0x05, 0x01, 0x02, 0x0a, 0x00, 0x00, 0x00,
    0x07, 0x05, 0x86, 0x02, 0x40, 0x00, 0x00,
    0x06, 0x30, 0x00, 0x00, 0x00, 0x00,
    0x07, 0x05, 0x07, 0x02, 0x40, 0x00, 0x00,
};

LS_CASE(rtl8153_ecm_configuration_parses)
{
    ls_usbnet_desc_t d;
    LS_CHECK(sizeof(rtl8153_ecm) == 0x50);
    LS_CHECK(ls_usbnet_parse_config(rtl8153_ecm, sizeof(rtl8153_ecm), &d));
    LS_CHECK(d.kind == LS_USBNET_ECM);
    LS_CHECK(d.config_value == 2);
    LS_CHECK(d.ctrl_intf == 0 && d.data_intf == 1 && d.data_alt == 1);
    LS_CHECK(d.notify_ep == 0x83 && d.notify_mps == 16);
    LS_CHECK(d.in_ep == 0x81 && d.out_ep == 0x02);
    LS_CHECK(d.in_mps == 512 && d.out_mps == 512);
    LS_CHECK(d.mac_string == 3);
    LS_CHECK(d.max_segment == 1514);
}

LS_CASE(vendor_and_sdr_configurations_are_not_network)
{
    ls_usbnet_desc_t d;
    LS_CHECK(!ls_usbnet_parse_config(rtl8153_vendor, sizeof(rtl8153_vendor), &d));
    LS_CHECK(!ls_usbnet_parse_config(rtl2832u, sizeof(rtl2832u), &d));
}

LS_CASE(android_rndis_parses)
{
    ls_usbnet_desc_t d;
    LS_CHECK(sizeof(android_rndis) == 0x4b);
    LS_CHECK(ls_usbnet_parse_config(android_rndis, sizeof(android_rndis), &d));
    LS_CHECK(d.kind == LS_USBNET_RNDIS);
    LS_CHECK(d.ctrl_intf == 0 && d.data_intf == 1 && d.data_alt == 0);
    LS_CHECK(d.notify_ep == 0x81 && d.notify_mps == 8);
    LS_CHECK(d.in_ep == 0x82 && d.out_ep == 0x01);
    LS_CHECK(d.mac_string == 0);
}

LS_CASE(ecm_without_union_and_with_companions_parses)
{
    ls_usbnet_desc_t d;
    LS_CHECK(sizeof(ecm_ss_no_union) == 0x52);
    LS_CHECK(ls_usbnet_parse_config(ecm_ss_no_union, sizeof(ecm_ss_no_union), &d));
    LS_CHECK(d.ctrl_intf == 4 && d.data_intf == 5 && d.data_alt == 1);
    LS_CHECK(d.in_ep == 0x86 && d.out_ep == 0x07 && d.in_mps == 64);
    LS_CHECK(d.notify_ep == 0x85 && d.mac_string == 7);
}

LS_CASE(malformed_descriptors_are_refused)
{
    uint8_t b[sizeof(rtl8153_ecm)];
    ls_usbnet_desc_t d;
    /* Cut inside the data alt setting: the bulk pair is gone. */
    LS_CHECK(!ls_usbnet_parse_config(rtl8153_ecm, 60, &d));
    /* A zero-length descriptor would loop forever if trusted. */
    memcpy(b, rtl8153_ecm, sizeof(b));
    b[18] = 0;
    LS_CHECK(!ls_usbnet_parse_config(b, sizeof(b), &d));
    /* A descriptor that runs past wTotalLength. */
    memcpy(b, rtl8153_ecm, sizeof(b));
    b[sizeof(b) - 7] = 9;
    LS_CHECK(!ls_usbnet_parse_config(b, sizeof(b), &d));
    /* Not a configuration descriptor at all. */
    memcpy(b, rtl8153_ecm, sizeof(b));
    b[1] = 0x01;
    LS_CHECK(!ls_usbnet_parse_config(b, sizeof(b), &d));
    LS_CHECK(!ls_usbnet_parse_config(NULL, 0, &d));
    /* ECM whose data interface lost its OUT endpoint. */
    memcpy(b, rtl8153_ecm, sizeof(b));
    b[sizeof(b) - 5] = 0x82; /* OUT 0x02 -> a second IN */
    LS_CHECK(!ls_usbnet_parse_config(b, sizeof(b), &d));
}

LS_CASE(routing_keeps_sdr_dongles_on_the_sdr_path)
{
    /* RTL-SDR: one configuration of vendor interfaces. */
    LS_CHECK(ls_usbnet_route(false, 1, rtl2832u, sizeof(rtl2832u), false) == LS_USB_ROUTE_SDR);
    LS_CHECK(ls_usbnet_route(false, 1, NULL, 0, false) == LS_USB_ROUTE_SDR);
    LS_CHECK(ls_usbnet_route(true, 1, rtl2832u, sizeof(rtl2832u), false) == LS_USB_ROUTE_HACKRF);
    /* An adapter already in its ECM configuration. */
    LS_CHECK(ls_usbnet_route(false, 2, rtl8153_ecm, sizeof(rtl8153_ecm), false) == LS_USB_ROUTE_NET);
    LS_CHECK(ls_usbnet_route(false, 1, android_rndis, sizeof(android_rndis), false) == LS_USB_ROUTE_NET);
    /* RTL8153 in vendor mode: look at its other configurations, once. */
    LS_CHECK(ls_usbnet_route(false, 2, rtl8153_vendor, sizeof(rtl8153_vendor), false) ==
             LS_USB_ROUTE_NET_PROBE);
    LS_CHECK(ls_usbnet_route(false, 2, rtl8153_vendor, sizeof(rtl8153_vendor), true) ==
             LS_USB_ROUTE_SDR);
}

LS_CASE(learned_configuration_is_picked_at_enumeration)
{
    ls_usbnet_forget_configs();
    LS_CHECK(ls_usbnet_pick_config(0x0bda, 0x8153, 2) == 1);
    LS_CHECK(!ls_usbnet_config_learned(0x0bda, 0x8153));
    ls_usbnet_learn_config(0x0bda, 0x8153, 2);
    LS_CHECK(ls_usbnet_config_learned(0x0bda, 0x8153));
    LS_CHECK(ls_usbnet_pick_config(0x0bda, 0x8153, 2) == 2);
    /* Same VID, the RTL-SDR: untouched. */
    LS_CHECK(ls_usbnet_pick_config(0x0bda, 0x2838, 1) == 1);
    /* A learned value the device does not have falls back to 1. */
    LS_CHECK(ls_usbnet_pick_config(0x0bda, 0x8153, 1) == 1);
    /* The table is small and wraps, newest kept. */
    for (uint16_t p = 1; p <= 4; ++p) ls_usbnet_learn_config(0x1234, p, 2);
    LS_CHECK(!ls_usbnet_config_learned(0x0bda, 0x8153));
    LS_CHECK(ls_usbnet_config_learned(0x1234, 4));
    ls_usbnet_forget_configs();
    LS_CHECK(!ls_usbnet_config_learned(0x1234, 4));
}

static size_t utf16(uint8_t *out, const char *s)
{
    size_t n = strlen(s);
    out[0] = (uint8_t)(2 + 2 * n);
    out[1] = 0x03;
    for (size_t i = 0; i < n; ++i) { out[2 + 2 * i] = (uint8_t)s[i]; out[3 + 2 * i] = 0; }
    return 2 + 2 * n;
}

LS_CASE(mac_string_parses)
{
    uint8_t d[64], mac[6] = {0};
    size_t n = utf16(d, "00E04C68a1B2");
    LS_CHECK(ls_usbnet_parse_mac_string(d, n, mac));
    const uint8_t want[6] = {0x00, 0xe0, 0x4c, 0x68, 0xa1, 0xb2};
    LS_CHECK(!memcmp(mac, want, 6));
    n = utf16(d, "00E04C68A1G2");
    LS_CHECK(!ls_usbnet_parse_mac_string(d, n, mac));
    n = utf16(d, "01E04C68A1B2"); /* multicast */
    LS_CHECK(!ls_usbnet_parse_mac_string(d, n, mac));
    n = utf16(d, "000000000000");
    LS_CHECK(!ls_usbnet_parse_mac_string(d, n, mac));
    n = utf16(d, "00E04C68A1");
    LS_CHECK(!ls_usbnet_parse_mac_string(d, n, mac));
    n = utf16(d, "00E04C68A1B2");
    d[3] = 0x01; /* a non-ASCII code unit */
    LS_CHECK(!ls_usbnet_parse_mac_string(d, n, mac));
}

LS_CASE(cdc_notifications)
{
    const uint8_t up[8] = {0xa1, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00};
    const uint8_t down[8] = {0xa1, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    /* RTL8153 sends the speed then the connection, sometimes in one transfer. */
    const uint8_t both[24] = {0xa1, 0x2a, 0x00, 0x00, 0x00, 0x00, 0x08, 0x00,
                              0x00, 0xe1, 0xf5, 0x05, 0x00, 0xe1, 0xf5, 0x05,
                              0xa1, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00};
    bool link = false;
    uint32_t bps = 0;
    LS_CHECK(ls_usbnet_parse_notification(up, 8, &link, &bps) == LS_USBNET_NOTE_LINK && link);
    LS_CHECK(ls_usbnet_parse_notification(down, 8, &link, &bps) == LS_USBNET_NOTE_LINK && !link);
    LS_CHECK(ls_usbnet_parse_notification(both, 24, &link, &bps) ==
             (LS_USBNET_NOTE_LINK | LS_USBNET_NOTE_SPEED));
    LS_CHECK(link && bps == 100000000u);
    /* A speed change cut short sets nothing. */
    bps = 7;
    LS_CHECK(ls_usbnet_parse_notification(both, 12, &link, &bps) == 0 && bps == 7);
    /* RNDIS RESPONSE_AVAILABLE. */
    const uint8_t resp[8] = {0x01, 0, 0, 0, 0, 0, 0, 0};
    LS_CHECK(ls_usbnet_parse_notification(resp, 8, &link, &bps) == LS_USBNET_NOTE_RESPONSE);
    LS_CHECK(ls_usbnet_parse_notification(up, 4, &link, &bps) == 0);
}

LS_CASE(ecm_frame_bounds_and_zero_length_packets)
{
    LS_CHECK(!ls_usbnet_frame_ok(13));
    LS_CHECK(ls_usbnet_frame_ok(14));
    LS_CHECK(ls_usbnet_frame_ok(1514));
    LS_CHECK(!ls_usbnet_frame_ok(1515));
    LS_CHECK(ls_usbnet_tx_needs_zlp(512, 512));
    LS_CHECK(ls_usbnet_tx_needs_zlp(1024, 512));
    LS_CHECK(!ls_usbnet_tx_needs_zlp(1514, 512));
    LS_CHECK(ls_usbnet_tx_needs_zlp(1472, 64));
    LS_CHECK(!ls_usbnet_tx_needs_zlp(60, 64));
    LS_CHECK(!ls_usbnet_tx_needs_zlp(0, 512));
    LS_CHECK(!ls_usbnet_tx_needs_zlp(512, 0));
    /* The bulk IN buffer is whole packets at every speed and holds a frame. */
    LS_CHECK(LS_USBNET_RX_BUFFER % 512 == 0 && LS_USBNET_RX_BUFFER % 64 == 0);
    LS_CHECK(LS_USBNET_RX_BUFFER >= LS_USBNET_FRAME_MAX + LS_RNDIS_HEADER);
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

LS_CASE(rndis_control_messages)
{
    uint8_t m[64];
    LS_CHECK(ls_rndis_build_init(m, sizeof(m), 1, 2048) == 24);
    LS_CHECK(rd32(m) == RNDIS_MSG_INIT && rd32(m + 4) == 24 && rd32(m + 8) == 1 &&
             rd32(m + 12) == 1 && rd32(m + 20) == 2048);
    LS_CHECK(ls_rndis_build_query(m, sizeof(m), 2, RNDIS_OID_PERMANENT_ADDR) == 28);
    LS_CHECK(rd32(m) == RNDIS_MSG_QUERY && rd32(m + 12) == RNDIS_OID_PERMANENT_ADDR);
    LS_CHECK(ls_rndis_build_set_u32(m, sizeof(m), 3, RNDIS_OID_PACKET_FILTER,
                                    RNDIS_FILTER_DEFAULT) == 32);
    LS_CHECK(rd32(m + 16) == 4 && rd32(m + 20) == 20 && rd32(m + 28) == RNDIS_FILTER_DEFAULT);
    LS_CHECK(ls_rndis_build_set_u32(m, 31, 3, 0, 0) == 0);

    /* QUERY_C carrying a MAC, as an Android phone answers. */
    uint8_t r[40] = {0};
    wr32(r, RNDIS_MSG_QUERY_C); wr32(r + 4, 30); wr32(r + 8, 2); wr32(r + 12, 0);
    wr32(r + 16, 6); wr32(r + 20, 16);
    const uint8_t mac[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
    memcpy(r + 24, mac, 6);
    const uint8_t *info = NULL;
    size_t n = 0;
    uint32_t status = 99;
    LS_CHECK(ls_rndis_parse_reply(r, 30, RNDIS_MSG_QUERY_C, 2, &status, &info, &n));
    LS_CHECK(status == 0 && n == 6 && !memcmp(info, mac, 6));
    LS_CHECK(!ls_rndis_parse_reply(r, 30, RNDIS_MSG_QUERY_C, 9, &status, &info, &n));
    LS_CHECK(!ls_rndis_parse_reply(r, 30, RNDIS_MSG_SET_C, 2, &status, &info, &n));
    LS_CHECK(!ls_rndis_parse_reply(r, 20, RNDIS_MSG_QUERY_C, 2, &status, &info, &n));
    wr32(r + 16, 64); /* information buffer past the message */
    LS_CHECK(!ls_rndis_parse_reply(r, 30, RNDIS_MSG_QUERY_C, 2, &status, &info, &n));
    wr32(r + 16, 6); wr32(r + 20, 0xfffffff0u);
    LS_CHECK(!ls_rndis_parse_reply(r, 30, RNDIS_MSG_QUERY_C, 2, &status, &info, &n));

    /* SET_C with a failure status (NDIS codes have the top bit set). */
    wr32(r, RNDIS_MSG_SET_C); wr32(r + 4, 16); wr32(r + 8, 3); wr32(r + 12, 0xC0000001u);
    LS_CHECK(ls_rndis_parse_reply(r, 16, RNDIS_MSG_SET_C, 3, &status, NULL, NULL));
    LS_CHECK(status == 0xC0000001u);

    uint8_t ind[20] = {0};
    wr32(ind, RNDIS_MSG_INDICATE); wr32(ind + 4, 20); wr32(ind + 8, RNDIS_STATUS_MEDIA_CONNECT);
    LS_CHECK(ls_rndis_parse_indicate(ind, 20) == 1);
    wr32(ind + 8, RNDIS_STATUS_MEDIA_DISCONNECT);
    LS_CHECK(ls_rndis_parse_indicate(ind, 20) == 0);
    wr32(ind + 8, 0x12345678);
    LS_CHECK(ls_rndis_parse_indicate(ind, 20) == -1);
    LS_CHECK(ls_rndis_parse_indicate(ind, 8) == -1);
}

typedef struct {
    unsigned frames;
    size_t lens[8];
    uint8_t first[8];
} sink_t;

static void collect(const uint8_t *frame, size_t len, void *ctx)
{
    sink_t *s = ctx;
    if (s->frames < 8) { s->lens[s->frames] = len; s->first[s->frames] = frame[0]; }
    s->frames++;
}

static size_t put_packet(uint8_t *at, size_t payload, uint8_t fill)
{
    ls_rndis_packet_header(at, payload);
    memset(at + LS_RNDIS_HEADER, fill, payload);
    return LS_RNDIS_HEADER + payload;
}

LS_CASE(rndis_frame_path)
{
    uint8_t buf[LS_USBNET_RX_BUFFER] = {0};
    sink_t s = {0};
    /* One frame, the TX side's own header read back. */
    size_t n = put_packet(buf, 60, 0xaa);
    LS_CHECK(rd32(buf + 4) == 104 && rd32(buf + 8) == 36 && rd32(buf + 12) == 60);
    LS_CHECK(ls_rndis_unwrap(buf, n, collect, &s) == 1 && s.lens[0] == 60 && s.first[0] == 0xaa);

    /* Two frames in one transfer, then the padding a device adds. */
    memset(&s, 0, sizeof(s));
    n = put_packet(buf, 1514, 0x11);
    n += put_packet(buf + n, 98, 0x22);
    LS_CHECK(n + 16 <= sizeof(buf));
    memset(buf + n, 0, 16);
    LS_CHECK(ls_rndis_unwrap(buf, n + 16, collect, &s) == 2);
    LS_CHECK(s.lens[0] == 1514 && s.first[0] == 0x11 && s.lens[1] == 98 && s.first[1] == 0x22);

    /* A runt is skipped, its neighbour still delivered. */
    memset(&s, 0, sizeof(s));
    n = put_packet(buf, 8, 0x33);
    n += put_packet(buf + n, 64, 0x44);
    LS_CHECK(ls_rndis_unwrap(buf, n, collect, &s) == 1 && s.first[0] == 0x44);

    /* A length past the transfer stops the walk without reading beyond it. */
    memset(&s, 0, sizeof(s));
    n = put_packet(buf, 64, 0x55);
    wr32(buf + 4, (uint32_t)n + 1);
    LS_CHECK(ls_rndis_unwrap(buf, n, collect, &s) == 0 && s.frames == 0);
    /* As does a data offset past the message. */
    n = put_packet(buf, 64, 0x55);
    wr32(buf + 8, 0xffffffe0u);
    LS_CHECK(ls_rndis_unwrap(buf, n, collect, &s) == 0);
    /* A zero message length cannot loop. */
    memset(buf, 0, 64);
    wr32(buf, RNDIS_MSG_PACKET);
    LS_CHECK(ls_rndis_unwrap(buf, 64, collect, &s) == 0);
}
