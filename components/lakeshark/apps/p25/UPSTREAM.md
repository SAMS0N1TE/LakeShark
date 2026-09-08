# components/lakeshark/apps/p25 — provenance

**This directory is not clear for public release.** It mixes LakeShark original
code with material derived from DSD, and several files that came from somewhere
carry no notice at all. Nothing here has been given invented attribution and
nothing should be.

## What is established

`dsd_dibit.c` retains, verbatim:

    Copyright (C) 2010 DSD Author
    GPG Key ID: 0x3F1D7FD0 (74EF 430D F7F2 0A48 FCE6  F630 FAA2 635D 3F1D 7FD0)

    Permission to use, copy, modify, and/or distribute this software for any
    purpose with or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.
    [... ISC-style disclaimer ...]

That is the licence of `szechyjs/dsd`. `CLAUDE.md` states the tree "descends
from DSD and mbelib". The README additionally credits
[dsd-fme](https://github.com/lwvmobile/dsd-fme), which is **not** under the
same terms — dsd-fme is GPL. Those two statements cannot both describe the
same files, and which upstream each file actually came from decides whether it
is ISC or GPL. That has to be settled by the author, not guessed.

## Files with no notice (scan of 2026-09-07)

Clearly DSD-lineage by name and by the `dsd_state` API they operate on, but
carrying no notice in this tree:

    dsd.h  dsd_filters.c  dsd_frame.c  dsd_frame_sync.c  dsd_main.c
    dsd_symbol.c

If these came from `szechyjs/dsd`, the ISC notice must be restored to each,
because that licence requires it "appear in all copies". If they came from
dsd-fme, they are GPL and need that header plus a section 2(a) notice of
change, since all of them are heavily modified here.

No notice **and** no traceable provenance:

    Golay24.hpp  Hamming.cpp  Hamming.hpp  ReedSolomon.hpp

These are not in the local `vendor/op25` checkout and match no file name there.
What can be said from the artifacts alone: the GUID-style include guards
(`GOLAY24_HPP_7a68240afda9406facf81fcad3851111`) and the `DSDGolay24 : public
Golay24` subclass point at a DSD-family C++ source, and `ReedSolomon.hpp` is a
template wrapper around the classic Reed-Solomon implementation — the
`generate_gf` / `generator_polinomial` spelling is the giveaway — whose own
terms have historically required the original author's name be kept in the
heading of the program. **That is inference, not evidence.** Do not write a
header into these files on the strength of it.

## What has to happen before release

1. The author identifies the upstream for each file above, with a revision.
2. Restore the applicable notice from that verified upstream, verbatim.
3. Where the upstream is GPL and the file was modified here, add a section 2(a)
   notice of change.
4. Confirm the licence chain to the GPLv3 root. GPL-2.0-**only** material would
   be incompatible with it; GPL-2.0-or-later and the ISC-style DSD terms are
   both fine.
5. Mark the LakeShark-original files in this directory with an SPDX header so
   the two are distinguishable, as was done in
   `components/lakeshark/radio/` (see its `UPSTREAM.md`).

Tracked as an open row in `docs/THIRD_PARTY_REVIEW.md`.
