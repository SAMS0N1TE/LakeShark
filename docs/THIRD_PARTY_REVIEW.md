# Third-party source inventory

LakeShark uses the root GPLv3 license together with the notices retained in each imported component. Distributions must include the corresponding modified sources and applicable notices.

| Component | Notice location | Terms |
| --- | --- | --- |
| RTL-SDR and tuner drivers | `components/lakeshark/radio/COPYRIGHT.librtlsdr` and per-file headers | GPL-2.0-or-later |
| IMBE vocoder | `components/imbe_vocoder/COPYRIGHT` and per-file headers | GPL-3.0-or-later |
| mbelib | `components/mbelib/COPYRIGHT` | ISC-style permission notice |
| DSD core and helpers | P25 source headers and `components/lakeshark/apps/p25/UPSTREAM.md` | ISC-style DSD permission notice and additional per-file helper notices |
| MeshCore and ed25519 | `components/meshcore/` notices and upstream notices | Preserve each component's license |
| libcarto, PMTiles and TUI library | Notices within their component directories | Preserve each component's license |
| DejaVu fonts | `components/apps/tui/DEJAVU-LICENSE.txt` | Bitstream Vera and DejaVu terms |
| LVGL | `managed_components/lvgl__lvgl/LICENCE.txt` and nested notices | MIT and applicable nested notices |
| Espressif and Waveshare components | Their component license files and source headers | Primarily Apache-2.0; retain nested notices |
| Audio player and file iterator | Their component notices and source headers | Apache-2.0 |

The LakeShark audio-player configuration provides WAV playback without the Helix MP3 decoder. The public source export excludes Helix, the optional host W95FA font, vendor document dumps and old binary bundles.

SAM, generated Consolas fonts and the old music assets are absent from the current source tree.

Before publication, record the available RTL-SDR import provenance and include the C6 build inputs and notices with any C6 firmware distribution. The source manifest must match the released firmware.

## DSD core attribution follow-up - 2026-09-11

The six remaining files in `components/lakeshark/apps/p25/` that carried no
origin notice now carry the DSD notice, copied from the same revision
`59423fa46be8b41ef0bd2f3d2b45590600be29f0` COPYRIGHT used for the helper notices. All
six arrived in the initial commit `893f74bc`, so no import commit records where
they came from; the lineage was established by measurement instead.

The supplied provenance analysis used this method: comments and whitespace were removed, identifiers and numeric literals
were collected, and for each pair of candidate forks the identifiers exclusive
to one fork were counted in the local file. Fork-exclusive vocabulary is what an
import carries. Candidates were `szechyjs/dsd` at the revision above,
`lwvmobile/dsd-fme` at `audio_work`, and `arancormonk/dsd-neo` at `main`.

| Local file | Upstream path at that revision | DSD-only identifiers kept | dsd-fme-only | dsd-neo-only |
|---|---|---|---|---|
| `dsd_filters.c` | `src/dsd_filters.c` | 125 of 125 | 0 of 19 | 0 of 90 |
| `dsd_frame.c` | `src/dsd_frame.c` | 55 of 75 | 0 of 48 | 0 of 22 |
| `dsd.h` | `include/dsd.h` | 73 of 115 | 11 of 737 | no file at a comparable path |
| `dsd_frame_sync.c` | `src/dsd_frame_sync.c` | 15 of 53 | 10 of 56 | 9 of 784 |
| `dsd_symbol.c` | `src/dsd_symbol.c` | 3 of 35 | 3 of 65 | 15 of 485 |
| `dsd_main.c` | `src/dsd_main.c` | 4 of 92 | 4 of 970 | no file at a comparable path |

`dsd_symbol.c` is the one row that favours dsd-neo. Its fifteen matches are C
keywords plus `lmid`, `umid`, `max_c` and `min_c`, and `lmid`, `umid` and
`numflips` are `dsd_state` members in the szechyjs `include/dsd.h` at lines 153,
154 and 168, present locally at 143, 144 and 158. The apparent signal is a file
boundary, not a different fork.

Corroborating: `dsd.h` includes `p25p1_heuristics.h` and `dsd_frame.c` includes
`p25p1_check_nid.h`, both filenames in that revision's `include/`, and
`dsd_dibit.c` in the same folder retained its DSD notice all along.

As with the helpers, this establishes a verified comparison source and the
lineage. It does not establish the original import revision, and it does not
rule out an intervening fork that had not diverged in these files. The inserted
headers say exactly that.

The DSD COPYRIGHT places this material under its ISC-style permission notice.
The GPLv2 terms in that file apply to `dstar_header.c/h`, `descramble.h` and
`fcs.h`, none of which are in this tree.
