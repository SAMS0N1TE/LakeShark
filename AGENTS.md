# LakeShark working instructions

## One entry point

Run `python bench/quality.py context --topic <area>` before changes.
Use `python bench/quality.py --help` for commands; do not search for test scripts.

| Need | Command after `python bench/quality.py` |
| --- | --- |
| Relevant behavior and ALL open blockers | `context --topic p25` |
| Index and path validation | `check` |
| All Python tests (offline) | `test` |
| Full existing host suite | `host` |
| Bounded serial capture | `watch --port COM9 --seconds 330 --log path/to/new.log` |
| Analyze existing capture | `analyze --log path/to/log` |
| Memory placement changes vs released baseline | `memory` |
| Every-feature radio audit | `audit --topic <feature>` / `audit --evidence <file>` |
| Universal source coverage and gaps | `sources` |
| Fingerprint source | `source-digest` |
| Hardware release clearance | `release-check --artifact build_tdp4/lakeshark.bin --evidence path/to/run.json` |
| Create gated package | `package --evidence path/to/run.json` |

## All-radio feature audit (every implementation)

Before implementing ANY feature, fix or refactor, audit ALL radios/data sources
for feature applicability. User examples are examples, never scope boundaries.
Run `python bench/quality.py audit --topic <feature>` in the firmware checkout.
Review RTL-SDR, HackRF, CC1101, SX1262/LoRa (including Mesh), GPS/GNSS, nRF24,
NFC, Wi-Fi and Bluetooth, plus newly discovered board/endpoint capabilities.
For EACH source record covered, gap, or not_applicable, with a concrete reason
and evidence. A missing adapter is a gap, not unsupported hardware. Include
recording/export, decode, spectrum/data views, tuning, metadata, ownership,
alerts, disconnect/lifecycle, and memory/display/audio impact where applicable.
Validate the completed audit with `audit --evidence <file>`. Repeat after edits;
release evidence must include source-bound feature audits. Keep unresolved gaps.
Use PortaPack Mayhem as a reference for shared controls, format-aware capture,
metadata, loss reporting and lifecycle; adapt it to LakeShark's multiple radios
and measured memory budgets. See design_references in bench/regressions.json.

## Mandatory behavior

- Universal tools (REC, waterfall and future shared apps) must expose ALL radio/data sources by real capability, including GPS recording. No fixed two-radio allowlist. Read the universal-radio-tools contract and universal_sources matrix.
- Treat every stack/buffer/DMA placement change as a measured change: run quality.py memory, explain the placement, compare headroom and test loaded display/UI/audio. PSRAM is not automatically wrong; moving everything internal can starve DMA.


- `bench/regressions.json` is current state; JSONL is append-only history. Load
  relevant contracts, not whole histories. New reproductions supersede old handoffs.
- Update state and append evidence before ending a failed-candidate turn. Sync
  public/private working copies and check both. Never discard contrary evidence.
- P25 blue flashing and Flipper ignored mute/crashes remain OPEN. Host success,
  idle SDR, missing Flipper or user silence cannot clear them.
- Identify device, branch and artifact hashes. Back up before installation;
  verify installed/readback hashes. Change one device/hypothesis per measured run.
  On new crashes stop the affected app and restore the backup; preserve logs.
- Test active SDR/P25 for five minutes, ten Flipper open/close cycles, ten FM/P25
  transitions, Wi-Fi retry, touch/audio, all alerts off, independent outputs and
  relaunch. Record observed outcomes. No pass may be inferred from host tests.
- Release evidence JSON must identify source/binary, observer, board, active SDR,
  duration, C6/Flipper versions, hashed log/Flipper app/sdkconfig and observed
  cases listed in `quality.py:REQUIRED_CASES`. Packaging enforces the same gate.
- Preserve ESP-Hosted cache-line base AND stride alignment. Use board SDK from
  `bench/configs.json`. Measure internal/DMA headroom before moving audio memory.
- During display acceptance captures, avoid heap/runtimes/task dumps: these diagnostic commands enumerate task stacks and can disturb the display timing being measured.
- Public publishing requires explicit user authorization. Do not bypass the gate.

Work from public `main` (2.3.0 and later). Private branches hold only private paths, evidence and notes.
Flipper companion source: `companion/modern-20260923` in LakeShark-Flipper (v2.6) is the current source and the build terminalbay.com hosts.

Memory evidence: release JSON requires memory_review.diff_sha256 from quality.py memory, placement_rationale, positive required_internal_largest/required_dma_largest allocation budgets, and two memory-command snapshots covering the loaded run. Use watch --memory with firmware containing the memory command. Full stack dumps stay outside timing captures.

Release source_cases must contain observed recording and waterfall/data-view results plus actual data_format for every universal_sources entry. A hardware limitation is shown to the user; it must not be used to hide an unimplemented adapter.

Shared chrome rule (user update 2026-09-15): words and touch targets keep rounded-corner padding. Landscape bar end caps extend to the physical arc with per-pixel clipping; do not fill invisible corners. Preserve the updated `rounded-panel-bars` contract.

## ASCII-first instrument design

All new or changed visuals across firmware, Flipper and Ground Station must preserve the ASCII/LED/VFO instrument look. ASCII is the default. Solid graphics may be offered when they convey data better (for example the existing waterfall), never as decoration alone.
Every data visual must identify its source, units, scale/axes and orientation, marker legend, freshness/unavailable state, and the practical question or comparison it supports. Provide exact values or selection when a graphic alone is insufficient. Do not draw invented measurements or imply orbital/geographic positions from local sky bearings. Audit applicability across every radio; preserve explicit adapter gaps. Test reference labels, missing/stale data and clipping as well as appearance.

Portrait UI rule: choose visible shortcuts by display orientation, never keyboard attachment. Portrait uses large labelled touch controls without letter/F-key badges or TOUCH placeholders. When apps overflow, page navigation must be a distinct large PREV/PAGE/NEXT control row in both orientations, with matched hit targets. HOME fills available space and hides unnecessary paging (user update 2026-09-15).

Hardware scripts must assert the active screen, source and running state before transmitting fixtures or interpreting counters. Commands sent during boot or interleaved console errors do not prove a test ran. Preserve invalid runs as such and restart from observed state.
