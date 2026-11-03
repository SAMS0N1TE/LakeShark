# Nano desktop over Ethernet

The Nano panel-runtime build enables `CONFIG_LS_NANO_ETH` by default. It starts
the P4's internal EMAC and IP101GRI PHY, obtains an IPv4 address with DHCP, and
keeps the C5 Wi-Fi netif independent. It never enables the terminal web server.
The existing `term web on/off` preference, password and browser token remain in
the `lsterm` NVS namespace. A fresh board has the web server off; a previously
saved owner choice to enable it still applies after reboot.

## Wiring and sources

The source for the **C5 / rev 3 Nano** is Waveshare's
[ESP32-P4-NANO-WIFI6-DB documentation, Peripheral Quick Reference and GPIO Allocation](https://docs.waveshare.com/ESP32-P4-NANO-WIFI6-DB).
It specifies IP101GRI, PHY address **1**, reset **GPIO51** (active low), and an
external **50 MHz REF_CLK input**. The P4 defaults in ESP-IDF 5.5.4
`components/esp_eth/include/esp_eth_mac_esp.h`,
`ETH_ESP32_EMAC_DEFAULT_CONFIG()` for `CONFIG_IDF_TARGET_ESP32P4`, match these pins:

| Signal | P4 GPIO |
| --- | --- |
| CRS_DV | 28 |
| RXD0 / RXD1 | 29 / 30 |
| MDC / MDIO | 31 / 52 |
| TXD0 / TXD1 | 34 / 35 |
| TX_EN | 49 |
| REF_CLK input | 50 |
| PHY reset | 51 |

The vendored `components/esp32_p4_wifi6_touch_lcd_4b` BSP does not supply a
Nano Ethernet configuration. The vendored mDNS example's `MDC23/MDIO18/reset5`
settings are for the original ESP32 Ethernet kit and **must not be used** here.
`main/ls_eth.c` uses the IDF P4 defaults and explicitly sets PHY address/reset.
Ethernet is gated by both Nano board selection and panel-runtime; it does not
initialize a PHY on T-Display builds. The additional desktop font assets are
also embedded only for panel-runtime, avoiding a T-Display flash-size increase.

## Owner setup

1. Build and flash the Nano/C5/panel-runtime firmware using your normal process.
   Example build with ESP-IDF 5.5.4:

   ```powershell
   idf.py -B build_desk_eth -D SDKCONFIG=build_desk_eth/sdkconfig -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;boards/nano_headless_nobt.defaults;boards/nano_dsi.defaults;boards/nano_c5.defaults' build
   ```

2. Power the Nano. Connect its RJ45 to a router/switch with an Ethernet cable;
   put the computer on the same reachable LAN. USB may supply power, but the
   Ethernet desktop does not hold the USB serial port. No SD card is needed.
3. In your console, run `eth`. Expect `on link=up speed=100M ip=...` after DHCP.
   If Ethernet was previously disabled, run `eth on`.
4. Run `term web on` yourself. It prints the URL and the current password,
   generating a password only when none exists. `term net show` shows the saved
   password; `term net pass <password>` changes it and invalidates old tokens.
5. Open **`http://<the-eth-IP>/`** in a current Edge/Chrome/Firefox browser and
   sign in with that password. The board serves the page, inlined JavaScript,
   glyph atlas, icons and DejaVu fonts directly from flash. The LAN HTTP/WS
   service uses the existing unencrypted password/token protocol.
6. Optional Windows launcher:

   ```powershell
   python host/lsterm/lsdesk.py --host 192.168.1.17
   # The same direct-page shortcut is available in lsweb:
   python host/lsterm/lsweb.py --host 192.168.1.17
   ```

   The IP is saved as `host` in `~/.lsdesk.json`. Explicit/remembered IP mode
   needs no serial port or PC bridge. With no IP saved, `lsdesk` uses its normal
   board finder, briefly asks `eth` at 115200 without DTR/RTS reset, closes the
   port, then opens the board URL when Ethernet has an address. It does not send
   `term web on` or change credentials. If no wired address is available it uses
   the existing USB bridge. `lsdesk.py --serial` forgets the saved host and forces
   USB. If DHCP changes a remembered address, supply the new `--host` address.

## Console

| Command | Effect |
| --- | --- |
| `eth` | Enabled/running state, link, negotiated speed, IPv4/mask/gateway, MAC and RX/TX counts |
| `eth on` / `eth off` | Start/stop Ethernet; save the choice in NVS |
| `eth static 192.168.1.17 255.255.255.0 192.168.1.1` | Apply and save static IPv4 settings, including when Ethernet is currently off |
| `eth dhcp` | Return to DHCP and save that choice |
| `term web on` / `term web off` | Owner's persistent web-server choice |
| `term web kick` | Disconnect authenticated browsers |
| `term net show` / `term net pass <password>` | Inspect/change the existing shared terminal password |

Ethernet settings live in NVS namespace `lseth`. RX/TX counters count L2 frames
and bytes delivered to lwIP / accepted by the MAC, including ARP/DHCP. They reset
on reboot and wrap at 32 bits. Ethernet route priority is 150 versus STA's 100;
IDF falls back to Wi-Fi when Ethernet goes down. A listener is created only
after an up netif has a nonzero IP. Losing every usable interface closes web
connections; reconnecting recreates the listener only if the owner enabled it.

## Desktop transport and bounds

The web session already used the shared renderer and input decoder. Missing
glyph/font HTTP routes previously made a direct board page fall back to text.
The new routes allow `cells` negotiation and the existing `term caps ev1 pix1
vec1`, `term visual`, C/X/P/G cells/palette/grid/font and I/O/V/L picture/mask/vector/
latency records, input epochs, acknowledgements and browser latency meter.
`screen.js` is inlined into the gzipped page by the existing `pack.py`; Studio
and CodeMirror are not embedded. PC-only Studio/console RPC endpoints remain on
the USB bridge.

The glyph `.gz` route deliberately has **no** HTTP `Content-Encoding: gzip`:
the page itself decompresses this file. Adding that header would inflate it
twice and break cell negotiation. HTML does carry its normal gzip header.

Web visual encoding receives **2 MiB raw bytes per tick**, serial retains
**512 KiB**. This bounds pixel payload rather than counting pixels directly
(RGB565 consumes two bytes per pixel plus record/mask overhead). Existing
visual cadence and input/ack priorities are unchanged.

Each connection has a lazy PSRAM queue capped at **3 MiB**, with one encoded
screen update pending. A slow viewer skips encoding intermediate snapshots and
then gets a delta from the latest snapshot; bytes from an already-started delta
are never dropped. Socket sends use `MSG_DONTWAIT` and bounded service passes.
HTTP assets are streamed from flash without copying their bodies into the
queue. Five seconds without send progress closes a stalled client; overflow or
allocation failure closes it so the next connection starts with a full
baseline. A new viewer wins, releasing old held input and draining the old
viewer's “taken” notification before closing it to prevent reconnect fights.

## Validation and remaining hardware checks

Only these added/modified host tests were run:

```powershell
python -B bench/tests/test_desk_eth.py
python -B bench/tests/test_rterm_memory.py C:/mingw64/bin/gcc.exe
```

The desktop suite covers real C WebSocket headers and masked-frame parsing,
large frames, malformed frames, pre-netif socket guards, DHCP waits, partial/EAGAIN sends, bounded queues, allocation
failure, budget selection, production web takeover/stall timeout/HTTP ordering,
embedded page/assets, `--host`, remembered config, USB fallback and fake-console
IP discovery. The memory suite checks grid allocation failures, reconnects,
UART/SSH ownership and allocation bounds.

The full firmware build was attempted but dependency resolution could not reach
Espressif's component registry (`david-cermak/libssh.json`). The changed firmware
translation units passed ESP compiler syntax checks against an existing Nano/C5
configuration and the unaffected T-Display configuration; this is not a full link or size check. The owner still needs to
build/flash and verify DHCP/static addressing, cable hotplug, simultaneous C5
Wi-Fi, password login, desktop input/pixels/latency, negotiated throughput and a
slow browser on hardware. No board or COM port was accessed during development.
