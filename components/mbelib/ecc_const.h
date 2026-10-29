/*
 * Copyright (C) 2010 mbelib Author
 * GPG Key ID: 0xEA5EFE2C (9E7A 5527 9CDC EBF7 BF1B  D772 4F98 E863 EA5E FE2C)
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

/*
 * constants for ecc.c
 */
#ifndef _ECC_H
#define _ECC_H

const int hammingGenerator[4] = {
  0x7f08, 0x78e4, 0x66d2, 0x55b1
};

const int imbe7100x4400hammingGenerator[4] = {
  0x7ac8, 0x3d64, 0x1eb2, 0x7591
};

/* The bit each syndrome points at, for each generator above. LakeShark: the
   table this replaces listed the syndromes in bit order, which matches
   neither generator. */
const int hammingMatrix[16] = {
  0x0, 0x1, 0x2, 0x10, 0x4, 0x20, 0x40, 0x80, 0x8, 0x100, 0x200, 0x400, 0x800, 0x1000, 0x2000, 0x4000
};

const int imbe7100x4400hammingMatrix[16] = {
  0x0, 0x1, 0x2, 0x10, 0x4, 0x100, 0x20, 0x400, 0x8, 0x4000, 0x200, 0x80, 0x40, 0x2000, 0x800, 0x1000
};

const int golayGenerator[12] = {
  0x63a, 0x31d, 0x7b4, 0x3da, 0x1ed, 0x6cc, 0x366, 0x1b3, 0x6e3, 0x54b, 0x49f, 0x475
};

/* golayMatrix (syndrome -> data error pattern) is built at start-up: ecc.c. */

#endif
