/* 802.11 frame reading for apps over lap_wifi_capture_* (API 2.7): the MAC
   header, management bodies and their tagged elements, security suites,
   deauthentication reasons and EAPOL-Key handshake messages.

   Receive only and read only. Nothing here builds a frame, and key material
   is never copied out: an EAPOL-Key frame reports which handshake message
   it is and whether a PMKID is present, nothing more.

   Input is the frame as lap_capture_read gives it: MAC header first, no
   radiotap, no FCS. No allocation; every result lives in lap_dot11_t. */
#ifndef LAP_DOT11_H
#define LAP_DOT11_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { LAP_DOT11_MGMT = 0, LAP_DOT11_CTRL = 1, LAP_DOT11_DATA = 2, LAP_DOT11_EXT = 3 };
/* Management subtypes. */
enum {
    LAP_DOT11_ASSOC_REQ = 0, LAP_DOT11_ASSOC_RESP = 1, LAP_DOT11_REASSOC_REQ = 2, LAP_DOT11_REASSOC_RESP = 3,
    LAP_DOT11_PROBE_REQ = 4, LAP_DOT11_PROBE_RESP = 5, LAP_DOT11_BEACON = 8, LAP_DOT11_ATIM = 9,
    LAP_DOT11_DISASSOC = 10, LAP_DOT11_AUTH = 11, LAP_DOT11_DEAUTH = 12, LAP_DOT11_ACTION = 13
};
/* What protects a network, from the RSN and WPA elements and the privacy bit. */
enum {
    LAP_DOT11_SEC_UNKNOWN = 0, LAP_DOT11_SEC_OPEN, LAP_DOT11_SEC_WEP, LAP_DOT11_SEC_WPA,
    LAP_DOT11_SEC_WPA2, LAP_DOT11_SEC_WPA2_EAP, LAP_DOT11_SEC_WPA2_WPA3, LAP_DOT11_SEC_WPA3,
    LAP_DOT11_SEC_OWE, LAP_DOT11_SEC_COUNT
};
/* AKM bits. */
#define LAP_DOT11_AKM_8021X 0x01u
#define LAP_DOT11_AKM_PSK   0x02u
#define LAP_DOT11_AKM_SAE   0x04u
#define LAP_DOT11_AKM_OWE   0x08u
#define LAP_DOT11_AKM_FT    0x10u
#define LAP_DOT11_AKM_OTHER 0x80u

typedef struct {
    bool ok;                    /* a header was read; false: too short to be a frame */
    uint8_t type, subtype;
    bool to_ds, from_ds, retry, protect, qos;
    uint16_t duration, seq;
    uint8_t hdr_len;
    uint8_t naddr;              /* addresses present: 1 (CTS, ACK), 2, 3 or 4 */
    uint8_t a1[6], a2[6], a3[6], a4[6];
    /* who is who, where the header says it */
    uint8_t ra[6], ta[6], bssid[6];
    bool have_ta, have_bssid;

    /* management bodies */
    uint16_t reason;            /* deauthentication, disassociation */
    uint16_t capab, interval;   /* beacon, probe response, association */
    uint16_t auth_alg, auth_seq, status;
    /* tagged elements */
    bool ssid_present;          /* an SSID element was there */
    bool ssid_hidden;           /* ... empty or all zero bytes */
    uint8_t ssid_len;
    char ssid[33];              /* printable: anything else became '.' */
    uint8_t channel;            /* DS parameter set or HT operation; 0 none */
    uint8_t security;           /* LAP_DOT11_SEC_* (beacons, probe responses, association requests) */
    uint8_t akm;                /* LAP_DOT11_AKM_* */
    bool rsn, wpa;
    bool rsn_pmkid;             /* an RSN element listing a PMKID (association requests) */

    /* data frames */
    bool eapol;                 /* LLC/SNAP EtherType 0x888E in the clear */
    uint8_t eapol_type;         /* 802.1X packet type: 0 EAP, 1 start, 2 logoff, 3 key */
    uint8_t eapol_msg;          /* EAPOL-Key: 1..4 for the 4-way handshake, 0 group or unknown */
    bool eapol_pmkid;           /* EAPOL-Key key data carries a PMKID KDE (present; never copied) */
    uint16_t ethertype;         /* LLC/SNAP in the clear; 0 none */
} lap_dot11_t;

/* Reads n bytes of one frame. False when there is no header to read. */
bool lap_dot11_parse(const uint8_t *frame, size_t n, lap_dot11_t *out);

const char *lap_dot11_subtype_name(int type, int subtype);
const char *lap_dot11_sec_name(int security);       /* "open", "WPA2", ... */
const char *lap_dot11_reason_name(unsigned reason); /* deauthentication reason codes, short */
/* aa:bb:cc:dd:ee:ff; needs 18 bytes. */
void lap_dot11_mac(char *out, size_t cap, const uint8_t mac[6]);
bool lap_dot11_is_broadcast(const uint8_t mac[6]);
bool lap_dot11_is_multicast(const uint8_t mac[6]);
/* Locally administered: how phones randomise the address they probe with. */
bool lap_dot11_is_random(const uint8_t mac[6]);
/* 2.4 GHz 1..14 and 5 GHz 32..177 to MHz, and back; 0 when not a channel. */
unsigned lap_dot11_channel_mhz(unsigned channel);
unsigned lap_dot11_mhz_channel(unsigned mhz);
/* Microseconds a frame of `bytes` (no FCS) takes on air at rate_500kbps
   (0 unknown: 6 Mb/s OFDM is assumed), preamble included. */
uint32_t lap_dot11_airtime_us(unsigned bytes, unsigned rate_500kbps, unsigned channel);

#ifdef __cplusplus
}
#endif
#endif
