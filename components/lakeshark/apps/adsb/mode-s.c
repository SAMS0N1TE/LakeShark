#include "mode-s.h"
#include "esp_attr.h"

#define MODE_S_PREAMBLE_US 8
#define MODE_S_LONG_MSG_BITS 112
#define MODE_S_SHORT_MSG_BITS 56
#define MODE_S_FULL_LEN (MODE_S_PREAMBLE_US + MODE_S_LONG_MSG_BITS)

#define MODE_S_ICAO_CACHE_TTL 60

static EXT_RAM_BSS_ATTR uint16_t maglut[129 * 129 * 2];
static int maglut_initialized = 0;

void mode_s_init(mode_s_t *self)
{
    int i, q;

    self->fix_errors = 1;
    self->check_crc = 1;
    self->aggressive = 0;
    self->on_preamble = NULL;

    memset(&self->icao_cache, 0, sizeof(self->icao_cache));

    if (!maglut_initialized)
    {
        for (i = 0; i <= 128; i++)
        {
            for (q = 0; q <= 128; q++)
            {
                maglut[i * 129 + q] = round(sqrt(i * i + q * q) * 360);
            }
        }
        maglut_initialized = 1;
    }
}

uint32_t mode_s_checksum_table[] = {
    0x3935ea, 0x1c9af5, 0xf1b77e, 0x78dbbf, 0xc397db, 0x9e31e9, 0xb0e2f0, 0x587178,
    0x2c38bc, 0x161c5e, 0x0b0e2f, 0xfa7d13, 0x82c48d, 0xbe9842, 0x5f4c21, 0xd05c14,
    0x682e0a, 0x341705, 0xe5f186, 0x72f8c3, 0xc68665, 0x9cb936, 0x4e5c9b, 0xd8d449,
    0x939020, 0x49c810, 0x24e408, 0x127204, 0x093902, 0x049c81, 0xfdb444, 0x7eda22,
    0x3f6d11, 0xe04c8c, 0x702646, 0x381323, 0xe3f395, 0x8e03ce, 0x4701e7, 0xdc7af7,
    0x91c77f, 0xb719bb, 0xa476d9, 0xadc168, 0x56e0b4, 0x2b705a, 0x15b82d, 0xf52612,
    0x7a9309, 0xc2b380, 0x6159c0, 0x30ace0, 0x185670, 0x0c2b38, 0x06159c, 0x030ace,
    0x018567, 0xff38b7, 0x80665f, 0xbfc92b, 0xa01e91, 0xaff54c, 0x57faa6, 0x2bfd53,
    0xea04ad, 0x8af852, 0x457c29, 0xdd4410, 0x6ea208, 0x375104, 0x1ba882, 0x0dd441,
    0xf91024, 0x7c8812, 0x3e4409, 0xe0d800, 0x706c00, 0x383600, 0x1c1b00, 0x0e0d80,
    0x0706c0, 0x038360, 0x01c1b0, 0x00e0d8, 0x00706c, 0x003836, 0x001c1b, 0xfff409,
    0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000,
    0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000,
    0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000};

uint32_t mode_s_checksum(unsigned char *msg, int bits)
{
    uint32_t crc = 0;
    int offset = (bits == 112) ? 0 : (112 - 56);
    int j;

    for (j = 0; j < bits; j++)
    {
        int byte = j / 8;
        int bit = j % 8;
        int bitmask = 1 << (7 - bit);

        if (msg[byte] & bitmask)
            crc ^= mode_s_checksum_table[j + offset];
    }
    return crc;
}

int mode_s_msg_len_by_type(int type)
{
    if (type == 16 || type == 17 ||
        type == 19 || type == 20 ||
        type == 21)
        return MODE_S_LONG_MSG_BITS;
    else
        return MODE_S_SHORT_MSG_BITS;
}

int fix_single_bit_errors(unsigned char *msg, int bits)
{
    int j;
    unsigned char aux[MODE_S_LONG_MSG_BITS / 8];

    for (j = 0; j < bits; j++)
    {
        int byte = j / 8;
        int bitmask = 1 << (7 - (j % 8));
        uint32_t crc1, crc2;

        memcpy(aux, msg, bits / 8);
        aux[byte] ^= bitmask;

        crc1 = ((uint32_t)aux[(bits / 8) - 3] << 16) |
               ((uint32_t)aux[(bits / 8) - 2] << 8) |
               (uint32_t)aux[(bits / 8) - 1];
        crc2 = mode_s_checksum(aux, bits);

        if (crc1 == crc2)
        {

            memcpy(msg, aux, bits / 8);
            return j;
        }
    }
    return -1;
}

