# Third-party release review

Reviewed 2026-09-07 against the current main checkout, not just its Git HEAD.
Evidence: component CMake files, `main/idf_component.yml`,
`dependencies.lock`, local source headers/notices, and the LCD build's
`build_lcd43/project_description.json` component list. This is an inventory
and release checklist, **not legal certification or permission to publish**.
No notices, sources, fonts, or media were removed by this review.

## Inventory and unresolved actions

Paths below are repository-relative. A notice in a wrapper does not establish
the terms of every nested library or asset.

| Bundled material | Existing source / notice | Required release action |
|---|---|---|
| LakeShark application | Root `LICENSE` contains GPLv3; `main/`, `components/apps/`, `components/lakeshark/` | Preserve it and all per-file notices. Match distributed firmware to its actual modified sources and build instructions. A root license does not resolve the rows below. |
| DSD-derived P25, error-correction helpers | `components/lakeshark/apps/p25/`; provenance noted in `CLAUDE.md` and component CMake; several examined DSD/Hamming files have no leading origin notice | Identify the exact upstream revisions and applicable per-file notices, including imported DSD-Neo/OP25-derived portions. Preserve existing headers; restore missing attribution only from verified upstream evidence. Do not infer all files share the root license. |
| Fixed-point IMBE vocoder | `components/imbe_vocoder/`; e.g. `decode.cc` retains 2009 copyright and GPLv3-or-later header; CMake identifies OP25 / Pavel Yazev | Preserve headers and record exact upstream provenance. Include the compiled local implementation and shim, not merely an upstream URL. |
| mbelib | `components/mbelib/`; `mbelib.c`, headers and tables retain the 2010 ISC-style permission notice | Preserve the complete per-file copyright/permission text and record upstream revision. Source includes additional AMBE tables even though current CMake compiles `mbelib.c`, `imbe7200x4400.c`, `ecc.c`; distinguish source inclusion from linked functionality. |
| RTL-SDR and tuner drivers — **notices done, revisions still open** | `components/lakeshark/radio/COPYRIGHT.librtlsdr` and `UPSTREAM.md` now record the licence and per-file provenance. `tuner_fc2580.c` had lost its upstream comment block and it was restored verbatim from `osmocom/rtl-sdr` master (fetched 2026-09-07). `tuner_fc2580.h`, `reg_field.h` and `rtlsdr_i2c.h` were checked against upstream and carry no header there either, so they are not missing one. The six files modified for the ESP32 port now carry a GPL-2.0-or-later section 2(a) notice of change, and the 28 LakeShark-original files in that directory carry an SPDX-License-Identifier so third-party and first-party code are distinguishable. Licence chain checked: upstream `COPYING` is GPLv2 but every per-file header grants "or (at your option) any later version", so the material is GPL-2.0-or-later and the GPLv3 root licence is compatible. | Remaining: pin the exact upstream librtlsdr and xtrsdr revisions in `UPSTREAM.md`. A GPL-2.0-only replacement for any of these files would invalidate the GPLv3 root licence — check the "or later" wording on any future update. |
| SAM speech synthesizer — **unresolved** | `components/lakeshark/audio/sam/SAM_LICENSE.md` and adjacent sources | Existing notice expressly says permission is unresolved. Age/public forks are not permission. Obtain documented authority or decide on a replacement/release exclusion and rebuild; do not silently omit a notice while retaining the code. Notice still says `main/sam/`, so its location description also needs correction. |
| Consolas bitmap fonts — **unresolved** | `components/apps/sdr_ui/lv_font_consolas_14.c`, `lv_font_consolas_16.c`; no provenance/license notice located in those generated files | Establish source font, conversion provenance, and redistribution authority, or replace with appropriately documented assets and rebuild. LVGL's license does not license arbitrary input fonts. |
| W95FA host font | `host/W95FA.woff`, `host/W95FA-FONT-LICENSE.txt` | Local note identifies author and OFL1.1 but links to rather than bundles the full text. Verify against the original font distribution and include its complete notice/license if distributing the optional host UI. Not part of the minimal firmware source set below. |
| LVGL 8.4.0 and local LVGL port | `managed_components/lvgl__lvgl/LICENCE.txt`; `components/esp_lvgl_port/license.txt`; local port manifest pins 2.5.0 | Preserve root and nested library/font notices. Export local port source and allocator integration in `main/ls_lvgl_allocator.cmake`; a fresh registry port is not the local override. |
| Waveshare BSP / panel drivers | `components/bsp_extra/LICENSE`; `managed_components/waveshare__esp32_p4_wifi6_touch_lcd_4b/LICENSE`; `waveshare__esp_lcd_st7703/license.txt`; `espressif__esp_lcd_st7701/license.txt` | Preserve notices and the three modified BSP files listed below. Versions/hashes come from the lock file, not a fresh version-range resolution. |
| Espressif networking, transport, codec and touch components | `managed_components/espressif__{esp_hosted,esp_wifi_remote,esp_serial_slave_link,eppp_link,esp_codec_dev}/LICENSE`; `espressif__{cmake_utilities,esp_lcd_touch,esp_lcd_touch_gt911}/license.txt` | Bundle the applicable notices and nested notices, notably `espressif__esp_hosted/common/protobuf-c/LICENSE`. If distributing C6 firmware, separately inventory its exact build/source and nested slave notices; P4 source alone does not cover that artifact. |
| Audio player / file iterator | `managed_components/chmorgan__esp-audio-player/LICENSE`; iterator's `file_iterator.c` and `include/file_iterator.h` contain Apache2.0 notice | Preserve attribution; bundle the full applicable license for iterator rather than relying only on its header URL. |
| Helix MP3 — **nested terms require review** | `managed_components/chmorgan__esp-libhelix-mp3/LICENSE` is Apache2.0; inner `libhelix-mp3/LICENSE.txt`, `RPSL.txt`, `RCSL.txt` state different RealNetworks terms | Do not describe the complete decoder as Apache-only. Review the actual inner license choice and distribution compatibility before publishing the linked firmware/source; preserve all nested texts. |
| SPIFFS media — **unresolved** | `spiffs/music/` contains `BGM 1.mp3`, `BGM 2.mp3`, `For Elise.mp3`, `Something.mp3`, `Waka Waka.mp3`; no adjacent rights notices located | Verify each recording, not just a composition's age/title. Current `main/CMakeLists.txt` embeds the entire `spiffs/` directory into a storage image. Hold redistribution of that image until media and panel profiles are reviewed or intentionally replaced and rebuilt. |
| ESP-IDF and external toolchain | Build uses ESP-IDF5.5.4; IDF lives outside this repository | Record exact IDF revision/submodules and tool versions; preserve their applicable notices for distributed artifacts. This bounded local inventory is not a complete transitive IDF/toolchain license audit. |

