/* mbelib's Hamming(15,11) decoders must put any single bit of a codeword
   back. Their table used to list the syndromes in bit order, which matches
   neither generator: an error in data bits 4..7 of an IMBE word stayed, and
   one in parity bit 3 moved to data bit 7. */
#include "ls_test.h"
#include "mbelib.h"

typedef int (*hamming_t)(char *in, char *out);

static void to_bits(int v, char *b)
{
    for (int i = 0; i < 15; i++) b[i] = (char)((v >> i) & 1);
}

static int from_bits(const char *b)
{
    int v = 0;
    for (int i = 0; i < 15; i++) v |= (b[i] & 1) << i;
    return v;
}

/* the codeword for these 11 data bits: the parity the decoder finds no fault in */
static int codeword(hamming_t dec, int data)
{
    char in[15], out[15];
    for (int p = 0; p < 16; p++) {
        const int v = (data << 4) | p;
        to_bits(v, in);
        if (dec(in, out) == 0) return v;
    }
    return -1;
}

static int unfixed_single_errors(hamming_t dec)
{
    int bad = 0;
    for (int data = 0; data < 2048; data += 7) {
        const int cw = codeword(dec, data);
        if (cw < 0) return -1;
        for (int b = 0; b < 15; b++) {
            char in[15], out[15];
            to_bits(cw ^ (1 << b), in);
            dec(in, out);
            if (from_bits(out) != cw) bad++;
        }
    }
    return bad;
}

LS_CASE(the_imbe_7200_hamming_word_survives_any_single_bit_error)
{
    LS_EQ_INT(0, unfixed_single_errors(mbe_hamming1511));
}

LS_CASE(the_imbe_7100_hamming_word_survives_any_single_bit_error)
{
    LS_EQ_INT(0, unfixed_single_errors(mbe_7100x4400hamming1511));
}