int fix_two_bits_errors(unsigned char *msg, int bits)
{
    int j, i;
    unsigned char aux[MODE_S_LONG_MSG_BITS / 8];

    for (j = 0; j < bits; j++)
    {
        int byte1 = j / 8;
        int bitmask1 = 1 << (7 - (j % 8));

        for (i = j + 1; i < bits; i++)
        {
            int byte2 = i / 8;
            int bitmask2 = 1 << (7 - (i % 8));
            uint32_t crc1, crc2;

            memcpy(aux, msg, bits / 8);

            aux[byte1] ^= bitmask1;
            aux[byte2] ^= bitmask2;

            crc1 = ((uint32_t)aux[(bits / 8) - 3] << 16) |
                   ((uint32_t)aux[(bits / 8) - 2] << 8) |
                   (uint32_t)aux[(bits / 8) - 1];
            crc2 = mode_s_checksum(aux, bits);

            if (crc1 == crc2)
            {

                memcpy(msg, aux, bits / 8);

                return j | (i << 8);
            }
        }
    }
    return -1;
}

uint32_t icao_cache_has_addr(uint32_t a)
{

    a = ((a >> 16) ^ a) * 0x45d9f3b;
    a = ((a >> 16) ^ a) * 0x45d9f3b;
    a = ((a >> 16) ^ a);
    return a & (MODE_S_ICAO_CACHE_LEN - 1);
}

void add_recently_seen_icao_addr(mode_s_t *self, uint32_t addr)
{
    uint32_t h = icao_cache_has_addr(addr);
    self->icao_cache[h * 2] = addr;
    self->icao_cache[h * 2 + 1] = (uint32_t)time(NULL);
}

int icao_addr_was_recently_seen(mode_s_t *self, uint32_t addr)
{
    uint32_t h = icao_cache_has_addr(addr);
    uint32_t a = self->icao_cache[h * 2];
    int32_t t = self->icao_cache[h * 2 + 1];

    return a && a == addr && time(NULL) - t <= MODE_S_ICAO_CACHE_TTL;
}

int brute_force_ap(mode_s_t *self, unsigned char *msg, struct mode_s_msg *mm)
{
    unsigned char aux[MODE_S_LONG_MSG_BYTES];
    int msgtype = mm->msgtype;
    int msgbits = mm->msgbits;

    if (msgtype == 0 ||
        msgtype == 4 ||
        msgtype == 5 ||
        msgtype == 16 ||
        msgtype == 20 ||
        msgtype == 21 ||
        msgtype == 24)
    {
        uint32_t addr;
        uint32_t crc;
        int lastbyte = (msgbits / 8) - 1;

        memcpy(aux, msg, msgbits / 8);

        crc = mode_s_checksum(aux, msgbits);
        aux[lastbyte] ^= crc & 0xff;
        aux[lastbyte - 1] ^= (crc >> 8) & 0xff;
        aux[lastbyte - 2] ^= (crc >> 16) & 0xff;

        addr = aux[lastbyte] | (aux[lastbyte - 1] << 8) | (aux[lastbyte - 2] << 16);
        if (icao_addr_was_recently_seen(self, addr))
        {
            mm->aa1 = aux[lastbyte - 2];
            mm->aa2 = aux[lastbyte - 1];
            mm->aa3 = aux[lastbyte];
            return 1;
        }
    }
    return 0;
}

int decode_ac13_field(unsigned char *msg, int *unit)
{
    int m_bit = msg[3] & (1 << 6);
    int q_bit = msg[3] & (1 << 4);

    if (!m_bit)
    {
        *unit = MODE_S_UNIT_FEET;
        if (q_bit)
        {

            int n = ((msg[2] & 31) << 6) |
                    ((msg[3] & 0x80) >> 2) |
                    ((msg[3] & 0x20) >> 1) |
                    (msg[3] & 15);

            return n * 25 - 1000;
        }
        else
        {

        }
    }
    else
    {
        *unit = MODE_S_UNIT_METERS;

    }
    return 0;
}

