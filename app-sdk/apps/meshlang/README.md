# MESH LANGUAGES

The translator languages MESHTASTIC reads, over Wi-Fi. API 2.4, needs the
NETWORK permission; runs on firmware 2.23.0 and later.

Spanish is built into MESHTASTIC. Portuguese, French, German, Italian,
Dutch, Polish, Russian and Ukrainian are a file each on the card, 155-198 KB.
This app gets them, so MESHTASTIC itself never asks for the network. Its
`+ MORE LANGUAGES` button (TRANSLATE) opens STORE at this app's card.

## The list

One row per language: name, size, and where it is.

| State | Means |
|---|---|
| ON CARD | `meshtastic/lang/<code>.bin` (downloaded here) or `meshtastic/lang-<code>.bin` (copied by hand) |
| PACK | `lakeshark/packs/lang-<code>.bin`, from SYSTEM > PACKS > LANGUAGES on firmware that has it |
| UPDATE | on the card, but the pack list has another size: an older or cut-off copy |
| GET | not on the card; the pack list has it |
| GETTING | a download under way, with its bar |

The hellos in the balloon at the top are lit the same way: green on the
card, cyan a pack, grey not there. MESHTASTIC reads a pack first, then
`meshtastic/lang/`, then `meshtastic/lang-`.

## A download

1. The pack list, `https://data.terminalbay.com/v1/catalog.txt`, read when
   the app opens with Wi-Fi up: a `pack lang-<code> LANG ...` row gives the
   size, the sha256 and the path of each file.
2. One confirm: `DOWNLOAD RUSSIAN (182 KB) FROM TERMINALBAY.COM ...?`
3. `https://data.terminalbay.com/v1/packs/lang-<code>.bin` into PSRAM over
   HTTPS (the board's CA bundle and hostname check). The byte count and the
   sha256 must match the pack list, and the file must name its language.
4. Written to `meshtastic/lang/<code>.bin` in 64 KB pieces, its size checked,
   then read back and compared. Anything wrong removes the file again.
5. MESHTASTIC turns it on in TRANSLATE > FROM; coming back to MESHTASTIC
   looks at the card again.

REMOVE deletes the downloaded or hand copy (never a pack: PACKS manages
those) after a confirm.

## Keys and buttons

| Key | Button | Does |
|---|---|---|
| D | DOWNLOAD | get the selected language (or a newer copy) |
| X | REMOVE | delete the selected language's file |
| R | RELOAD | Wi-Fi, the pack list and the card again |
| UP, DOWN, tap | | select; ENTER or a second tap downloads, or removes one on the card |

With no Wi-Fi the top line says `No Wi-Fi. Connect in SETTINGS, then RELOAD`.

## Simulator

lssim's HTTPS is one text fixture (`httpbody=`, 1 KB through the script),
so a real language file cannot come through it: the screens, the pack-list
row, the bar, the hash and the card writes can be looked at, the end is a
"not a pt file" refusal. A board shows the rest.

## Data

The language files are made by `app-sdk/apps/meshtastic/tools/gen_xlate_lang.py`
from English Wiktionary (via wiktextract / kaikki.org) and the FrequencyWords
lists by Hermit Dave, both CC-BY-SA-4.0; see `app.json`.
