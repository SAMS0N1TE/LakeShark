LCD-4.3 application update for the Waveshare 32 MB, 480x800 board.

- Fixes recording command stack exhaustion and a recording-control lock that could starve the receiver CPU.
- Keeps FATFS work buffers in internal RAM and adds completed-load metadata.
- Restores missing symbol glyphs in mono-font labels and keyboard buttons.

The owner confirmed the end-to-end hardware test: a recording downloaded to the Flipper and replayed successfully on the current LCD recovery firmware. The recording recovery also completed over 15 minutes without watchdog or late-display errors, and 200/250-edge UART downloads matched expected counts and durations. This exact font-update application passed the host gate, LCD build, flash hash verification and a short display health check with zero late frames.

Download `lakeshark-lcd43-v1.0.4-app.zip` for an existing LCD-4.3 installation. Follow its README: app-only at `0x10000`; settings/storage are preserved. This is not a first-install bundle and must not be flashed to another board. The optional debug archive contains the matching ELF.

The Flipper FAP was not updated; automated alternating BLE-transfer integrity and the new paired-app ACK handling remain unverified. Visual confirmation of every affected glyph screen and audible P25 during the final short check remain pending. Source provenance and exact hashes are included in the application archive.

## Exact released image

- Release: v1.0.4, LCD-4.3 only, ESP-IDF 5.4.3.
- Application SHA256: `1272f0b4da537e7ac8a1ec83c883a1030d9647b69d7d3ccf991929887b83e4a5`.
- Matching ELF SHA256: `be16864859e033653ae646e2e973cc554d1780269f9b220d7fc83ffd2bdcde45`.
- Embedded version: `1.0.4-g84fb9af6ec15-dirty`; this is the exact tested incremental image, not a rebuild after publication.
- Source base: `84fb9af6ec15c7e76a8df287bd20ce37db0b99dd` plus the six-file patch included in the application archive. The release tag also restores verified upstream comment notices; those comments do not alter firmware behavior.

The earlier experimental-preview notes described another image and are superseded by this release record. For first installation use the LCD-4.3 v1.0.3 full package before this application-only update.