int decode_ac12_field(unsigned char *msg, int *unit)
{
    int q_bit = msg[5] & 1;

    if (q_bit)
    {

        *unit = MODE_S_UNIT_FEET;
        int n = ((msg[5] >> 1) << 4) | ((msg[6] & 0xF0) >> 4);

        return n * 25 - 1000;
    }
    else
    {
        return 0;
    }
}

static const char *ais_charset = "?ABCDEFGHIJKLMNOPQRSTUVWXYZ????? ???????????????0123456789??????";

void mode_s_decode(mode_s_t *self, struct mode_s_msg *mm, unsigned char *msg)
{
    uint32_t crc2;

    memcpy(mm->msg, msg, MODE_S_LONG_MSG_BYTES);
    msg = mm->msg;

    mm->msgtype = msg[0] >> 3;
    mm->msgbits = mode_s_msg_len_by_type(mm->msgtype);

    mm->crc = ((uint32_t)msg[(mm->msgbits / 8) - 3] << 16) |
              ((uint32_t)msg[(mm->msgbits / 8) - 2] << 8) |
              (uint32_t)msg[(mm->msgbits / 8) - 1];
    crc2 = mode_s_checksum(msg, mm->msgbits);

    mm->errorbit = -1;
    mm->crcok = (mm->crc == crc2);

    if (!mm->crcok && self->fix_errors && (mm->msgtype == 11 || mm->msgtype == 17))
    {
        if ((mm->errorbit = fix_single_bit_errors(msg, mm->msgbits)) != -1)
        {
            mm->crc = mode_s_checksum(msg, mm->msgbits);
            mm->crcok = 1;
        }
        else if (self->aggressive && mm->msgtype == 17 &&
                 (mm->errorbit = fix_two_bits_errors(msg, mm->msgbits)) != -1)
        {
            mm->crc = mode_s_checksum(msg, mm->msgbits);
            mm->crcok = 1;
        }
    }

    mm->ca = msg[0] & 7;

    mm->aa1 = msg[1];
    mm->aa2 = msg[2];
    mm->aa3 = msg[3];

    mm->metype = msg[4] >> 3;
    mm->mesub = msg[4] & 7;

    mm->fs = msg[0] & 7;
    mm->dr = msg[1] >> 3 & 31;
    mm->um = ((msg[1] & 7) << 3) |
             msg[2] >> 5;

    {
        int a, b, c, d;

        a = ((msg[3] & 0x80) >> 5) |
            ((msg[2] & 0x02) >> 0) |
            ((msg[2] & 0x08) >> 3);
        b = ((msg[3] & 0x02) << 1) |
            ((msg[3] & 0x08) >> 2) |
            ((msg[3] & 0x20) >> 5);
        c = ((msg[2] & 0x01) << 2) |
            ((msg[2] & 0x04) >> 1) |
            ((msg[2] & 0x10) >> 4);
        d = ((msg[3] & 0x01) << 2) |
            ((msg[3] & 0x04) >> 1) |
            ((msg[3] & 0x10) >> 4);
        mm->identity = a * 1000 + b * 100 + c * 10 + d;
    }

    if (mm->msgtype != 11 && mm->msgtype != 17)
    {

        if (brute_force_ap(self, msg, mm))
        {

            mm->crcok = 1;
        }
        else
        {
            mm->crcok = 0;
        }
    }
    else
    {

        if (mm->crcok && mm->errorbit == -1)
        {
            uint32_t addr = (mm->aa1 << 16) | (mm->aa2 << 8) | mm->aa3;
            add_recently_seen_icao_addr(self, addr);
        }
    }

    if (mm->msgtype == 0 || mm->msgtype == 4 ||
        mm->msgtype == 16 || mm->msgtype == 20)
    {
        mm->altitude = decode_ac13_field(msg, &mm->unit);
    }

    if (mm->msgtype == 17)
    {

        if (mm->metype >= 1 && mm->metype <= 4)
        {

            mm->aircraft_type = mm->metype - 1;
            mm->flight[0] = (ais_charset)[msg[5] >> 2];
            mm->flight[1] = ais_charset[((msg[5] & 3) << 4) | (msg[6] >> 4)];
            mm->flight[2] = ais_charset[((msg[6] & 15) << 2) | (msg[7] >> 6)];
            mm->flight[3] = ais_charset[msg[7] & 63];
            mm->flight[4] = ais_charset[msg[8] >> 2];
            mm->flight[5] = ais_charset[((msg[8] & 3) << 4) | (msg[9] >> 4)];
            mm->flight[6] = ais_charset[((msg[9] & 15) << 2) | (msg[10] >> 6)];
            mm->flight[7] = ais_charset[msg[10] & 63];
            mm->flight[8] = '\0';
        }
        else if (mm->metype >= 9 && mm->metype <= 18)
        {

            mm->fflag = msg[6] & (1 << 2);
            mm->tflag = msg[6] & (1 << 3);
            mm->altitude = decode_ac12_field(msg, &mm->unit);
            mm->raw_latitude = ((msg[6] & 3) << 15) |
                               (msg[7] << 7) |
                               (msg[8] >> 1);
            mm->raw_longitude = ((msg[8] & 1) << 16) |
                                (msg[9] << 8) |
                                msg[10];
        }
        else if (mm->metype == 19 && mm->mesub >= 1 && mm->mesub <= 4)
        {

            if (mm->mesub == 1 || mm->mesub == 2)
            {
                mm->ew_dir = (msg[5] & 4) >> 2;
                mm->ew_velocity = ((msg[5] & 3) << 8) | msg[6];
                mm->ns_dir = (msg[7] & 0x80) >> 7;
                mm->ns_velocity = ((msg[7] & 0x7f) << 3) | ((msg[8] & 0xe0) >> 5);
                mm->vert_rate_source = (msg[8] & 0x10) >> 4;
                mm->vert_rate_sign = (msg[8] & 0x8) >> 3;
                mm->vert_rate = ((msg[8] & 7) << 6) | ((msg[9] & 0xfc) >> 2);

                mm->velocity = sqrt(mm->ns_velocity * mm->ns_velocity +
                                    mm->ew_velocity * mm->ew_velocity);
                if (mm->velocity)
                {
                    int ewv = mm->ew_velocity;
                    int nsv = mm->ns_velocity;
                    double heading;

                    if (mm->ew_dir)
                        ewv *= -1;
                    if (mm->ns_dir)
                        nsv *= -1;
                    heading = atan2(ewv, nsv);

                    mm->heading = heading * 360 / (M_PI * 2);

                    if (mm->heading < 0)
                        mm->heading += 360;
                }
                else
                {
                    mm->heading = 0;
                }
            }
            else if (mm->mesub == 3 || mm->mesub == 4)
            {
                mm->heading_is_valid = msg[5] & (1 << 2);
                mm->heading = (360.0 / 128) * (((msg[5] & 3) << 5) |
                                               (msg[6] >> 3));
            }
        }
    }
    mm->phase_corrected = 0;
}

