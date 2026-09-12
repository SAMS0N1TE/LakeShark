# components/imbe_vocoder - provenance

Fixed-point P25 IMBE encoder/decoder by Pavel Yazev, as carried in OP25.

    upstream: https://github.com/boatbod/op25
    path:     op25/gr-op25_repeater/lib/imbe_vocoder/
    licence:  GPL-3.0-or-later  (see COPYRIGHT)

**The exact upstream revision is not recorded** and should be pinned before
release. A local reference checkout exists at `vendor/op25` outside this
repository and was used to verify the header restorations below.

## Notices

Every upstream `.cc`/`.h` carries the full GPLv3-or-later header, except the
two files that upstream itself gives only the four-line attribution block:
`imbe_vocoder.cc` and `imbe_vocoder.h`. Those two are covered by the project
licence, and their attribution block must still be preserved.

Repaired 2026-09-07, verified character for character against `vendor/op25`:

- `imbe_vocoder.cc` - the attribution block had been **lost entirely**.
  Restored.
- `imbe_vocoder.h` - the block was present but corrupted into
  `* * Project 25...` / `*  * Developed by...` with stray asterisks and
  indentation. Replaced with the upstream text.

Neither file's code was changed by that repair.

## LakeShark original

    imbe_shim.cpp / imbe_shim.h   C wrapper around the decode path, so C code
                                  in components/lakeshark/apps/p25/ can call
                                  the C++ vocoder. GPL-3.0-or-later; carries an
                                  SPDX header.

`imbe_vocoder_impl.cc`, `imbe_vocoder_impl.h` and `imbe_vocoder_api.h` are the
OP25 implementation split as upstream ships it.

## Updating

Replace only from a reviewed OP25 revision, record its commit hash here, keep
every header including the two attribution-only ones, and re-run
`pwsh -File bench/verify.ps1 -Level smoke`.
