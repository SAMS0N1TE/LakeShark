# MESHTASTIC

Be a Meshtastic node on the board's LoRa radio (LR2021 or SX1262).

The screen: a title bar with a receive light; a header with the mesh web,
the frequency in block figures, channel use, signal history and status
chips; a `< TAB >` strip; the tab in its bezel; four finger-sized keys.
Lists follow the finger. Landscape puts the header down the left. A short
window (a desk's) folds the header into four rows. On the TFT the text stops
short of the camera hole (firmware with `ui.cutout`).

- CHAT: one conversation at a time, or all of them. CHATS lists every
  channel and every node a DM went to or came from, with what is new and
  the last line. Bubbles in each sender's colour, what this board sent on a
  filled background with its state (sending, sent, repeated, delivered, no
  ack). Tap a bubble to choose it: REPLY answers it (the answer quotes it,
  as the official apps show a reply), REACT puts an emoji on it (a tapback);
  reactions show under the bubble as chips with counts. A line in a language
  you read shows its English inside the bubble, marked `PT > EN` and so on
  (tap a chosen bubble: both, as sent, English only). The keyboard counts bytes against what one packet carries (233 on
  a channel, 222 in a private DM, less for a reply). QUICK sends a phrase
- NODES: a card a node (signal bars, hops, battery, distance and bearing,
  role, S&F), favourites first, ignored last, sorted by heard, signal,
  nearest or name. A node's page: name in block letters, DEVICE (hardware
  model, role, licensed, when last heard, hops away), the links it reports,
  signal history, a compass needle, power, route (TRACE). MORE on the page:
  DM, favourite, ask for its position, node info or telemetry, traceroute,
  ask a store-and-forward router for what it kept, ignore, remove
- MESH MAP: the mesh as a web. Links the nodes report (neighbour info) are
  drawn in cyan and decide who a node came through; others are a guess
  (dashed). A pulse runs along the path when a packet is heard. Waypoints
  show as crosses with their names. WAYPTS lists them (who, how far, when
  they go; tap twice to remove one here)
- AIR: packets a minute, the last minute as LoRa chirps, channel and airtime
  meters, every packet heard. The RANGE TEST box: what goes out, each `seq`
  heard with its SNR, RSSI and distance, and how many were missed
- SCAN: which presets people use nearby. The radio steps through the
  presets, each on its default slot, and keeps a table: packets, nodes,
  last signal, when, the channel hash heard (green: the default channel)
  and each one's share. Packets show as they come, read when they use the
  default key. Receive only (see below)
- TRANSLATE: Spanish, Portuguese, French, German, Italian, Dutch, Polish,
  Russian and Ukrainian to English on the board, no Internet; FROM picks the
  languages read, and the phrasebook sends in any of them (see below)
- SHARE (MORE > SHARE CHANNELS, or OPTIONS > CHANNELS): the channel set and
  LoRa settings as a `meshtastic.org/e/#` link and its QR code, all channels
  or the primary only. ADD LINK reads one typed in: it replaces the set, or
  adds to it when the link says `?add=true` (TRANSMIT and power never come
  from a link)
- MAP: every node and waypoint with a position appears on the board's MAP
  ("MT"; waypoints are named "WP ...")
- Alerts on a new message: the board's chirp and tick, and a banner when it
  is not on screen; OPTIONS > MODULES > ALERTS picks every message, direct
  only, or off. A muted channel never alerts
- Private DMs (PKI): X25519 keys, AES-CCM, as Meshtastic 2.5+ does. On by
  default: Meshtastic 2.7 sends no DM at all to a node that has not
  published a key
- Answers node info, position, telemetry, neighbour info, traceroute and
  ack requests; re-acks a repeated DM; answers a router's ping
- Background: keeps listening after you leave the app when it is the
  primary mesh (MESH > OPTIONS > RADIO > PRIMARY MESH, or console
  `mesh primary meshtastic`), or KEEP RUNNING = ALWAYS. A radio app that
  wants the LoRa chip (P25, FM, ADS-B on the LR2021) takes it, and the app
  listens again when that app lets go
