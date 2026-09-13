# Third-party components

LakeShark is licensed under GPL-3.0. Bundled components retain their copyright and license notices.

| Component | Notice location | Terms |
| --- | --- | --- |
| RTL-SDR and tuner drivers | `components/lakeshark/radio/COPYRIGHT.librtlsdr` and per-file headers | GPL-2.0-or-later |
| IMBE vocoder | `components/imbe_vocoder/COPYRIGHT` and per-file headers | GPL-3.0-or-later |
| mbelib | `components/mbelib/COPYRIGHT` | ISC-style permission notice |
| DSD core and helpers | P25 source headers and `components/lakeshark/apps/p25/UPSTREAM.md` | ISC-style DSD permission notice and additional per-file helper notices |
| Mode S decoder | `components/lakeshark/apps/adsb/LICENSE.libmodes` and source headers | BSD-2-Clause, libmodes by Thomas Watson, derived from dump1090 by Salvatore Sanfilippo |
| Classic NFC cipher helpers | `components/lakeshark/nfc/nfc_classic.c` header and `LICENSE` | GPL-3.0-or-later; adapted from Flipper Zero firmware Crypto1 helpers |
| MeshCore and ed25519 | `components/meshcore/` notices and upstream notices | Preserve each component's license |
| libcarto, PMTiles and TUI library | Notices within their component directories | Preserve each component's license |
| DejaVu fonts | `components/apps/tui/DEJAVU-LICENSE.txt` | Bitstream Vera and DejaVu terms |
| LVGL | `managed_components/lvgl__lvgl/LICENCE.txt` and nested notices | MIT and applicable nested notices |
| Espressif and Waveshare components | Their component license files and source headers | Primarily Apache-2.0; retain nested notices |
| Audio player and file iterator | Their component notices and source headers | Apache-2.0 |

See the component license files and source headers for complete terms. Firmware packages include third-party notices and corresponding source information.
