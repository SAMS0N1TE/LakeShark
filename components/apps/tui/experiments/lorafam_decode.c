/* Wire facts: meshtastic/firmware src/mesh/RadioInterface.h,
   meshcore-dev/MeshCore src/Packet.{h,cpp}, zwave-js/core protocol/MPDU.ts
   (US classic header) and SDS13782 CRC, IEEE 802.15.4 SUN FSK PHR masks
   as documented by Wireshark packet-silabs-dch.c. Parsers are independent,
   bounded and return header guesses, never decrypted content. */
#include "lorafam_decode.h"
#include <string.h>
static uint32_t le32(const uint8_t *p)
{ return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24); }
bool lorafam_meshtastic(const uint8_t *p, size_t n, lorafam_mesh_t *out)
{
    if (!p || !out || n < 16 || n > 255 || !le32(p+4) || !le32(p+8)) return false;
    memset(out, 0, sizeof(*out));
    out->dest = le32(p); out->source = le32(p+4); out->id = le32(p+8);
    out->flags = p[12]; out->channel = p[13]; out->source_known = true;
    return true;
}
bool lorafam_meshcore(const uint8_t *p, size_t n, lorafam_mesh_t *out)
{
    if (!p || !out || n < 3 || n > 255 || (p[0] >> 6)) return false;
    const uint8_t route = p[0] & 3, type = (p[0] >> 2) & 15;
    const size_t path_at = (route == 0 || route == 3) ? 5 : 1;
    if (path_at >= n || (p[path_at] >> 6) == 3) return false;
    const size_t path_bytes = (p[path_at] & 63) * ((p[path_at] >> 6) + 1u);
    const size_t payload = path_at + 1 + path_bytes;
    if (path_bytes > 64 || payload >= n || n - payload > 184) return false;
    memset(out, 0, sizeof(*out)); out->route = route; out->type = type;
    /* Advertisements carry a public key; use its prefix as a bounded node
       key. Direct encrypted packets expose only a one-byte source hash. */
    if (type == 4 && n - payload >= 32) {
        out->source = le32(p+payload); out->source_known = true;
    } else if ((type == 0 || type == 1 || type == 2 || type == 8) && n - payload >= 4) {
        out->dest = p[payload]; out->source = p[payload+1];
        out->source_known = true; out->source_hash = true;
    }
    return true;
}
bool lorafam_zwave(const uint8_t *p, size_t n, uint8_t rate, lorafam_zwave_t *out)
{
    const size_t fcs = rate == 3 ? 2 : 1;
    if (!p || !out || rate < 1 || rate > 3 || n < 9+fcs || n > 255 || p[7] != n) return false;
    memset(out, 0, sizeof(*out));
    out->homeid = ((uint32_t)p[0]<<24) | ((uint32_t)p[1]<<16) | ((uint32_t)p[2]<<8) | p[3];
    out->source = p[4]; out->control = ((uint16_t)p[5]<<8) | p[6];
    out->type = p[5] & 15; out->length = p[7];
    /* Multicast has a node mask rather than a destination byte. */
    if (out->type == 1 || out->type == 3 || out->type == 5 || out->type == 8) {
        out->dest = p[8]; out->dest_known = true;
    }
    if (fcs == 1) {
        uint8_t sum = 0xff;
        for (size_t i = 0; i < n-1; i++) sum ^= p[i];
        out->checksum_ok = sum == p[n-1];
    } else {
        uint16_t crc = 0x1d0f;
        for (size_t i = 0; i < n-2; i++) {
            crc ^= (uint16_t)p[i] << 8;
            for (unsigned b = 0; b < 8; b++) crc = (uint16_t)((crc << 1) ^ ((crc & 0x8000) ? 0x1021 : 0));
        }
        out->checksum_ok = crc == (uint16_t)((p[n-2]<<8) | p[n-1]);
    }
    return true;
}
bool lorafam_wisun_phr(uint16_t raw, lorafam_phr_t *out)
{
    if (!out || (raw & 0x6000)) return false;
    out->mode_switch = !!(raw & 0x8000);
    out->length = out->mode_switch ? 0 : raw & 0x07ff;
    out->crc16 = !!(raw & 0x1000); out->whitening = !!(raw & 0x0800);
    return out->mode_switch || out->length >= (out->crc16 ? 2 : 4);
}
bool lorafam_wisun_mac(const uint8_t *p, size_t n, uint8_t *type)
{
    if (!p || !type || n < 2) return false;
    const uint16_t fcf = p[0] | ((uint16_t)p[1]<<8);
    const unsigned version = (fcf >> 12) & 3, dest = (fcf >> 10) & 3, src = (fcf >> 14) & 3;
    if (version > 2 || dest == 1 || src == 1 || (fcf & 7) > 3) return false;
    if (!(fcf & 0x100) && n < 3) return false;
    *type = fcf & 7;
    return true;
}
