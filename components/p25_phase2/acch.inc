// P25 TDMA Decoder (C) Copyright 2013, 2014, 2021 Max H. Parke KA1RBI
// Copyright 2017-2025 Graham J. Norbury
// 
// This file is part of OP25
// 
// OP25 is free software; you can redistribute it and/or modify it
// under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 3, or (at your option)
// any later version.
// 
// OP25 is distributed in the hope that it will be useful, but WITHOUT
// ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
// or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public
// License for more details.
// 
// You should have received a copy of the GNU General Public License
// along with OP25; see the file COPYING. If not, write to the Free
// Software Foundation, Inc., 51 Franklin Street, Boston, MA
// 02110-1301, USA.

static uint16_t crc12(const uint8_t bits[], unsigned int len) {
    uint16_t crc=0;
    static const unsigned int K = 12;
    static const uint8_t poly[K+1] = {1,1,0,0,0,1,0,0,1,0,1,1,1}; // p25 p2 crc 12 poly
    uint8_t buf[256];
    if (len+K > sizeof(buf)) {
        fprintf (stderr, "crc12: buffer length %u exceeds maximum %lu\n", len+K, sizeof(buf));
        return 0;
    }
    memset (buf, 0, sizeof(buf));
    for (unsigned int i=0; i<len; i++){
        buf[i] = bits[i];
    }
    for (unsigned int i=0; i<len; i++)
        if (buf[i])
            for (unsigned int j=0; j<K+1; j++)
                buf[i+j] ^= poly[j];
    for (unsigned int i=0; i<K; i++){
        crc = (crc << 1) + buf[len + i];
    }
    return crc ^ 0xfff;
}

static bool crc12_ok(const uint8_t bits[], unsigned int len) {
    uint16_t crc = 0;
    for (unsigned int i=0; i < 12; i++) {
        crc = (crc << 1) + bits[len+i];
    }
    return (crc == crc12(bits,len));
}

int decoder::handle_acch_frame(const uint8_t dibits[], bool fast, bool is_lcch) 
{
    int rc, rs_errs = 0;
    unsigned int i, j = 0;
    uint8_t bits[512];
    std::vector<uint8_t> HB(63,0);
    std::vector<int> Erasures;
    uint8_t byte_buf[32];
    unsigned int bufl=0;
    unsigned int len=0;
    if (fast) {
        for (i=11; i < 11+36; i++) {
            bits[bufl++] = (dibits[i] >> 1) & 1;
            bits[bufl++] = dibits[i] & 1;
        }
        for (i=48; i < 48+31; i++) {
            bits[bufl++] = (dibits[i] >> 1) & 1;
            bits[bufl++] = dibits[i] & 1;
        }
        for (i=100; i < 100+32; i++) {
            bits[bufl++] = (dibits[i] >> 1) & 1;
            bits[bufl++] = dibits[i] & 1;
        }
        for (i=133; i < 133+36; i++) {
            bits[bufl++] = (dibits[i] >> 1) & 1;
            bits[bufl++] = dibits[i] & 1;
        }
    } else {
        for (i=11; i < 11+36; i++) {
            bits[bufl++] = (dibits[i] >> 1) & 1;
            bits[bufl++] = dibits[i] & 1;
        }
        for (i=48; i < 48+84; i++) {
            bits[bufl++] = (dibits[i] >> 1) & 1;
            bits[bufl++] = dibits[i] & 1;
        }
        for (i=133; i < 133+36; i++) {
            bits[bufl++] = (dibits[i] >> 1) & 1;
            bits[bufl++] = dibits[i] & 1;
        }
    }

    // Reed-Solomon
    if (fast) {
        j = 9;
        len = 270;
        Erasures = {0,1,2,3,4,5,6,7,8,54,55,56,57,58,59,60,61,62};
    } else {
        j = 5;
        len = 312;
        Erasures = {0,1,2,3,4,57,58,59,60,61,62};
    }

    for (i = 0; i < len; i += 6) { // convert bits to hexbits
        HB[j] = (bits[i] << 5) + (bits[i+1] << 4) + (bits[i+2] << 3) + (bits[i+3] << 2) + (bits[i+4] << 1) + bits[i+5];
        j++;
    }
    rs_errs = rs28.decode(HB, Erasures);
    if (rs_errs < 0) // Don't need upper limit on corrections because pdu is crc12 checked before subsequent processing
        { stats.control_errors++; return -1; }

    // Adjust FEC error counter to eliminate erasures
    if (rs_errs >= (int)Erasures.size())
        rs_errs -= (int)Erasures.size();

    if (fast) {
        j = 9;
        len = 144;
    } else {
        j = 5;
        len = (is_lcch) ? 180 : 168;
    }
    for (i = 0; i < len; i += 6) { // convert hexbits back to bits
        bits[i]   = (HB[j] & 0x20) >> 5;
        bits[i+1] = (HB[j] & 0x10) >> 4;
        bits[i+2] = (HB[j] & 0x08) >> 3;
        bits[i+3] = (HB[j] & 0x04) >> 2;
        bits[i+4] = (HB[j] & 0x02) >> 1;
        bits[i+5] = (HB[j] & 0x01);
        j++;
    }

    bool crc_ok = (is_lcch) ? (crc16(bits, len) == 0) : crc12_ok(bits, len);
    unsigned int olen = (is_lcch) ? 23 : len/8;
    rc = -1;
    if (crc_ok) {
        for (i=0; i<olen; i++) {
            byte_buf[i] = (bits[i*8 + 0] << 7) + (bits[i*8 + 1] << 6) + (bits[i*8 + 2] << 5) + (bits[i*8 + 3] << 4) + (bits[i*8 + 4] << 3) + (bits[i*8 + 5] << 2) + (bits[i*8 + 6] << 1) + (bits[i*8 + 7] << 0);
        }
        rc = process_mac_pdu(byte_buf, olen, rs_errs);
    } else stats.control_errors++;
    return rc;
}

