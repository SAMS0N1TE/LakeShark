# The app model, and where it is going

For the catalog/storefront track. **This is a trajectory, not a set of
limits.** An earlier version of this file answered the same questions by
describing today's implementation as the permanent answer - "no loader, none
planned", "Tier C has nothing to review" - and that was wrong. Those are the
maintainer's calls, the direction is toward apps with real hardware access,
and a catalog designed against today's snapshot would have to be rebuilt.

Correction recorded rather than quietly edited, because the first version was
already sent: the section on NFC in it was factually wrong. See §4.

## The three tiers

| Tier | What an app is | Status |
|---|---|---|
| **1. Described screens** | `.lsapp` text: panels bound to named read-only values | **built** |
| **2. Scripted apps** | described screens plus actions - tune, scan, capture, sequence | **not started** |
| **3. Native apps** | compiled against an exported API, with hardware capabilities | **not started** |

Tier 1 exists so there is something real to build a catalog against. It is a
floor, not a ceiling. **Design the store for tier 3 and let tiers 1 and 2 be
the easy cases**, because the reverse - a catalog built for data-only content
that later has to grow a signing model, a capability model and an ABI check -
is the expensive order to do it in.

### Tier 1, as built

`.lsapp` files in `/sdcard/apps`, parsed into a bounded model and drawn by the
firmware's own widgets. Format in `USER_APPS.md`. Bound to `ls_value.h` - 25
read-only dotted paths today, and the list is meant to grow.

Current caps are in `ls_userapp.h` and are **implementation caps chosen
quickly, not engineering budgets**: read them from the header rather than
copying the numbers, because they will move. They already have once - the
first version put the model array in internal `.bss`, measured at 7,362 bytes
off the target ELF against a board whose recorded post-P25 headroom is 2,823
bytes, so it moved to PSRAM and the caps roughly doubled.

Everything that hits a cap now reports it. A refused file names the line; a
full directory names the app it dropped.

### Tier 2, the next step

Described screens that can *do* something: retune, start a sweep, arm a
capture, step a scan list, sequence those. The natural shape given tier 1 is
an action vocabulary over the same named-binding idea - `ls_value` paths for
reading, a matching verb table for doing - which stays parseable, stays
bounded, and stays incapable of expressing an unreviewed operation because the
verb has to exist in firmware first.

`bench/PLAN_INSTRUMENT.md` §"Phase 5 - the script layer" already sketches this
for the instrument work, deliberately as "a small event/action vocabulary over
the primitives that already exist, so it stays testable on the bench". Tier 2
should be that same layer, not a second one.

### Tier 3, native apps

The real target, and the one the keyboard expansion exists for: CC1101
sub-GHz, nRF24L01+, ST25R3916 NFC. Apps that talk to those are the point of an
app store on this device.

Read §4b first: of that list only NFC has any driver at all, and it is
read-only with its chip-select pin commented out. Tier 3 is not blocked on a
loader decision alone - most of the hardware it would expose is unbuilt.
That is schedule, not obstacle, but a catalog planned around capabilities the
firmware cannot yet perform should know which ones those are.

The open decision - **the maintainer's, not the firmware track's and not
mine** - is loader vs. static rebuild vs. VM. The honest input from this side
is one measured constraint, not a veto: internal RAM. Recorded headroom after
P25 entry has been **2,823 bytes** (`bench/P25_LCD_MEMORY_HEADROOM.md`,
hardware, 2026-09-07), and `bench/WIFI_RESOURCE_MODES.md` records that
Flipper's own docs say FAP code copied from SD into RAM reduces headroom
versus flash-resident firmware. So a loader is not impossible here - it is
expensive, and the cost lands on the scarcest resource. Whichever way it goes
wants a queue task with a memory measurement attached, the way every other
expensive decision in this repo has one.

**What the catalog should assume:** tier 3 happens. Build the size gate, the
capability manifest, the signing model and the ABI check now, even if they sit
inert while tiers 1-2 are the only content.

## 1-2. Running model and API surface

Today: built-ins are statically compiled and registered by C pointer
(`ls_app_register`, `ls_app.h`); user apps are declarative data. There is no
loader yet, so there is no ABI yet and no symbol check to build.

What is stable today and worth binding a validator to:
- `version=1` in every `.lsapp`, refused otherwise.
- `ls_value.h` paths. An unknown path renders `--`, never zero, so an app
  written against newer firmware degrades instead of failing to install.
  A catalog check should **warn, not block**, on an unrecognised path.