- Remembers its nodes (keys, favourites, ignored, hardware, role), its
  waypoints and its settings between runs

## Keys
LEFT/RIGHT or 1-6 tabs, UP/DOWN scroll (NODES and CHATS: choose, ENTER
opens), B back (closes whatever is open), M more, O options.
CHAT: S send, Q quick reply, C conversations, P choose a message (again: an
older one), R reply, E react, K (in CHATS) mark all read.
NODES: G sort, N say hello (on a page: ask who it is), D direct message,
R traceroute, F favourite, V mesh map.
MESH MAP: Z zoom, L names, J waypoints. AIR: K clear.
SCAN: G go or stop, UP/DOWN choose a row, ENTER or P pin it (again: cycle),
W dwell, K clear, O its settings.
SHARE: H all channels or the primary, I add a link.
TRANSLATE: Y try a translation, A auto translate, W what to show, F the
languages to read, L the language the phrasebook and QUICK send in.

## Settings (OPTIONS)
One tree, laid out as the official apps have it. A row whose words change
(a name, a role) brings its list back fresh, with `< ALL SETTINGS` on top.

- TRANSMIT
- NODE: NAME (long name), SHORT NAME (4), LICENSED (HAM: nothing encrypted,
  no PKI, the long name a callsign, may pass the region's power cap), PRIVATE
  DMS (PKI), ROLE (CLIENT, CLIENT_MUTE, CLIENT_HIDDEN, CLIENT_BASE, TRACKER,
  SENSOR, ROUTER, ROUTER_LATE, REPEATER, LOST_AND_FOUND, TAK, TAK_TRACKER),
  REBROADCAST (all, all without decoding, local only, known only, none, core
  only), NODE INFO EVERY (1 to 24 hours, 3 by default), KEEP RUNNING
- LORA: REGION (all 26), PRESET, CUSTOM (no preset: BANDWIDTH 31.25 to 500
  kHz, SPREAD FACTOR 5-12, CODING RATE 4/5-4/8), HOP LIMIT 1-7, TX POWER,
  FREQUENCY SLOT (0 = by the channel name, else that slot), FREQUENCY OFFSET
  (kHz), OVERRIDE DUTY CYCLE
- POSITION: SHARE POSITION, POSITION EVERY, SMART POSITION (send when moved
  so many metres, not more often than so many seconds), FIXED POSITION
- CHANNELS: eight, as Meshtastic keeps them. CHANNEL picks one (< >), then
  its NAME, ROLE (primary, secondary, off; making one primary swaps it into
  slot 1), KEY (default AQ==, none, a new random AES-256 or AES-128, or type
  one), MUTE, POSITION ON IT (off, precise, 45 m to 23 km), SHARE: LINK +
  QR, ADD FROM A LINK
- MODULES: ALERTS, SEND TELEMETRY, RANGE TEST (off, log only, send every
  15 s to 5 min; a sender stops after 8 hours), NEIGHBOR INFO (off, 4 to 24
  hours; never on the default channel), AUTO TRANSLATE, QUICK IN (a list:
  English or any phrasebook language), SPEAK MESSAGES
- BACKUP TO CARD, RESTORE FROM CARD: every setting and this node's keys in
  `meshtastic/settings.bin` on the card (keep it private: it holds the key)

MORE also has: SHARE CHANNELS, ADD CHANNELS FROM A LINK, DROP A WAYPOINT
HERE (sent on the channel in use, gone after a day), SEND POSITION NOW,
SAY HELLO, QUICK REPLY, AUTO TRANSLATE, TRANSLATE FROM, MOTION, CLEAR CHAT,
CLEAR NODE LIST.

## Roles, as this node plays them
- CLIENT repeats by REBROADCAST; CLIENT_MUTE repeats nothing; CLIENT_HIDDEN
  sends no routine node info, position or telemetry and repeats only its
  own channels; CLIENT_BASE repeats like ROUTER_LATE for its favourites
- ROUTER repeats at once and always; ROUTER_LATE repeats after the others,
  always; REPEATER repeats and sends nothing routine
- TRACKER sends its position twice as often (and smart position); SENSOR
  sends telemetry at least every 30 minutes; LOST_AND_FOUND sends
  "I'm lost! Position: lat, lon" on the primary; TAK and TAK_TRACKER keep
  the routine broadcasts off

## Range test
As upstream's module: the sender broadcasts `seq N` (hop limit 0) every 15
s to 5 min; any node with RANGE TEST on writes a line a packet to
`meshtastic/rangetest-<date>-<time>.csv` on the card, in upstream's columns
(time, from, sender name, sender lat/long, rx lat/long/elevation, rx snr,
distance, hop limit, payload, rx rssi).

## Preset scan
The presets of the node's region, each on the slot its default channel
name hashes to. US: all ten (EU_868: the seven upstream allows there).

| Row | Preset | US MHz | Slot | BW kHz | SF | CR |
|---|---|---|---|---|---|---|
| 1 | LongFast | 906.875 | 20/104 | 250 | 11 | 4/5 |
| 2 | LongMod | 902.6875 | 6/208 | 125 | 11 | 4/8 |
| 3 | LongSlow | 905.3125 | 27/208 | 125 | 12 | 4/8 |
| 4 | LongTurbo | 908.75 | 14/52 | 500 | 11 | 4/8 |
| 5 | MediumFast | 913.125 | 45/104 | 250 | 9 | 4/5 |
| 6 | MediumSlow | 914.875 | 52/104 | 250 | 10 | 4/5 |
| 7 | MediumTurbo | 925.25 | 47/52 | 500 | 9 | 4/5 |
| 8 | ShortFast | 918.875 | 68/104 | 250 | 7 | 4/5 |
| 9 | ShortSlow | 920.625 | 75/104 | 250 | 8 | 4/5 |
| 0 | ShortTurbo | 926.75 | 50/52 | 500 | 7 | 4/5 |

Sync word 0x2B, preamble 16 (12 on LORA_24).

- Dwell: 10 s a preset by default (W or OPTIONS: 5 to 60 s). LongSlow gets
  three 64-byte packets' time (13 s). After a packet it stays one packet's
  time plus 1 s for a reply or a relay, never more than two dwells
- SHARE: each preset's packets a minute of listening against all of them,
  so a preset that held the radio longer does not look bigger
- PIN: one preset only, until unpinned
- The band ruler shows each preset where it sits; a tick over it once heard
- CH: the most heard channel hash. Green is the preset's default channel; `+`
  means more than one was heard (a private channel shares the slot)
- The live list: from, to, hop limit/start, hash, packet id, and what it was
  when it reads with the default key (AQ==)
- OPTIONS on SCAN: SCAN, DWELL, PIN, PRESETS (each in the cycle or out),
  CLEAR TABLE, and `< ALL SETTINGS`
- While it runs the node sends nothing and does not hear its own mesh;
  sends wait. STOP or leaving the app puts the node back
- Both radios: one preset at a time. The SX1262 and the LR2021 both retune
  per step

## Store and forward
A router announces itself (S&F on its card). MORE on its page asks it for
the last two hours; what it kept comes back marked S&F in CHAT. Routers
answer only on a private channel, as upstream.

## Primary mesh
One LoRa radio, one mesh at a time. With PRIMARY MESH = MESHTASTIC, MeshCore stops
until the next restart; open this app once and it keeps running in the background.
The board always starts on MeshCore and never opens this app by itself.
Back to MESHCORE the same way.

Buenos Aires: ANZ + MediumFast = 926.125 MHz.

## Private DMs
With PRIVATE DMS on, node info carries this node's public key, and a DM to a node
that published one is sealed for that node alone. If two tries get no ack (or the
node refuses it), the third goes on the channel key. A private DM from a node whose
key is not known yet asks that node for its node info. Meshtastic 2.7 refuses to
send a channel-key ("legacy") DM to a node without a key, so leave it on.

## Translation (offline)
The board reads chat lines without the Internet. It tells which of ten
languages a line is in (English, Spanish, Portuguese, French, German,
Italian, Dutch, Polish, Russian, Ukrainian; short lines, no accents, mixed
lines; it does not call English or Spanish anything else), writes an English
gist of a line in a language you read, and has a phrasebook of 57 lines to
send in any of the nine (mesh, signal, battery, meetup, weather, emergency).
Words it does not know, names, numbers and emoji pass through.

The screen draws Latin letters, so Russian and Ukrainian show transliterated
(BGN/PCGN without the marks: Privet, kak dela?; Pryvit, yak spravy?) and
Polish without its marks (Lodz); what is kept and sent is the line as
written. A phrasebook line in Cyrillic or Polish letters goes out as written
(UTF-8, two bytes a Cyrillic letter) after a yes on its Latin spelling and
byte count, since the keyboard cannot edit those letters.

TRANSLATE > FROM (tap a chip, or F, or MORE > TRANSLATE FROM) turns each
language on or off; Spanish alone at first. A line in a language that is off
is still named and is never read as Spanish. The chip by PHRASEBOOK (or L)
picks the language QUICK and the phrasebook send in.

- Spanish (built in, `mt_xlate.c`): chat Spanish without accents (esta /
  está, que / qué by the sentence), q, xq, pq, tb, tmb, dnd, xfa, porfa,
  finde, jaja; Rioplatense vos (tenés, sos, andá, decime), che, re, boludo,
  laburo, bondi; clitics (decime, dámelo), negation, questions turned round,
  progressive, going to, have to, perfect, future, there is / are, la casa
  de Juan: Juan's house
- Portuguese (Brazilian chat first): vc, tb, blz, kkk, tá, tô, pra, a gente,
  me manda (a request), tem alguém (is anyone), o nó da praça: the square node
- French: elisions (j'ai, l'antenne, qu'il), ne ... pas and the chat pas,
  est-ce que, inversion (es-tu), dis-moi, il y a, en train de, slt, cv, mdr, tkt
- German: the verb at the end of a clause brought forward (habe ... gekauft:
  bought; weil ich ... bin), separable verbs joined (rufe ... an: call),
  compounds split (Akkuladung: battery charge), umlauts typed or not
  (fuer, fur), moin, lg, hdl
- Italian: elisions (c'è, dell'acqua), pronouns on the end of a verb (dimmi,
  mandami, sentirti), sono arrivati (they arrived), cmq, xké, nn
- Dutch: as German (bel ... op, heb ... nodig), ff, idd, mss, k for ik
- Polish: typed with or without its letters (ł as l); słyszę and slysze both
  "I hear" when nothing says otherwise; nwm, pzdr, spoko, xd
- Russian and Ukrainian: ы э ъ ё say Russian and і ї є ґ Ukrainian, else the
  words do; also typed in chat Latin (privet, kak dela; pryvit, dyakuyu;
  horosho, ja); "be" put back where they leave it out (где ты?: where are
  you; сигнал хороший: the signal is good); спс, щас, норм
- Every one: verbs by the rules learned from Wiktionary's tables plus the
  forms they cannot make, a subject for a dropped one, object pronouns after
  the verb, not and questions with do, the perfect as a past, progressive,
  going to, have to, can, want, the adjective before its noun

A gist, not a translation: an idiom the tables lack comes out word for word.

### The language files
Spanish is in the app. The others are a file each on the card, read into
the app's memory while that language is on (155-198 KB each, 1.4 MB the
eight).

MESHTASTIC never asks for the network. The files come from:

- STORE > MESH LANGUAGES, on any board from 2.23.0. It downloads each
  language over Wi-Fi into `meshtastic/lang/<code>.bin`, where this app
  reads it, and removes it again.
- SYSTEM > PACKS > LANGUAGES, on a board after 2.23.2: packs `lang-pt`,
  `lang-fr`, `lang-de`, `lang-it`, `lang-nl`, `lang-pl`, `lang-ru`,
  `lang-uk`. A pack lands as `lakeshark/packs/lang-<code>.bin` and the app
  reads it from there first.

TRANSLATE has a `+ MORE LANGUAGES` button (also the last row of FROM, MORE >
MORE LANGUAGES and the `x` key). On a board after 2.23.2 it opens STORE at
MESH LANGUAGES' card through the `store.open` action; on 2.23.2, which has
no such action, it opens a panel that says where to get them. FROM lists a
file in `meshtastic/lang/` as `REMOVE <language> (<size>)` to give the card
space back without the network. Coming back to the app looks at the card
again, so a language just downloaded turns on by itself.

The files are also in this folder, `lang/`, and the app still takes a copy
by hand:

    meshtastic/lang/pt.bin  fr.bin  de.bin  it.bin  nl.bin  pl.bin  ru.bin  uk.bin

- With a card reader: copy the `lang` folder into the card's `meshtastic`
  folder (make it if there is none): `meshtastic/lang/pt.bin` and so on.
- Over the USB cable, a file at a time (a few minutes each): the console
  writes one folder deep, so the app also takes `meshtastic/lang-<code>.bin`:
  `python tools/sd_put.py app-sdk/apps/meshtastic/lang/pt.bin meshtastic/lang-pt.bin <port>`

FROM marks where each came from, `(PACK)` or `(CARD)`, and `(NOT ON CARD)`.
A language with no file has a red chip and "get it from STORE > MESH
LANGUAGES" (with "or PACKS" where PACKS has LANGUAGES); TRANSLATE looks
again each time it is opened. `tools/gen_xlate_lang.py`
builds the files again, and `mt_xlate_langs.h` (detection words and
phrasebooks); the downloads and commands are in its header.

## Not yet
- Admin messages (setting another node from here)
- RX BOOSTED GAIN: the board's packet API has no switch for it
- Override frequency, MQTT, serial, canned messages, paxcounter, detection
  sensor and the other upstream modules that need hardware this board has
  not, or a phone app
- More translator languages: Czech, and Japanese or Chinese, which the
  screen could only show through a transliteration not written yet
- The Slavic gists leave out "a" (I have problem) and read some plurals as
  one (in the mountain); Polish and Russian aspect is read as tense only

## How it was written
1.0 to 1.2 came from Meshtastic's published protocol description (packet header,
sync word 0x2B, 16-symbol preamble, AES-CTR, default key, presets, PKI with X25519
and AES-CCM) plus well-known protocol facts for the nonce, channel hash and
frequency slot. Checked against the published US LongFast frequency (906.875 MHz,
slot 20), EU 869.525 MHz, the default channel hash 8, the FIPS-197 / SP 800-38A
AES vectors, RFC 7748 X25519, RFC 3610 CCM and FIPS 180-4 SHA-256 vectors.

