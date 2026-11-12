# MESHTASTIC

Be a Meshtastic node on the board's LoRa radio (LR2021 or SX1262).

- CHAT: channel text and direct messages, with send state (sent, repeated, delivered, no ack)
- NODES: name, signal, hops, battery, voltage, channel and air use, position, distance and bearing
- AIR: every packet heard, including other channels and private DMs it cannot read
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
S send (C picks the channel), D or ENTER on NODES: DM, R on NODES: traceroute,
T next tab, N say hello, O options.

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
