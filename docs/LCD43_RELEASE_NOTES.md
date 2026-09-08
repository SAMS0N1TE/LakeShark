# LakeShark LCD4.3 experimental preview

Prepared September 7, 2026. Local release candidate; not published.

For the Waveshare ESP32-P4-WIFI6-Touch-LCD-4.3, 480×800 display,
32 MB flash. This is an application-only upgrade for an already compatible
LakeShark installation, not a universal installer or an all-board release.

## Included changes

- Foreground applications unload before another application takes ownership
  of the radio; shared UI allocations stay in PSRAM when resized.
- Shared receiver headers, dark text/password/numeric keyboards, ACARS
  receiver controls, and selectable HOME receiver/status/clock widgets.
- Wi-Fi network discovery and connection controls in Settings.
- Bounded crash inspection that does not parse a stored ELF dump on-device.
- P25 first-frame threshold seeding and corrected CQPSK discriminator polarity.
- AUTO acquisition windows count delivered IQ, preserve the winning DSP
  state, and discard results that cross a demodulator configuration change.
- Invalid P25 encryption metadata is rejected; IMBE decoder allocation uses
  PSRAM and fails safely instead of allowing an unchecked allocation failure.

These changes do not add P25 Phase 2 voice, DMR voice, mesh chat, or LoRa TX.

## Exact candidate

| Item | Identity |
|---|---|
| Firmware version | `1.0.1-gfc9fb8104ab9-dirty` |
| ESP-IDF | `v5.5.4` |
| Tested C6 / Hosted firmware | `2.12.9` / `2.12.9`; not bundled |
| Application SHA256 | `ef70d1c3554d372430d3030853b7b15d696fe2435c21cf94291e24e96eddeb20` |
| Upgrade offset | `0x10000`, only with the verified compatible partition layout |

The build contains uncommitted changes. Its Git revision alone is not a full
source identity. Use the package manifest and hashes to identify this binary;
the manifest describes the scope and limitations of its source fingerprint.

## Validation

The combined host tests, 37 C++ header checks and LCD4.3 firmware build passed.
On the exact candidate, application-only flashing verified the image hash;
boot and P25 reception ran through 49 seconds without an observed panic/reset,
with approximately 0.49 MB/s IQ delivery and no reported USB/ring drops.

The test channel was quiet. **Audible P25 voice and live trunk following have
not been verified on this candidate.** An earlier build received 12 valid NIDs
from a 20-frame control-only RF fixture; that is not a voice test or a test of
this exact binary. Host IMBE tests produce nonzero PCM from synthetic harmonics,
not verified intelligible speech from the LCD speaker.

## Known limitations

- P25 warm C4FM-to-CQPSK switching still has a documented fixture packet-loss
  case: 19 valid NIDs but 14 valid TSBKs. The test remains marked known-failing.
- Complete touch/layout acceptance, MAP crash replay, long transition soak,
  Wi-Fi reconnect, on-air ACARS and full capture workflows remain unverified.
- A SIGNAL callback exceeded its 20 ms budget during the bounded hardware
  check. Do not interpret this preview as a complete P25 performance fix.
- LCD4.3 battery/power routing remains hardware-specific. Other boards must
  not use this image.

## Installation and reporting

Read [Quickstart](LCD43_QUICKSTART.md) and the packaged
`FLASH_INSTRUCTIONS.txt` before upgrading. Keep the matching ELF and previous
known-working application for recovery. Do not erase settings or crash evidence.
For failures, use [Troubleshooting](LCD43_TROUBLESHOOTING.md).

## Publication status

Public distribution is pending review of SAM permissions, font provenance,
complete third-party notices and corresponding source, followed by maintainer
approval. Including a notice is not itself permission to redistribute. See
[Third-party review](THIRD_PARTY_REVIEW.md). No download is announced by this draft.