1.3 is source-derived. Meshtastic's protobufs and firmware (both GPL-3.0, the
licence LakeShark itself is under) were read and used as the reference: the field
numbers and enum values of Data (reply_id, emoji), User (is_licensed),
Waypoint, NeighborInfo, StoreAndForward, ChannelSet, ChannelSettings,
ModuleSettings and LoRaConfig; the RegionCode, ModemPreset, Role,
RebroadcastMode and HardwareModel values and names; the firmware's slot rule
(channel_num, frequency_offset, "Custom" for a custom LoRa config, "LongMod" for
LONG_MODERATE, which 1.2 had wrong); how the range test, neighbour info and store
and forward modules behave (hop limit 0 and `seq N`, the CSV columns, no
neighbour info on the default channel, routers refusing history there). The
files that carry it (`mt_proto.c`, `mt_proto.h`, `main.c`) say so in their
headers. Checked by `test_mt_proto` (bench) byte for byte against those field
numbers and against the default channel link the official apps print
(`meshtastic.org/e/#CgMSAQESCAgBOAFAA0gB`). The QR code is Project Nayuki's
QR Code generator library (MIT), unchanged but for asserts compiled out.

1.5's SCAN (`mt_scan.c`) is source-derived from the same firmware: the
preset parameters of `modemPresetToParams` (MeshRadio.h), the display names
of DisplayFormatters.cpp (the default channel names), and from
RadioInterface.cpp the region profiles (PRESETS_STD, PRESETS_EU_868), the
16 and 12 symbol preambles and the slot rule, `round(band / width)` slots
and `djb2(name) % slots`. Checked by `test_mt_scan` (bench).

