/*
 * DSD-derived source, modified for LakeShark.
 * Comparison source: https://github.com/szechyjs/dsd/blob/59423fa46be8b41ef0bd2f3d2b45590600be29f0/src/Hamming.cpp
 * Original import revision is unrecorded.
 *
 * Copyright (C) 2010 DSD Author
 * GPG Key ID: 0x3F1D7FD0 (74EF 430D F7F2 0A48 FCE6  F630 FAA2 635D 3F1D 7FD0)
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND ISC DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS.  IN NO EVENT SHALL ISC BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE
 * OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */


#include "Hamming.hpp"

Hamming_10_6_3_data Hamming_10_6_3::data;

Hamming_10_6_3_TableImpl_data Hamming_10_6_3_TableImpl::data;

int Hamming_10_6_3::decode(std::bitset<10>& input)
{
    int error_count;

    int s0 = ((data.h0 & input).count() & 1) << 3;
    int s1 = ((data.h1 & input).count() & 1) << 2;
    int s2 = ((data.h2 & input).count() & 1) << 1;
    int s3 = ((data.h3 & input).count() & 1);
    int parity = s0 | s1 | s2 | s3;

    if (parity == 0) {

        error_count = 0;
    } else {

        int bad_bit_index = data.bad_bit_table[parity];

        if (bad_bit_index < 0) {

            error_count = 2;
        } else {

            if (bad_bit_index < 4) {

                error_count = 1;
            } else {
                input.flip(bad_bit_index);

                error_count = 1;
            }
        }
    }

    return error_count;
}

int Hamming_10_6_3::encode(std::bitset<6>& input)
{

    int s0 = ((data.gt0 & input).count() & 1) << 3;
    int s1 = ((data.gt1 & input).count() & 1) << 2;
    int s2 = ((data.gt2 & input).count() & 1) << 1;
    int s3 = ((data.gt3 & input).count() & 1);
    int parity = s0 | s1 | s2 | s3;

    return parity;
}

int Hamming_10_6_3_TableImpl::decode(int input, int* output)
{
    assert (input < 1024 && input >= 0);

    *output = data.fixed_values[input];

    return data.error_counts[input];
}

int Hamming_10_6_3_TableImpl::encode(int input)
{
    assert (input < 64 && input >= 0);

    return data.encode_parities[input];
}