Other decoder/app files, logos and generated assets still need a provenance
pass; absence of a matching notice search result is not an ownership finding.

Scan of 2026-09-07, for the two rows above. In `components/imbe_vocoder/`,
`imbe_vocoder.cc` has lost the four-line Pavel Yazev attribution block that
upstream OP25 carries, and `imbe_vocoder.h` still has it but corrupted with
stray asterisks; `imbe_shim.cpp`/`.h` are LakeShark glue around GPLv3 code and
carry no SPDX header. In `components/lakeshark/apps/p25/`, only `dsd_dibit.c`
retains the "Copyright (C) 2010 DSD Author" ISC-style notice - `dsd.h`,
`dsd_filters.c`, `dsd_frame.c`, `dsd_frame_sync.c`, `dsd_main.c` and
`dsd_symbol.c` have none, and `Golay24.hpp`, `Hamming.cpp`, `Hamming.hpp` and
`ReedSolomon.hpp` have neither notice nor any traceable provenance marker.
Those four are **not** identifiable from the tree: their GUID-style include
guards and the `DSDGolay24` subclass point at some DSD-family C++ source, and
`ReedSolomon.hpp` looks like a template wrapper around the classic Simon
Rockliff / Phil Karn Reed-Solomon code whose own terms require the author's
name be kept in the heading - but that is inference, not evidence. The author
has to say where they came from before anything is written into them. The
README's blanket claim that all vendored headers are preserved verbatim was
false and has been corrected.
`tools/lcd43-local-license-paths.json` is a useful existing notice list, but it
does not cover all the rows above and is not a release-clearance record.

## Managed source changes that must survive export

Comparing present production C/C++/header/CMake/Kconfig/manifest files against
their component `CHECKSUMS.json`, with CRLF normalized to LF for comparison,
found these content changes in the current main checkout:

```text
managed_components/waveshare__esp32_p4_wifi6_touch_lcd_4b/esp32_p4_wifi6_touch_lcd_4b.c
managed_components/waveshare__esp32_p4_wifi6_touch_lcd_4b/CMakeLists.txt
managed_components/waveshare__esp32_p4_wifi6_touch_lcd_4b/include/bsp/display.h
```

No additional production-code paths outside those checksum manifests were
found in that check. Examples/tests/docs were outside the content comparison;
this is not a claim that every cached byte equals upstream. Raw hashes differ
widely from registry records because of line endings. Preserve existing
registry metadata as origin evidence and produce a separate export SHA256
manifest for actual shipped bytes; do not relabel registry hashes as hashes
of patched source. Registry redownload alone loses the BSP changes.

## Minimum explicit firmware-source export allowlist

This is the LCD4.3 build closure observed above, not permission to distribute
unresolved material and not an implemented exporter. Use only these named
roots/files, retaining their source headers, build scripts, manifests, notices,
and required assets. Resolve links before copying and reject anything escaping
the selected roots. A final secret/content scan is still mandatory.

```text
CMakeLists.txt
LICENSE
README.md
dependencies.lock
sdkconfig.defaults
partitions.csv
cmake/
main/
components/apps/
components/bsp_extra/
components/esp_lvgl_port/
components/imbe_vocoder/
components/lakeshark/
components/mbelib/
boards/lcd43_gui.defaults
boards/partitions_32m.csv
docs/THIRD_PARTY_REVIEW.md

managed_components/chmorgan__esp-audio-player/
managed_components/chmorgan__esp-file-iterator/
managed_components/chmorgan__esp-libhelix-mp3/
managed_components/espressif__cmake_utilities/
managed_components/espressif__eppp_link/
managed_components/espressif__esp_codec_dev/
managed_components/espressif__esp_hosted/
managed_components/espressif__esp_lcd_st7701/
managed_components/espressif__esp_lcd_touch/
managed_components/espressif__esp_lcd_touch_gt911/
managed_components/espressif__esp_serial_slave_link/
managed_components/espressif__esp_wifi_remote/
managed_components/lvgl__lvgl/
managed_components/waveshare__esp32_p4_wifi6_touch_lcd_4b/
managed_components/waveshare__esp_lcd_st7703/
```

Do not bulk-export the remaining cached managed components: the current LCD
build does not list `esp_modem`, `esp-dsp`, `esp-modbus`, `libsodium`, `mdns`,
`network_provisioning`, or `littlefs`. Re-evaluate the closure for other targets.

Required preparation before this becomes a reproducible release package:

1. Resolve the SAM/fonts/Helix/media decisions above. The allowlist preserves
   current source; it is not a workaround for unresolved redistribution rights.
2. Supply an explicitly reviewed `spiffs/` input set. Do not copy current music
   or `spiffs/panels/{custom,lab_bench,windows}.json` automatically. An approved
   empty storage input is a deliberate changed image, not reproduction of the
   current storage image. The build requires the directory to exist.
3. Generate a reviewed, credential-free release configuration from the actual
   tested target and compare it with `sdkconfig.defaults` plus
   `boards/lcd43_gui.defaults`. Do not export the working root `sdkconfig`,
   build-directory configs, or runtime NVS/SD data as a shortcut.
4. Add public build instructions independent of private `bench/configs.json`:
   ESP-IDF5.5.4, `esp32p4`, explicit separate SDKCONFIG path, the two defaults
   above, and exact toolchain versions. Include an actual source-file hash
   manifest and the originating firmware/build identity. An archive without
   `.git` intentionally reports `1.0.1-archive`; do not claim byte-identical
   firmware without testing and accounting for build identity/timestamps.
5. Rebuild from a clean staged export with the local patches present; compare
   dependency closure, configuration and artifacts. This review did not perform
   that rebuild and does not certify the source package complete.

Explicitly excluded: `.git/`, `.claude/`, all `bench/` (including queues,
claims, notes, logs, local agent configuration and physical board identities),
all `build*/`, working `sdkconfig*` except the approved defaults above,
credentials/keys/certificates/private environment files, captures, crash dumps,
recordings, `LCD_4_3_Docs/`, `c6_firmware/`, host UI and Flipper packages, and
unreviewed `tools/` or other `docs/`. Optional public tests, companion firmware,
host tools and documentation require their own reviewed allowlist; do not copy
private bench infrastructure just to make a build command work.