When tier 3 lands, the exported API becomes the ABI and this section is
replaced. Version it from the start.

## 3. Storage

No app partition exists today; `.lsapp` files live on SD.

The 16 MB map (`boards/partitions_16m.csv`), offsets starting at `0x9000`:

| | |
|---|---|
| nvs | 24 K |
| phy_init | 4 K |
| factory (app) | 9 M |
| storage (SPIFFS) | 4 M |
| coredump | 64 K |
| **allocated** | **~13.2 MB of 16 MB** |
| **unallocated** | **~2.8 MB** |

**Correction.** An earlier version of this file said "about 6.7 MB of flash is
unallocated". That was wrong and it was the wrong kind of wrong: 6.7 MB is the
*slack inside the 9 MB factory partition* (the current image is 2,267,104
bytes), which is not free flash - it is headroom for the firmware to grow into.
The genuinely unallocated space at the end of the chip is **~2.8 MB**. Sizing a
native-app partition against the first number would have been out by 2.4x.

Note also that any new partition changes the table, and the table is what an
application-only upgrade at `0x10000` depends on staying identical. Adding one
is a full-flash event for existing boards, not an app-only update - which is a
release-process question as much as a layout one.

Tier 1 sizing is a per-file cap in `ls_userapp.h`. Tier 3 sizing is an open
question that depends on the loader decision.

## 4. Signing - and the NFC correction

**I got NFC wrong in the first version of this file and it needs saying
plainly.** I wrote that `nfc_write` was a capability gated by the transmit
broker and therefore nothing for a review tier to worry about. All three parts
were wrong, and I asserted it without checking:

- The transmit broker does not gate NFC. `grep -rni nfc components/lakeshark/radio/`
  returns **nothing**. It is a radio-TX broker; NFC is not radio TX.
- NFC write does not exist. The component is `components/lakeshark/nfc/`
  (`ls_nfc.c`, `nfc_a.c`, `st25r3916_regs.h`) and its public API is
  `ls_nfc_start` / `ls_nfc_stop` / `ls_nfc_running` / `ls_ndef_parse` - a
  read-only scanner, exactly the first slice `bench/NFC.md` scoped. A
  user-confirmed Text/URI write is explicitly a later task there.
- It is not "unreachable by design". `LS_HAS_NFC` is 0 on every board today
  only because `LS_BOARD_NFC_CS_GPIO` is commented out at
  `board/variants/t_display_p4.h:363` - the keyboard expansion pin map was
  never published (`bench/done/060`). The hardware is on the keyboard, which
  is rarely attached. That is a **gap**, not a safety property.

So: `nfc_write` is a real future capability that will need its own deliberate
-action gate, and the transmit broker will not provide it. **Keep Tier C.**

On signing: nothing verifies a signature on-device today, and for tier 1 there
is nothing to verify - the grammar has no verb. But tier 3 is native code with
hardware access, so **build the signing model now**. The device-side verify
path is a firmware task that does not exist yet and should be filed.

## 4b. What hardware capability actually exists today

Checked by grep against this tree on 2026-09-09, because the first version of
this file asserted several of these from design documents rather than from
code and got them wrong.

Judged by **public API**, not by filename. Two of my earlier "none" answers
were wrong because I searched for a part number in filenames: the SX1262
driver is called `ls_lora.c` and the TCA8418 driver is called `ls_keypad.c`,
so a filename grep reports zero for both while the drivers sit right there.

| Capability | Driver | Reach of its API | Pin declared | `LS_HAS_*` |
|---|---|---|---|---|
| RTL-SDR receive (USB) | `radio/librtlsdr.c` etc. | full receive | n/a | in use |
| Keyboard, TCA8418 | `board/ls_keypad.c`, 341 lines | full: probe, read, backlight | yes | in use |
| GPS | `board/ls_gps.c` | fix + sentence health | yes | in use |
| LoRa, SX1262 | `board/ls_lora.c`, 188 lines | **bring-up only**: `start`, `present`, `status`, `read_reg`, `diagnostics` - **no send** | yes | - |
| NFC, ST25R3916 | `lakeshark/nfc/` | **read only**: `start`/`stop`/`running`/`ndef_parse` - no write | **commented out** (`t_display_p4.h:363`) | `LS_HAS_NFC` **0** |
| Sub-GHz, CC1101 | **none** (0 files tree-wide) | - | **commented out** (`t_display_p4.h:357`) | `LS_HAS_SUBGHZ_TX` **0** |
| nRF24L01+ | **none** (0 files tree-wide) | - | - | - |