Against Meshtastic 2.7.26 on a Heltec V4 (ANZ MediumFast, 2026-11-07): node info
both ways (the Heltec stored this node's key), channel text both ways, PKI DMs both
ways decrypted and acked ("PKI Decryption worked", "delivered"), traceroute both
ways with SNR, telemetry request answered (PKI-sealed reply decrypted), the
Heltec's battery and voltage on NODES.

The Spanish translator's engine and its hand tables (`tools/xlate_es_en.txt`: function
words, phrases, chat spellings, overrides, phrasebook) were written for this
app. Its lexicon (`mt_xlate_es_en.h`) is generated by `tools/gen_xlate.py`
from English Wiktionary's Spanish entries as extracted by wiktextract
(kaikki.org, dump of 2026-10-30; Wiktionary text is CC BY-SA 4.0, which may
be adapted under GPLv3): 6,615 lemmas with one short English gloss each,
chosen by how often their forms are said in the FrequencyWords Spanish list
(Hermit Dave, CC BY-SA 4.0, from OpenSubtitles 2018), plus the 836 verb
forms the C rules cannot make. Detection's English words are the top of the
same project's English list. Checked and not used: Apertium apertium-eng-spa
and apertium-spa (GPL-2.0 text with no "or any later version" anywhere, so
not GPLv3-compatible), FreeDict spa-eng (GPL-2.0-or-later, but 4,500 travel
headwords). The generator needs only Python's standard library; the
downloads and the command are in its header. Bench: `test_mt_xlate`.

