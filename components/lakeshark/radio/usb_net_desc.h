/* SPDX-License-Identifier: GPL-3.0-or-later
   LakeShark original. USB Ethernet adapters (CDC-ECM, RNDIS): descriptor
   parsing, notification and frame helpers. Pure C with no ESP-IDF calls, so
   the bench tests it against descriptors taken from real adapters. The USB
   transfer side is usb_net.c. */
#ifndef LS_USB_NET_DESC_H
#define LS_USB_NET_DESC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Largest Ethernet frame without FCS (1500 MTU + 14 header). */
#define LS_USBNET_FRAME_MAX     1514u
#define LS_USBNET_FRAME_MIN     14u
/* Bulk IN transfer size: one ECM frame, or an RNDIS transfer of up to this
   many bytes (the size is offered to the device in REMOTE_NDIS_INITIALIZE).
   A multiple of every bulk max packet size (8..512). */
#define LS_USBNET_RX_BUFFER     2048u
#define LS_RNDIS_HEADER         44u

typedef enum {
    LS_USBNET_NONE = 0,
    LS_USBNET_ECM,
    LS_USBNET_RNDIS,
} ls_usbnet_kind_t;

/* One network function found in a configuration descriptor. */
typedef struct {
    ls_usbnet_kind_t kind;
    uint8_t config_value;     /* bConfigurationValue it was found in */
    uint8_t ctrl_intf;        /* communication interface */
    uint8_t data_intf;        /* data interface (union slave) */
    uint8_t data_alt;         /* data alt setting with the bulk pair (ECM: 1) */
    uint8_t notify_ep;        /* interrupt IN; 0 if the function has none */
    uint16_t notify_mps;
    uint8_t in_ep, out_ep;    /* bulk */
    uint16_t in_mps, out_mps;
    uint8_t mac_string;       /* ECM iMACAddress string index; 0 if absent */
    uint16_t max_segment;     /* ECM wMaxSegmentSize; 0 if absent */
} ls_usbnet_desc_t;

/* Finds the first complete CDC-ECM or RNDIS function in a configuration
   descriptor (bytes as sent by the device, wTotalLength long). False when the
   configuration has none or the descriptor is malformed. */
bool ls_usbnet_parse_config(const uint8_t *cfg, size_t len, ls_usbnet_desc_t *out);

/* iMACAddress string descriptor (12 UTF-16LE hex digits) to 6 bytes. */
bool ls_usbnet_parse_mac_string(const uint8_t *desc, size_t len, uint8_t mac[6]);

/* CDC notifications on the interrupt endpoint. Applies every notification in
   the buffer; returns which fields it set (LS_USBNET_NOTE_* bits). */
#define LS_USBNET_NOTE_LINK      1u
#define LS_USBNET_NOTE_SPEED     2u
#define LS_USBNET_NOTE_RESPONSE  4u  /* RNDIS: an encapsulated response is waiting */
unsigned ls_usbnet_parse_notification(const uint8_t *buf, size_t len, bool *link,
                                      uint32_t *down_bps);

/* Frames shorter than an Ethernet header or longer than the MTU are dropped. */
static inline bool ls_usbnet_frame_ok(size_t len)
{
    return len >= LS_USBNET_FRAME_MIN && len <= LS_USBNET_FRAME_MAX;
}
/* A bulk OUT transfer that is an exact multiple of the max packet size needs a
   zero-length packet so the device sees where the frame ends. */
static inline bool ls_usbnet_tx_needs_zlp(size_t len, uint16_t mps)
{
    return mps && len && (len % mps) == 0;
}

/* Where an attached device goes. */
typedef enum {
    LS_USB_ROUTE_SDR = 0,     /* RTL-SDR path, as before USB Ethernet */
    LS_USB_ROUTE_HACKRF,
    LS_USB_ROUTE_NET,         /* the active configuration has ECM or RNDIS */
    LS_USB_ROUTE_NET_PROBE,   /* look for ECM in the other configurations
                                 (RTL8152/8153), then fall back to the SDR path */
} ls_usb_route_t;

ls_usb_route_t ls_usbnet_route(bool hackrf, uint8_t num_configs,
                               const uint8_t *active_cfg, size_t len, bool learned);

/* Configurations learned this boot: a device whose ECM function is not in the
   configuration it enumerated with is remembered by VID:PID and re-enumerated;
   the enumeration filter then picks that configuration. */
void ls_usbnet_learn_config(uint16_t vid, uint16_t pid, uint8_t config_value);
bool ls_usbnet_config_learned(uint16_t vid, uint16_t pid);
/* bConfigurationValue to enumerate with (1 unless learned). */
uint8_t ls_usbnet_pick_config(uint16_t vid, uint16_t pid, uint8_t num_configs);
void ls_usbnet_forget_configs(void);

/* RNDIS. Control messages go out with SEND_ENCAPSULATED_COMMAND and come back
   with GET_ENCAPSULATED_RESPONSE; data packets carry a 44-byte header. */
#define RNDIS_MSG_PACKET         0x00000001u
#define RNDIS_MSG_INIT           0x00000002u
#define RNDIS_MSG_QUERY          0x00000004u
#define RNDIS_MSG_SET            0x00000005u
#define RNDIS_MSG_INDICATE       0x00000007u
#define RNDIS_MSG_INIT_C         0x80000002u
#define RNDIS_MSG_QUERY_C        0x80000004u
#define RNDIS_MSG_SET_C          0x80000005u
#define RNDIS_OID_PERMANENT_ADDR 0x01010101u
#define RNDIS_OID_PACKET_FILTER  0x0001010Eu
#define RNDIS_OID_MEDIA_STATUS   0x00010114u
#define RNDIS_OID_LINK_SPEED     0x00010107u  /* units of 100 bit/s */
#define RNDIS_STATUS_MEDIA_CONNECT    0x4001000Bu
#define RNDIS_STATUS_MEDIA_DISCONNECT 0x4001000Cu
/* Directed, all-multicast and broadcast. */
#define RNDIS_FILTER_DEFAULT     0x0000000Du

size_t ls_rndis_build_init(uint8_t *buf, size_t cap, uint32_t req_id, uint32_t max_transfer);
size_t ls_rndis_build_query(uint8_t *buf, size_t cap, uint32_t req_id, uint32_t oid);
size_t ls_rndis_build_set_u32(uint8_t *buf, size_t cap, uint32_t req_id, uint32_t oid,
                              uint32_t value);
/* True when buf is the completion of type `type` for request `req_id`; sets
   *status (0 = success) and points *info at a QUERY_C information buffer. */
bool ls_rndis_parse_reply(const uint8_t *buf, size_t len, uint32_t type, uint32_t req_id,
                          uint32_t *status, const uint8_t **info, size_t *info_len);
/* REMOTE_NDIS_INDICATE_STATUS_MSG media connect/disconnect: 1 up, 0 down,
   -1 anything else. */
int ls_rndis_parse_indicate(const uint8_t *buf, size_t len);
/* Writes the 44-byte data header for a payload of `payload` bytes. */
void ls_rndis_packet_header(uint8_t hdr[LS_RNDIS_HEADER], size_t payload);

typedef void (*ls_usbnet_frame_cb)(const uint8_t *frame, size_t len, void *ctx);
/* Splits one bulk IN transfer into its packets; returns how many it
   delivered. Stops at the first malformed packet. */
unsigned ls_rndis_unwrap(const uint8_t *buf, size_t len, ls_usbnet_frame_cb cb, void *ctx);

#ifdef __cplusplus
}
#endif

#endif
