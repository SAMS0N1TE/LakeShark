# components/mbelib - provenance

IMBE 7200x4400 voice decoding for P25 Phase 1.

    upstream: https://github.com/szechyjs/mbelib
    licence:  ISC-style permission notice (see COPYRIGHT)

Every source and header in this directory retains the complete
"Copyright (C) 2010 mbelib Author" permission notice, which the licence
requires to appear in all copies. Verified 2026-09-07: no file is missing it.

**Modified.** Compared against the same subset in the sibling OrcSDR project,
which pins upstream commit `9a04ed5c78176a9965f3d43f7aa1b1f5330e771f`:

    ecc.c                    identical
    ecc_const.h              identical
    mbelib_const.h           identical
    mbelib.c                 differs
    mbelib.h                 differs
    imbe7200x4400.c          differs
    imbe7200x4400_const.h    differs

So this tree is neither a clean copy of that commit nor pinned to any recorded
revision. **Pin the actual upstream revision and describe the local changes
before release.** The ISC terms do not require a notice of change, but shipping
modified code with no record of the base revision makes the source obligation
on the GPL side of the build impossible to satisfy honestly.

**Local fix (2026-10-04):** `hammingMatrix` in `ecc_const.h` listed the
Hamming(15,11) syndromes in bit order, which matches neither generator: for
IMBE 7200x4400 an error in data bits 4..7 of c4..c6 was never corrected and
one in parity bit 3 was moved into data bit 7. It now holds the bit each
syndrome of `hammingGenerator` points at, and `imbe7100x4400hammingMatrix`
does the same for the 7100x4400 generator, used by
`mbe_7100x4400hamming1511`. *Test:* `test_mbe_hamming`.

The directory also carries AMBE tables (`ambe3600x2400_const.h`,
`ambe3600x2450_const.h`) that the current CMake does not compile. Source
inclusion is not the same as linked functionality; keep the distinction when
describing what ships.

## Patent notice

IMBE and AMBE are covered by patents held by Digital Voice Systems, Inc. A
software licence is not a patent licence.

## Updating

Replace files only from a reviewed mbelib revision, record its commit hash
here, keep every permission notice, and re-run
`pwsh -File bench/verify.ps1 -Level smoke`.
