# MESH

A MeshCore node: hear other nodes, relay for them, and send short messages over LoRa with no other network.

| | |
|---|---|
| HOME group | RADIO |
| Radio | The LoRa chip (SX1262 or LR2021) |

## What it does

- Runs a MeshCore node on the board's LoRa radio.
- Shows messages on the public channel and on private channels you add keys for.
- Sends channel messages and direct messages to one node.
- Lists the nodes in range with signal, age, position, distance and bearing.
- Saves nodes as contacts, finds them on the map, and queries them with INSPECT.
- Tells the rest of the board when a message arrives, with an optional spoken read out.

## Radio

MESH always uses the LoRa chip. Its RADIO settings live in OPTIONS > RADIO, and R on the NODES page opens them. Other apps that need the LoRa chip, such as LORA LABS or a LoRa sweep in FALLS, pause the mesh while they use it.

## How to use it

MESH has two pages, CHAT and NODES, plus OPTIONS. In portrait they are tabs along the top. In landscape they are along the bottom. Left and Right change page.

The node starts disarmed. It listens, but it does not transmit until you arm it.

- **ARM TX / DISARM TX**: the large button at the bottom arms or disarms the transmitter. It shows NOT RUNNING until the mesh is started.
- The MIC key arms and disarms. Holding MIC turns auto transmit on or off, which arms the node at every boot.
- Start the mesh from OPTIONS > MESH, or type `/start`.

### CHAT

Tap TAP TO WRITE A MESSAGE to open the keyboard. With a keyboard attached, type and press Enter. Up and Down scroll the messages. Esc clears the draft, then leaves a private chat.

Messages that start with `/` are commands. `/help` lists them:

| Command | Action |
|---|---|
| /start, /stop | Start listening disarmed, or stop |
| /arm, /disarm | Enable or disable transmit |
| /advert | Announce this node |
| /name X | Save this node's name |
| /to X | Chat privately with a node by id or name |
| /pub | Back to the channel |
| /options, /radio | Open settings |

### NODES

Up and Down select a node. Enter, or a second tap on the same node, opens it. On the list, S starts or stops the mesh, T arms or disarms, A sends an advert, R opens the radio settings and O opens OPTIONS.

A node's page has four actions:

| Key | Action |
|---|---|
| D | MESSAGE: lock the chat to this node |
| S | SAVE: keep it as a contact |
| M | MAP: open the map on its position |
| I | INSPECT: query the node |

INSPECT offers TRACE (P), TELEM (T), STATUS (S), LOGIN (L), OPTIONS (O) and BACK (B). Esc leaves a page.

### OPTIONS

O, or the OPTIONS tab, opens the list. BACK goes up one level.

- MESH (start or stop), TRANSMIT (arm) and SEND ADVERT.
- INSPECT: TIMEOUT, MIN INTERVAL, AUTO REPEAT, LOGIN PASSWORD, ANSWER THIS NODE and FORGET CONTACT.
- NODE: NAME, ADVERT EVERY, ROLE (chat node, repeater, room server or sensor), FORWARDING, LOCATION (off, GPS or manual), LATITUDE and LONGITUDE.
- RADIO: BAND, FREQUENCY, SPREADING, BANDWIDTH, CODING RATE, POWER, SYNC WORD, CRC and RESET RADIO.
- CHANNELS: SEND ON, and CHANNEL 0 to 3. Channel 0 is the fixed public channel; the others take a base64 key.
- AT BOOT: LISTEN, TRANSMIT and ADVERT.
- VOICE: READ OUT (OFF, SENDER, or SENDER + TEXT), MESSAGES (ALL or DIRECT ONLY), and voice level settings.
- ADD CONTACT: enter a node's 64 character public key.

## What the screen shows

- The node name and id.
- The state: OFFLINE, LISTENING, ARMED, ON AIR, RX FAULT or RADIO HELD.
- HEARD and SENT packet counts, and the last signal and SNR.
- The message feed, then the compose bar.
- NODES IN RANGE with a signal scope below it.
- A node's page: signal and SNR, when it was heard, its position, distance and bearing from you, its public key and a signal history.

## Tips and limits

- A node appears when it advertises. If the list stays empty, check that both ends match on frequency, spreading factor and bandwidth.
- Sync word 0x12 is the MeshCore mesh. CRC off means other nodes drop your packets.
- Distance and bearing need a GPS fix or a saved home.
- Node sightings are saved with the position you heard them from, and exported as GPX waypoints beside the GPS track.

## More guides

- [Controls and navigation](../UI_CONTROLS.md)
- [LoRa Labs and Notes](../FIELD_LABS.md)
- [All apps](README.md)