void mode_s_compute_magnitude_vector(unsigned char *data, uint16_t *mag, uint32_t size)
{
    uint32_t j;

    for (j = 0; j < size; j += 2)
    {
        int i = data[j] - 127;
        int q = data[j + 1] - 127;

        if (i < 0)
            i = -i;
        if (q < 0)
            q = -q;
        mag[j / 2] = maglut[i * 129 + q];
    }
}

int detect_out_of_phase(uint16_t *mag)
{
    if (mag[3] > mag[2] / 3)
        return 1;
    if (mag[10] > mag[9] / 3)
        return 1;
    if (mag[6] > mag[7] / 3)
        return -1;
    if (mag[-1] > mag[1] / 3)
        return -1;
    return 0;
}

void apply_phase_correction(uint16_t *mag)
{
    int j;

    mag += 16;
    for (j = 0; j < (MODE_S_LONG_MSG_BITS - 1) * 2; j += 2)
    {
        if (mag[j] > mag[j + 1])
        {

            mag[j + 2] = (mag[j + 2] * 5) / 4;
        }
        else
        {

            mag[j + 2] = (mag[j + 2] * 4) / 5;
        }
    }
}

void mode_s_detect(mode_s_t *self, uint16_t *mag, uint32_t maglen, mode_s_callback_t cb)
{
    unsigned char bits[MODE_S_LONG_MSG_BITS];
    unsigned char msg[MODE_S_LONG_MSG_BITS / 2];
    uint16_t aux[MODE_S_LONG_MSG_BITS * 2];
    uint32_t j;
    int use_correction = 0;

    for (j = 0; j < maglen - MODE_S_FULL_LEN * 2; j++)
    {
        int low, high, delta, i, errors;
        int good_message = 0;

        if (use_correction)
            goto good_preamble;

        if (!(mag[j] > mag[j + 1] &&
              mag[j + 1] < mag[j + 2] &&
              mag[j + 2] > mag[j + 3] &&
              mag[j + 3] < mag[j] &&
              mag[j + 4] < mag[j] &&
              mag[j + 5] < mag[j] &&
              mag[j + 6] < mag[j] &&
              mag[j + 7] > mag[j + 8] &&
              mag[j + 8] < mag[j + 9] &&
              mag[j + 9] > mag[j + 6]))
        {
            continue;
        }

        high = (mag[j] + mag[j + 2] + mag[j + 7] + mag[j + 9]) / 6;
        if (mag[j + 4] >= high ||
            mag[j + 5] >= high)
        {
            continue;
        }

        if (mag[j + 11] >= high ||
            mag[j + 12] >= high ||
            mag[j + 13] >= high ||
            mag[j + 14] >= high)
        {
            continue;
        }

    good_preamble:

        if (!use_correction && self->on_preamble) self->on_preamble();

        if (use_correction)
        {
            memcpy(aux, mag + j + MODE_S_PREAMBLE_US * 2, sizeof(aux));
            if (j && detect_out_of_phase(mag + j))
            {
                apply_phase_correction(mag + j);
            }

        }

        errors = 0;
        for (i = 0; i < MODE_S_LONG_MSG_BITS * 2; i += 2)
        {
            low = mag[j + i + MODE_S_PREAMBLE_US * 2];
            high = mag[j + i + MODE_S_PREAMBLE_US * 2 + 1];
            delta = low - high;
            if (delta < 0)
                delta = -delta;

            if (i > 0 && delta < 256)
            {
                bits[i / 2] = bits[i / 2 - 1];
            }
            else if (low == high)
            {

                bits[i / 2] = 2;
                if (i < MODE_S_SHORT_MSG_BITS * 2)
                    errors++;
            }
            else if (low > high)
            {
                bits[i / 2] = 1;
            }
            else
            {

                bits[i / 2] = 0;
            }
        }

        if (use_correction)
            memcpy(mag + j + MODE_S_PREAMBLE_US * 2, aux, sizeof(aux));

        for (i = 0; i < MODE_S_LONG_MSG_BITS; i += 8)
        {
            msg[i / 8] =
                bits[i] << 7 |
                bits[i + 1] << 6 |
                bits[i + 2] << 5 |
                bits[i + 3] << 4 |
                bits[i + 4] << 3 |
                bits[i + 5] << 2 |
                bits[i + 6] << 1 |
                bits[i + 7];
        }

        int msgtype = msg[0] >> 3;
        int msglen = mode_s_msg_len_by_type(msgtype) / 8;

        delta = 0;
        for (i = 0; i < msglen * 8 * 2; i += 2)
        {
            delta += abs(mag[j + i + MODE_S_PREAMBLE_US * 2] -
                         mag[j + i + MODE_S_PREAMBLE_US * 2 + 1]);
        }
        delta /= msglen * 4;

        if (delta < 10 * 255)
        {
            use_correction = 0;
            continue;
        }

        if (errors == 0 || (self->aggressive && errors < 3))
        {
            struct mode_s_msg mm;

            mode_s_decode(self, &mm, msg);

            if (mm.crcok)
            {
                j += (MODE_S_PREAMBLE_US + (msglen * 8)) * 2;
                good_message = 1;
                if (use_correction)
                    mm.phase_corrected = 1;
            }

            if (self->check_crc == 0 || mm.crcok)
            {
                cb(self, &mm);
            }
        }

        if (!good_message && !use_correction)
        {
            j--;
            use_correction = 1;
        }
        else
        {
            use_correction = 0;
        }
    }
}