The other languages' reader (`mt_xlate_lang.c`) and their hand tables
(`tools/xlate_<code>_en.txt`: function words, phrases, chat spellings,
overrides, phrasebooks) were written for this app. Their lexicons
(`lang/<code>.bin`) are generated by `tools/gen_xlate_lang.py` from English
Wiktionary's Portuguese, French, German, Italian, Dutch, Polish, Russian and
Ukrainian entries as extracted by wiktextract (kaikki.org, dumps of
2026-11-19; Wiktionary text is CC BY-SA 4.0, which may be adapted under
GPLv3): about 5,900 lemmas a language with one short English gloss each,
chosen by how often their forms are said in the FrequencyWords lists for
Brazilian Portuguese, French, German, Italian, Dutch, Polish, Russian and
Ukrainian (Hermit Dave, CC BY-SA 4.0, from OpenSubtitles 2018), with the
inflection rules the generator learns from Wiktionary's inflection tables
and the forms those rules cannot make. Russian and Ukrainian are keyed by
their BGN/PCGN transliteration, so Cyrillic and chat Latin meet the same
lexicon. The built-in detection words (`mt_xlate_langs.h`) are the most said
words of the same project's lists for all ten languages, and for Russian and
Ukrainian also the 600 most said words of each that Wiktionary does not list
as words of the other. Bench: `test_mt_xlate_lang`.

## Regions (1.2)

- The frequency follows Meshtastic's rule: the band is cut into slots of the preset's bandwidth and the
  channel name (the preset's name when empty) picks the slot with a djb2 hash, so a LongFast node on
  US lands on 906.875 MHz, EU_868 869.525, EU_433 433.875, ANZ 919.875 and so on, as other nodes do
- Power is capped at the region's limit (EU_868 27 dBm, EU_433 12, JP 13 ...)
- Duty cycle: EU_433, EU_868, UA_433 and TH allow 10 % of an hour on air, UA_868 1 %. The app keeps a
  budget over the last hour: a send over it waits (the chip says DUTY WAIT), repeats are dropped,
  and the header and AIR tab show used/allowed
- A region the radio cannot reach (the SX1262 covers 150-960 MHz; the LR2021 also has 2.4 GHz for
  LORA_24) shows BAND NOT ON THIS RADIO and does not transmit or listen
- A new install picks the region of its first GPS fix (a coarse map, change it in RADIO); an
  existing install keeps what it had (US by default)
