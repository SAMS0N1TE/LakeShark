# LS-739 P25 trunking receive contract

Evidence: **Build-verified**, not Hardware-verified. No Phase II voice decoder
or Phase II audio is implemented. Workstream 736 owns the GUI; no GUI file was
changed here.

## Protocol references and compatibility

The existing 0x33 implementation was already present (LS-672), despite task
661 saying it was absent. Its bit extraction and signed offset were checked
on 2026-09-07 against these primary implementation sources:

- [OP25 trunking.py](https://github.com/boatbod/op25/blob/master/op25/gr-op25_repeater/apps/trunking.py),
  opcode 0x33 and `channel_id_to_frequency`. Copyright Max H. Parke KA1RBI
  (2011–2017) and Graham Norbury (2017–2021), GPL-3.0-or-later.
- [sdrtrunk FrequencyBandUpdateTDMA](https://github.com/DSheirer/sdrtrunk/blob/master/src/main/java/io/github/dsheirer/module/decode/p25/phase1/message/tsbk/standard/osp/FrequencyBandUpdateTDMA.java)
  and [ChannelType](https://github.com/DSheirer/sdrtrunk/blob/master/src/main/java/io/github/dsheirer/module/decode/p25/reference/ChannelType.java),
  copyright Dennis Sheirer, GPL-3.0-or-later. These independently confirm the
  field positions, signed spacing multiplier, slot counts and vocoder types.
  The earlier code cites TIA-102.AABC-D table 2.3.40-1; this run checked the
  implementation sources, not a purchased copy of that standard.

Fields are MSB-first across the 96-bit block: identifier 16–19, channel type
20–23, offset sign 24 (zero negative), offset magnitude 25–37, spacing 38–47
in 125 Hz, base 48–79 in 5 Hz. Offset is signed magnitude times spacing.
For logical position `channel & 0xfff`, carrier is
`base + floor(position / slots) * spacing`; slot is `position % slots`.
The replay's hand-worked example uses base 851.0125 MHz and 12.5 kHz spacing:
positions 14 and 15 both resolve to 851.1000 MHz on a two-slot carrier, slots
0 and 1. These are synthetic fixtures, not an RF recording from a named site.

| Channel type in 0x33 | Slots | Voice handling |
| --- | ---: | --- |
| 0 | 1 | Unsupported FDMA half-rate |
| 1 | 1 | Phase I full-rate path |
| 2 | 1 | Unsupported FDMA half-rate |
| 3, 5 | 2 | Unsupported TDMA |
| 4 | 4 | Unsupported TDMA |
| 6–15 | Unresolved | Reserved; invalidate that identifier |

Zero base/spacing and carriers beyond the 32-bit tune API do not yield a
followable carrier. The legacy `p25_tsbk_frequency_hz` stays zero for
unsupported grants. The typed grant preserves the real carrier even when
voice is unsupported. No TDMA or half-rate grant can reach the Phase I
follower through the production scanner, including HOLD/priority paths.

Existing API names/signatures remain. Existing `p25_iden_entry_t` FDMA slot
values remain **zero**, because that sentinel is already public and explicitly
asserted by `fdma_iden_still_resolves_the_old_way`. The new canonical
`p25_call_info_t.slots_per_carrier` defaults to **one** for FDMA. This deliberate
compatibility adaptation supersedes 661's assumption that no slot field existed.

**Supervisor finding: 0x34 offset disagreement.** The existing VU tests require
`magnitude * 250000`, and the implementation does that. OP25's 0x34 path uses
`magnitude * spacing * 125`, as does its 0x33 path. The explicit instruction
to preserve the 0x34 tests prevents changing that behavior here. The tests and
their assertions remain unchanged; this discrepancy needs supervisor
reconciliation. It affects stored uplink offset, not downlink grant tuning.

## Backend delivery and lifecycle

The parser delivers up to six typed grants per TSDU (three blocks, two pairs
per 0x02). The last legacy opcode/fields remain historical telemetry. The
scanner consumes each batch generation once, so a later status/vendor block
cannot hide a prior grant or replay a stale one. A standalone parse attempt,
including CRC/vendor/protection rejection, replaces the previous delivery.

`p25_receive_frame()` is used by both app_p25's real decode loop and the host
replay. It routes grants through scanner policy, treats only validated TDU or
TDULC as terminators, refreshes traffic activity for muted valid LDUs, and
processes each LDU2's ESS (including KID changes). Return, new call, changed
talker, and tune boundaries retire PCM/ESS/LCW state. A changed carrier on the
same TG does not extend the old call. New control NAC or WACN/SYSID retires
band plans and system-local encryption skips. A contradictory LCW returns to
control without assigning that call's encryption to the requested TG.

The RX task signals tune completions; requests and completions advance an
atomic generation. Only the decode task resets decoder state. A frame crossing
that generation cannot deliver grants, ESS or PCM to the application. Pending
or failed tunes are not reported as received traffic; a failed traffic request
requests return to control. Physical retune timing remains unmeasured.

No dynamic allocations were added. On the host ABI each typed call is 40 B;
the six-entry batch is 240 B inside the existing PSRAM decoder state. The
follower and published P25 telemetry are bounded static internal state, and
the cross-task generation is a static internal atomic. No new DMA buffer or
task stack is placed in PSRAM. This is backend protocol state, not a reusable
screen widget; any presentation belongs in the shared UI kit under 736.

## GUI handoff to 736

`app_p25.c:p25_grant_publish_ui()` publishes these additional fields in `P25`:

| Field | Meaning and suggested use |
| --- | --- |
| `grant_observed` | Latest routed grant evidence, whether followed, filtered, unresolved or unsupported. Show TG/source, NAC, logical channel, carrier, slot/count, and WACN/SYSID only when `system_valid`. This is not current audio. |
| `grant_active` | Currently followed call identity; cleared on every return. A source-less update retains the known source. |
| `receive_state` | See state table below. Do not derive AUDIO from a grant, RF sync count or historical Phase II count. |
| `grant_unsupported_count` | Unsupported voice observations, including half-rate FDMA and TDMA. |
| `grant_unresolved_count` | Missing identifiers or invalid grant/carrier observations. |
| `control_relock_count` | Entries into control lock from validated TSDU/CRC evidence, including initial acquisition. |

| Receive state | Suggested label |
| --- | --- |
| `P25_RX_CONTROL_SEARCH` | Control: acquiring |
| `P25_RX_CONTROL_LOCKED` | Control: decoded TSDU |
| `P25_RX_TRAFFIC_WAIT` | Traffic: awaiting validated frame |
| `P25_RX_TRAFFIC_SYNC` | Traffic: frame received, no PCM |
| `P25_RX_AUDIO` | Phase I PCM produced |
| `P25_RX_ENCRYPTED` | Traffic: encrypted, no PCM |
| `P25_RX_TUNE_FAILED` | Tune failed |

For `grant_observed.support == P25_CALL_UNSUPPORTED`, show “Unsupported TDMA”
when slots exceed one, otherwise “Unsupported half-rate FDMA”. Keep that
observation separate from the active Phase I call. Slot indices are zero-based;
a display may present slot + 1, but must label its convention. Missing/invalid
support is an unresolved grant, not idle/no traffic. Legacy Phase II counters
remain historical compatibility telemetry and must not be labeled decoded
bursts or audio. `grant_on_traffic` is follower intent; use the existing
`p25_get_receiver_status()` requested/effective/error fields for tuner state.

GUI consumption and real display appearance remain **unverified** here. No
screen was observed, flashed or manipulated. This handoff is part of 736's
integration acceptance rather than a new UI microtask.

## Validation boundary

Final gate: `pwsh -NoProfile -File bench/verify.ps1 -Level smoke`, exit 0,
`VERIFY OK (level=smoke, 69s)` on 2026-09-07. Host suite, 36 C++ headers and
LCD4.3 firmware passed. `p25_receive.h` also passed a standalone g++ C++
syntax check. Receive replay: 13/13; grant suite: 21/21; TSBK suite: 36/36.

Three added cases in `test_p25_grant` first failed on the original backend
(6 assertions): wrong-carrier duplicate refresh, vendor replay, and a frequency
unrepresentable by the tune API. They pass after the changes.

`test_p25_receive` exercises real BCH/frame dispatch, TSBK CRC/trellis and
interleaving, the production scanner/follower, and the shared receive bridge.
Its LDU/vocoder endpoint is a spy: PCM-presence tests prove dispatch and mute
policy, not intelligible speech or an IQ-to-audio decoder acceptance result.
It covers all 16 type codes, mixed TDMA/FDMA pairs, status-after-grant delivery,
carrier/slot/identity, return/relock, stale calls, NAC/system changes, missing
identifiers, invalid plans/grants, protected/vendor/CRC rejection, corrupted
NIDs and unsupported DUIDs. Existing 0x34/0x3d and encryption tests stay intact.

Hardware checks still required: named-board control-to-traffic tuning and
return latency, same-TG reacquisition under live RF, pending/failed tune
recovery, cross-core ring timing, encryption transitions, intelligible Phase I
audio, and 736's actual display/touch acceptance. There is no Phase II burst
decoder or audio acceptance claim. The wider IQ replay gap described in
`bench/P25_ACCEPTANCE.md` remains.
