# MESHTASTIC

Be a Meshtastic node on the board's LoRa radio (LR2021 or SX1262).

The screen (1.0): a title bar with a receive light; a header with the mesh
web, the frequency in block figures, channel use, signal history and status
chips; a `< TAB >` strip; the tab in its bezel; four finger-sized keys.
Lists follow the finger. Landscape puts the header down the left.

- CHAT: bubbles in each sender's colour, what this board sent on a filled
  background with its state (sending, sent, repeated, delivered, no ack);
  Spanish shows its English inside the bubble (tap a bubble: both, as sent,
  English only). QUICK sends a phrase from the phrasebook, in Spanish by default
- NODES: a card a node (signal bars, hops, battery, distance and bearing),
  sorted by heard, signal, nearest or name; tap one for its page: name in
  block letters, signal history, a compass needle, power, route (TRACE)
- MESH MAP: the mesh as a web: each node linked to the one it most likely
  came through (solid direct, dashed relayed), a pulse along the path when
  a packet is heard and out along every link when one is sent; by bearing and
  distance on a map grid with a position here (GPS or FIXED), by hops without
- AIR: packets a minute (half an hour), the last minute as LoRa chirps,
  channel and airtime meters, every packet heard, including other channels
  and private DMs it cannot read
- TRANSLATE: Spanish to English on the board, no Internet: a try-it box, a
  HISTORY of the last 16 lines tried or heard in Spanish (kept on the card;
  tap one to read it whole in the box, CLEAR twice to empty it), the
  phrasebook (tap to send). AUTO translates as messages arrive; SHOW picks
  both, English only or as sent
- MAP: every node that shares a position appears on the board's MAP (green, "MT")
- Sends text, direct messages (acked, up to 3 tries), node info, position, device telemetry
- Up to four channels: the primary plus three more, each with its own name and key (base64)
- Private DMs (PKI): X25519 keys, AES-CCM, as Meshtastic 2.5+ does. On by default:
  Meshtastic 2.7 sends no DM at all to a node that has not published a key
- Traceroute (R on NODES): the path there and back with the SNR of every hop
- Answers node info, position, telemetry, traceroute and ack requests; re-acks a repeated DM
- RELAY OTHERS: repeats packets for the mesh and adds itself to traceroutes it carries
- Background: keeps listening after you leave the app when it is the primary mesh
  (MESH > OPTIONS > RADIO > PRIMARY MESH, or console `mesh primary meshtastic`),
  or KEEP RUNNING = ALWAYS. New messages show a banner on any screen. A radio app
  that wants the LoRa chip (P25, FM, ADS-B on the LR2021) takes it, and the app
  listens again when that app lets go
- Remembers its node list (and the keys nodes published) between runs

## Keys
LEFT/RIGHT or 1-5 tabs, UP/DOWN scroll (NODES: choose, ENTER opens), S send,
C channel, Q quick reply, D direct message and R traceroute on a node's page,
B back, G sort, N say hello, Z zoom and L names on MESH MAP, Y try a
translation, A auto translate, W what to show, M more, O options.

## Settings (OPTIONS) added in 1.0
- TRANSLATE: AUTO TRANSLATE, SHOW (both, English only, as sent), QUICK REPLIES IN,
  SPEAK MESSAGES (off, translations, every message: read aloud in the board's voice)
- SCREEN: MOTION (full, calm: nothing moves), MAP NAMES (names on MESH MAP)

## Primary mesh
One LoRa radio, one mesh at a time. With PRIMARY MESH = MESHTASTIC, MeshCore stops
until the next restart; open this app once and it keeps running in the background.
The board always starts on MeshCore and never opens this app by itself.
Back to MESHCORE the same way.

## Settings (OPTIONS)
- TRANSMIT
- RADIO: REGION (US, ANZ, EU_868, EU_433, NZ_865, IN, KR, SG_923; Argentina = ANZ), PRESET, POWER
- CHANNELS + KEYS: PRIMARY NAME (empty = preset name; the name sets the frequency slot),
  PRIMARY KEY (AQ== default, empty = none), CHANNEL 2..4 as `name key`
- POSITION: SHARE POSITION (off, exact, ~1 km), POSITION EVERY, FIXED POSITION, SEND POSITION NOW
- THIS NODE: MY NAME, PRIVATE DMS (PKI), SEND TELEMETRY (1 h, 30 min, 3 h, off), SEND TELEMETRY NOW,
  RELAY OTHERS, KEEP RUNNING, SAY HELLO
- CLEAR LISTS

Buenos Aires: ANZ + MediumFast = 926.125 MHz.

## Private DMs
With PRIVATE DMS on, node info carries this node's public key, and a DM to a node
that published one is sealed for that node alone. If two tries get no ack (or the
node refuses it), the third goes on the channel key. A private DM from a node whose
key is not known yet asks that node for its node info. Meshtastic 2.7 refuses to
send a channel-key ("legacy") DM to a node without a key, so leave it on.

## Translation (Spanish to English, offline)
`mt_xlate.c` reads chat lines without the Internet: it tells Spanish from
English (short lines, no accents, mixed lines; it does not call English
Spanish), writes an English gist of a Spanish line, and has a phrasebook of
57 lines with the Spanish to send (mesh, signal, battery, meetup, weather,
emergency; Rioplatense vos). Words it does not know, names, numbers and
emoji pass through.

- Chat Spanish: no accents (esta / está, que / qué by the sentence), q, xq,
  pq, tb, tmb, bn, dnd, xfa, porfa, msj, finde, jaja; holaaa; manana for mañana
- Rioplatense: vos forms (tenés, sabés, sos, andá, decime), che, re (re lindo),
  boludo, laburo, bondi, birra, quilombo, posta, al toque
- Grammar: verbs by rule plus the forms the rules cannot make, clitics (te
  quiero, decime, dámelo), negation (no tengo: I don't have), questions turned
  round, progressive, going to, have to, can, perfect, future, conditional,
  adjective before noun, there is / are, la casa de Juan: Juan's house
- The engine is pair-neutral on the English side: another language into
  English needs a data header and a reader for that language

Not wired into the screens yet: `mt_xlate_incoming()` is the one call for a
received text (see `mt_xlate.h`).

## Not yet
- Admin messages, waypoints, neighbour info, store and forward

## How it was written
From Meshtastic's published protocol description (packet header, sync word 0x2B,
16-symbol preamble, AES-CTR, default key, presets, PKI with X25519 and AES-CCM)
plus well-known protocol facts for the nonce, channel hash and frequency slot. No
Meshtastic firmware source was used. Checked against the published US LongFast
frequency (906.875 MHz, slot 20), EU 869.525 MHz, the default channel hash 8, the
FIPS-197 / SP 800-38A AES vectors, RFC 7748 X25519, RFC 3610 CCM and FIPS 180-4
SHA-256 vectors. Board to board on the LR2021 and SX1262.

Against Meshtastic 2.7.26 on a Heltec V4 (ANZ MediumFast, 2026-11-07): node info
both ways (the Heltec stored this node's key), channel text both ways, PKI DMs both
ways decrypted and acked ("PKI Decryption worked", "delivered"), traceroute both
ways with SNR, telemetry request answered (PKI-sealed reply decrypted), the
Heltec's battery and voltage on NODES.

The translator's engine and its hand tables (`tools/xlate_es_en.txt`: function
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
