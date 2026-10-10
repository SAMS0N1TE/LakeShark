# Third-party components

LakeShark is licensed under GPL-3.0. Bundled components retain their copyright and license notices.

| Component | Notice location | Terms |
| --- | --- | --- |
| RTL-SDR and tuner drivers | `components/lakeshark/radio/COPYRIGHT.librtlsdr` and per-file headers | GPL-2.0-or-later |
| IMBE vocoder | `components/imbe_vocoder/COPYRIGHT` and per-file headers | GPL-3.0-or-later |
| Experimental Phase II decoder | `components/p25_phase2/README.md`, `COPYING` and per-file headers; OP25 by Max H. Parke, Graham J. Norbury and contributors | GPL-3.0-or-later, with retained mbelib ISC notices |
| mbelib | `components/mbelib/COPYRIGHT` | ISC-style permission notice |
| DSD core and helpers | P25 source headers and `components/lakeshark/apps/p25/UPSTREAM.md` | ISC-style DSD permission notice and additional per-file helper notices |
| Mode S decoder | `components/lakeshark/apps/adsb/LICENSE.libmodes` and source headers | BSD-2-Clause, libmodes by Thomas Watson, derived from dump1090 by Salvatore Sanfilippo |
| Classic NFC cipher helpers | `components/lakeshark/nfc/nfc_classic.c` header and `LICENSE` | GPL-3.0-or-later; adapted from Flipper Zero firmware Crypto1 helpers |
| MeshCore and ed25519 | `components/meshcore/` notices and upstream notices | Preserve each component's license |
| libcarto, PMTiles and TUI library | Notices within their component directories | Preserve each component's license |
| CartoCore and Zstandard 1.5.7 (including FSE/HUF and xxHash) | `components/cartocore/upstream/NOTICE`, `LICENSE`, `PROVENANCE.txt` and `third_party/zstd/LICENSE` | CartoCore GPL-3.0-or-later; Zstandard BSD-3-Clause option |
| Speech synthesis (Moonshine klatt-tts and g2p) | `components/klatt_tts/NOTICE` and `LICENSE` | MIT |
| Pronouncing dictionary | `components/klatt_tts/LICENSE.CMUdict` | CMU Pronouncing Dictionary BSD-style terms; the notice ships with binaries |
| DejaVu fonts | `components/apps/tui/DEJAVU-LICENSE.txt` | Bitstream Vera and DejaVu terms |
| LVGL | `managed_components/lvgl__lvgl/LICENCE.txt` and nested notices | MIT and applicable nested notices |
| T-Display-P4 panel and touch sequences (RM69A10, HI8561, GT9895, HI8561 touch) | `components/lakeshark/board/ls_panel.c` and `ls_touch.c` headers | GPL-3.0; adapted from LILYGO_L cpp_bus_driver |
| Espressif and Waveshare components | Their component license files and source headers | Primarily Apache-2.0; retain nested notices |
| Audio player and file iterator | Their component notices and source headers | Apache-2.0 |

See the component license files and source headers for complete terms. Firmware packages include third-party notices and corresponding source information.