So nothing in this firmware can transmit anything today: the LoRa driver can
bring an SX1262 up and read its registers but has no send path, and the two
parts that would do sub-GHz and 2.4 GHz have no driver at all.

Corrections that fall out of this, two of them against what I first said:

- **There is no transmitter driver of any kind.** `components/lakeshark/radio/`
  contains `tx_broker.c`, `tx_policy.c`, `tx_plan.h` and `tx_driver_private.h`
  - the gate and the interface a driver would implement - and **nothing
  implements it**. Saying "transmit authorisation already exists" reads as
  "transmit exists and is gated". It does not. The gate is built and stands in
  front of zero transmitters.
- `bench/done/140-cc1101-subghz-driver.md` is filed under `done/` with **every
  done-when box unticked** and no driver in the tree. Worth reconciling: an
  inventory that says a driver landed when it did not is the same failure this
  project keeps naming.
- **The keyboard is implemented**, which I had listed as future work.
  `bench/HANDOFF_NEXT.md` (2026-09-08) says "Keyboard TCA8418 software-I2C
  reader is not implemented"; `ls_keypad.c` exists and `compact_ui.cpp` drives
  the TUI from `ls_keypad_read()`. The handoff is stale on that line.

What this means for the catalog: every Tier C capability is currently
**unbuilt**, not merely ungrantable. That does not make Tier C wrong - it
makes it early, which is the right way round.

## 5. Transmit authorisation, as actually built

This part of the first version was right and is verified - with the caveat in
§4b that it currently gates nothing, because no transmitter driver exists. From
`bench/done/190-add-fail-closed-transmit-broker.md`; public API in
`radio/tx_plan.h`:

```c
ls_radio_tx_err_t ls_radio_tx_plan(const ls_radio_tx_request_t *request,
                                   ls_radio_tx_plan_t *out);   /* validate, no RF */
ls_radio_tx_err_t ls_radio_tx_commit(ls_radio_tx_plan_id_t plan,
                                     ls_radio_tx_token_t one_shot);
void              ls_radio_tx_cancel(ls_radio_tx_plan_id_t plan);
```

`ls_radio_tx_authorize_physical()` is deliberately in `tx_broker_private.h`,
reachable only by the shell's confirmation controller. A token is bound to the
endpoint and to a digest of the whole immutable plan, consumed once, policy
rechecked immediately before keying. Transmit is denied by default and
disabled entirely with no regulatory profile selected.

**This is the pattern the other dangerous capabilities should copy** rather
than each inventing a gate: plan, show the operator the exact thing, take a
physical confirmation, mint a one-shot token, recheck at the point of action.
`nfc_write` and `gpio_raw` want the same shape. That is a firmware task and it
is not written.

## 6. Icons

`ls_icons.h`. A 12x12 monochrome bitmap - 12 strings of 12 characters, `#` for
ink - packed 2x2 into a 6x6 block of quadrant cells. No decoder, no asset, no
PSRAM. A 3x2 version is sampled from the same art so the two cannot drift.

A `.lsapp` picks one of twelve names rather than carrying art, which removes
an asset-review problem from tier 1. Custom art is the obvious tier-2/3
extension and the format to specify is the 12x12 grid above - it would want a
review pass, because twelve pixels is enough to draw something misleading.

## What the catalog track should take from this

1. **Design for tier 3.** Capability manifests, signing, size gates and an ABI
   check will all be needed; building them now costs less than retrofitting.
2. **Keep Tier C.** `subghz_tx`, `nfc_write`, `gpio_raw` are real future
   capabilities. Nothing can request them *yet*; that is a schedule fact, not
   a design guarantee.
3. **Bind to `version=1` and the value list, and warn rather than block** on
   anything unrecognised.
4. **Read caps from the headers, not from this document.** They move.
5. **Open decisions that are the maintainer's:** loader vs. rebuild vs. VM;
   whether a native-app flash partition is reserved out of the ~6.7 MB
   currently unallocated; whether device-side signature verification ships
   with tier 3 or before it.
