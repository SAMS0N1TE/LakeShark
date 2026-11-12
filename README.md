<p align="center">
  <img src="assets/lakeshark-banner.png" alt="LakeShark" width="900">
</p>

<h3 align="center">A handheld SDR powered by a LoRa chip, RTL-SDR, and HackRF</h3>

<p align="center">
  <strong>P25 Digital Voice · FM Radio · ADS-B Aircraft · Pagers · MeshCore · RF Tools</strong>
</p>

<p align="center">
  Turn the <a href="https://lilygo.cc/en-us/products/t-display-p4">LILYGO T-Display P4</a>, Waveshare ESP32-P4 Nano DB, and Wifi6, into an open-source handheld radio multitool.
</p>

<p align="center">
  <strong><a href="https://terminalbay.com/?m=lakeshark">Install LakeShark</a></strong>
  &nbsp;·&nbsp;
  <strong><a href="https://terminalbay.com/?m=lakeshark-showcase">See it in action</a></strong>
  &nbsp;·&nbsp;
  <strong><a href="https://terminalbay.com/?m=wiki">Documentation</a></strong>
</p>

<h2 align="center">LR2021 LoRa Chip</h2>

<p align="center"><strong>The onboard LR2021 receives P25 voice, aircraft, Analog FM and pagers. No external SDR required.</strong></p>

<p align="center"><strong>Raw I/Q capture · Live waterfalls · Software demodulation · Experimental I/Q transmission</strong></p>

<p align="center"><strong>New in 2.20.0: the LR2021 runs programs on its own CPU.</strong><br>
On-chip waterfall · On-chip down-conversion · Background LoRa scan · FM at half the CPU</p>

<p align="center">The programs on the radio come from <a href="https://github.com/beala/lr2021-re">Alex Beal's lr2021-re</a>, which builds on LR2021-SDR: his on-chip FFT, sweep and LoRa scanner, and the ARC toolchain and Patch RAM loader we used for our own. <a href="https://beala.substack.com/p/your-node-doesnt-need-a-microcontroller">Read his write-up</a>.</p>

<p align="center"><strong><a href="https://github.com/SAMS0N1TE/lr2021-sdr">Explore LR2021-SDR →</a></strong></p>
<p align="center">
  <img src="assets/lakeshark-divider.png" alt="" width="900">
</p>
<table align="center">
  <thead>
    <tr><th>Hardware</th><th>Use</th><th>Firmware</th></tr>
  </thead>
  <tbody>
    <tr><td><strong>LILYGO T-Display P4</strong> (AMOLED, SX1262 or LR2021)</td><td>Handheld</td><td>2.21.3 stable</td></tr>
    <tr><td><strong>LILYGO T-Display P4 TFT</strong></td><td>Handheld</td><td>2.21.3 stable</td></tr>
    <tr><td><strong>LILYGO T-Display P4, LR edition</strong> (LR2021 only)</td><td>Handheld</td><td>2.21.3 stable</td></tr>
    <tr><td><strong>Waveshare ESP32-P4-NANO-DB</strong> (P4 rev 3, ESP32-C5)</td><td>LakeShark Desktop</td><td>2.21.2 stable</td></tr>
    <tr><td><strong>Waveshare ESP32-P4-NANO</strong></td><td>Headless (serial, Flipper)</td><td>2.0.2 in the installer, newer from source</td></tr>
    <tr><td><strong>Waveshare Touch-LCD-4.3, Touch-LCD-4B, Smart 86 Box</strong></td><td>Touch screen</td><td>Up to 2.16.4, no longer updated</td></tr>
    <tr><td><strong>RTL-SDR Blog/Nooelec V3/V4</strong></td><td>USB SDR</td><td>All current builds except the LR edition</td></tr>
    <tr><td><strong>HackRF One</strong></td><td>USB SDR (experimental)</td><td>All current builds except the LR edition</td></tr>
    <tr><td><strong>MIX-RF keyboard</strong></td><td>Keys, CC1101, nRF24, NFC</td><td>T-Display P4 builds</td></tr>
    <tr><td><strong>Flipper Zero</strong></td><td>Control head (LakeShark-Flipper app)</td><td>T-Display P4 and headless NANO</td></tr>
  </tbody>
</table>
<table align="center">
  <thead>
    <tr>
      <th>Hardware</th>
      <th>Capabilities</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td><strong>T-Display P4 + LR2021</strong></td>
      <td>Onboard P25 voice, FM, ADS-B, pagers, MeshCore and LoRa tools</td>
    </tr>
    <tr>
      <td><strong>T-Display P4 + SX1262</strong></td>
      <td>MeshCore, LoRa and POCSAG. Add an RTL-SDR for FM, P25 and ADS-B</td>
    </tr>
    <tr>
      <td><strong>RTL-SDR Blog V3/V4</strong></td>
      <td>External SDR reception, P25 Phase I trunking and sub-GHz captures</td>
    </tr>
    <tr>
      <td><strong>MIX-RF keyboard</strong></td>
      <td>Optional physical controls, CC1101, nRF24 and NFC</td>
    </tr>
  </tbody>
</table>

<p align="center">Install from Chrome or Edge with the <a href="https://terminalbay.com/?m=lakeshark">browser installer</a>. After that, the <strong>UPDATE</strong> app updates over Wi-Fi.</p>

<blockquote><p align="center"><strong>Power:</strong> use the P4's own USB-C port, not the keyboard's 5V output, or it keeps restarting.</p></blockquote>

<h2 align="center">LakeShark Desktop and Studio</h2>

<p align="center"><strong>Run every app in windows on your PC. Write your own in minutes.</strong><br>New in 2.20.0.</p>

<h3 align="center"><a href="https://github.com/SAMS0N1TE/LakeShark/blob/main/app-sdk/README.md#your-first-app-in-5-minutes">Make your first app →</a></h3>

<p align="center">
  <a href="https://www.waveshare.com/esp32-p4-nano-wifi6-db.htm"><picture>
    <source media="(prefers-color-scheme: dark)" srcset="docs/screenshots/desktop/esp32-p4-nano-paper-20261101.png">
    <img src="docs/screenshots/desktop/esp32-p4-nano-ink-20261101.png" width="300" alt="Waveshare ESP32-P4-NANO board">
  </picture></a>
</p>

<p align="center"><strong>Real hardware, not an emulator (but also an emulator too).</strong> The apps run on a
<a href="https://www.waveshare.com/esp32-p4-nano-wifi6-db.htm">Waveshare ESP32-P4-NANO-DB</a> with its radios and SD card. Your PC is its
screen, mouse and keyboard over one USB cable.</p>

<p align="center">
  <img src="docs/screenshots/desktop/desk-p25-20261101.png" width="49%" alt="P25 in a desktop window, receiver health and signal" />
  <img src="docs/screenshots/desktop/studio-20261101.png" width="49%" alt="LakeShark Studio with an app running in the simulator" />
</p>
<p align="center">
  <img src="docs/screenshots/desktop/music-20261101b.gif" width="49%" alt="MUSIC playing on the desktop, meters and waterfall" />
  <img src="docs/screenshots/desktop/focus-map-20261101.png" width="49%" alt="Focused mode: MAP alone at the full frame rate" />
  
<p align="center">
  Run every app in its own window on your PC, or press Ctrl+F11 to give one app the whole screen at twice the frame rate. Write your own apps in Studio, test them in the simulator and put them on the board, or do the same from a terminal with lsdk. The app store installs anything in one click.
</p>

<p align="center">
  <a href="https://github.com/SAMS0N1TE/LakeShark/releases/tag/v2.21.3">Get 2.21.3</a>
  &nbsp;·&nbsp;
  <a href="https://github.com/SAMS0N1TE/LakeShark/blob/main/app-sdk/README.md#your-first-app-in-5-minutes">Make your first app</a>
</p>

<h2 align="center">What's New</h2>

<p align="center"><strong>2.21.3:</strong> FREQS frequency library with imports and NEAR · MESHTASTIC app · Lists scroll under a finger · FM waterfall ZOOM to 32x</p>
<p align="center">SUB-GHZ WATCH, READ and BAND · P25 shows why a grant was not followed · The LR2021 moves cleanly between apps</p>
<p align="center"><strong>2.21:</strong> DOPPLER RADAR with two boards · FREQ OFFSET for crystal LR2021 modules · UAT 978 hears every frame</p>
<p align="center">New SETTINGS pages · POWER modes with WATCH · GPS standby saves about 23 mA · Redrawn MUSIC views</p>
<p align="center"><strong>2.21.1 and 2.21.2:</strong> Sharp street-level MAP zooms · RTL-SDR Blog V4 FM and REC · Steady P25 GPS sites</p>
<p align="center"><strong>2.20:</strong> LR2021 on-chip waterfall and down-conversion · LakeShark Desktop · App SDK 2.6 and Studio · <a href="https://terminalbay.com/?m=lakeshark-apps">App store with one-click installs</a></p>

<p align="center">
  <img width="48%" alt="CARRIER, an early access space game" src="docs/screenshots/apps/carrier-0.4.gif">
  <img width="48%" alt="DICE, an example app" src="docs/screenshots/apps/dice-showcase.gif">
</p>

<p align="center">
  <a href="https://github.com/SAMS0N1TE/LakeShark/releases/tag/v2.21.3">Release notes</a>
  &nbsp;·&nbsp;
  <a href="https://github.com/SAMS0N1TE/LakeShark/releases">All releases</a>
</p>

<p align="center">
  <img src="assets/lakeshark-divider.png" alt="" width="900">
</p>

<h2 align="center">Meet the LakeShark</h2>

<p align="center">
  <img width="900" alt="LakeShark on the LILYGO T-Display P4" src="assets/lakeshark-tdp4.jpg">
</p>

<p align="center">
  <strong>LILYGO T-Display P4 · 4.1&quot; AMOLED · ESP32-P4 · LR2021 or SX1262</strong><br>
  Touchscreen, GPS, motion sensing, and an optional detachable MIX-RF keyboard.
</p>

<p align="center">
<img src="docs/screenshots/tdp4/home-20261025.png" width="32%" alt="HOME" />
<img src="docs/screenshots/tdp4/motion/music-scan-20261014.gif" width="32%" alt="MUSIC analyzer and waterfall" />
<img src="docs/screenshots/tdp4/motion/adsb.gif" width="32%" alt="ADS-B" />
</p>

<details>
<summary><strong>More screenshots</strong></summary>
<br>

<p align="center">
<img src="docs/screenshots/tdp4/motion/p25-call-20261022.gif" width="32%" alt="P25 decoding a call" />
<img src="docs/screenshots/tdp4/motion/music-waterfall-smooth-20261022.gif" width="32%" alt="MUSIC waterfall drawing a picture hidden in a WAV" />
<img src="docs/screenshots/tdp4/motion/boot-splash-20261022.gif" width="32%" alt="Boot globe and progress line" />
<img src="docs/screenshots/tdp4/motion/update.gif" width="32%" alt="UPDATE over WiFi" />
<img src="docs/screenshots/tdp4/motion/map.gif" width="32%" alt="MAP following an aircraft" />
<img src="docs/screenshots/tdp4/motion/compass.gif" width="32%" alt="COMPASS FIND" />
<img src="docs/screenshots/tdp4/motion/falls-chip-20261103.gif" width="32%" alt="FALLS on the LR2021, computed on the chip" />
<img src="docs/screenshots/tdp4/motion/subghz_scan2.gif" width="32%" alt="SUB-GHZ scan" />
<img src="docs/screenshots/tdp4/motion/subghz-lr2021-hackrf-ook-decode-20261014.gif" width="32%" alt="SUB-GHZ decoding a Flipper" />
<img src="docs/screenshots/tdp4/motion/fm-pager-20261025.gif" width="32%" alt="FM on a busy VHF channel" />
<img src="docs/screenshots/tdp4/motion/music-bands-20261022.gif" width="32%" alt="MUSIC 2/3 octave bands" />
<img src="docs/screenshots/tdp4/motion/mesh-notice.gif" width="32%" alt="Mesh message notices" />
<img src="docs/screenshots/tdp4/gallery-20261007/experiments-port.png" width="32%" alt="EXPERIMENTS" />
<img src="docs/screenshots/tdp4/motion/night-theme.gif" width="32%" alt="Night theme" />
<img src="docs/screenshots/tdp4/files-to-music-20261022.png" width="32%" alt="FILES sending a P25 call to MUSIC" />
</p>

</details>

## Applications

About thirty apps: P25 voice and trunking, FM with pagers, ADS-B, waterfalls, MeshCore, Sub-GHz capture and replay, FT8, offline maps, compass, GPS and music. Each one has a guide in the [wiki](https://terminalbay.com/?m=wiki#tdp4/APPS).

The [web tools](https://terminalbay.com/?m=lakeshark) make P25 profiles, GPS scan lists, channel memories and map tiles. See also [replay and aircraft details](docs/REPLAY_AND_AIRCRAFT.md).

### Headless

The **Waveshare ESP32-P4-NANO** and **ESP32-P4-WIFI6** run the same receivers without a screen. Control them from a serial console, a Flipper Zero or the [remote terminal](#remote-terminal). On the NANO the terminal is the full interface; `panel term 160x40` sets its size.

The [LakeShark-Flipper app](https://github.com/SAMS0N1TE/LakeShark-Flipper) uses a Flipper Zero as the screen and buttons. It connects over Bluetooth (no pairing code) or GPIO.

<table align="center">
  <thead>
    <tr>
      <th align="center">Offline aircraft map</th>
      <th align="center">ADS-B traffic</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td align="center"><img src="https://raw.githubusercontent.com/SAMS0N1TE/LakeShark-Flipper/9eff8f06eb8899a3ac1b6058837897c3fe8bbbda/docs/screenshots/adsb_map.png" alt="Offline aircraft map" /></td>
      <td align="center"><img src="https://raw.githubusercontent.com/SAMS0N1TE/LakeShark-Flipper/9eff8f06eb8899a3ac1b6058837897c3fe8bbbda/docs/screenshots/adsb_traffic.png" alt="ADS-B traffic" /></td>
    </tr>
    <tr>
      <td align="center">App launcher</td>
      <td align="center">P25 radio</td>
    </tr>
    <tr>
      <td align="center"><img src="https://raw.githubusercontent.com/SAMS0N1TE/LakeShark-Flipper/9eff8f06eb8899a3ac1b6058837897c3fe8bbbda/docs/screenshots/launcher.png" alt="App launcher" /></td>
      <td align="center"><img src="https://raw.githubusercontent.com/SAMS0N1TE/LakeShark-Flipper/9eff8f06eb8899a3ac1b6058837897c3fe8bbbda/docs/screenshots/p25_vfo.png" alt="P25 radio" /></td>
    </tr>
  </tbody>
</table>

<p align="center">
  <strong><a href="https://terminalbay.com/?m=wiki#flipper/Home">Flipper setup guide →</a></strong>
</p>

## Experimental features

<table align="center">
  <thead>
    <tr>
      <th>Feature</th>
      <th>Status</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td><strong>P25 Phase II</strong></td>
      <td>Grant following, bench-tested with public recordings; live RF unverified. Off after restart. <a href="docs/P25_PHASE2.md">Details</a></td>
    </tr>
    <tr>
      <td><strong>HackRF One</strong></td>
      <td>FM, POCSAG, ACARS, P25, recording and ADS-B. Pagers verified; live P25 unconfirmed. <a href="docs/HACKRF_BRINGUP.md">Status</a></td>
    </tr>
    <tr>
      <td><strong>LR2021 I/Q TX</strong></td>
      <td>Short custom waveforms, including P25 test tones and temperature packets. <a href="docs/LR2021_IQ_TX.md">Research</a></td>
    </tr>
    <tr>
      <td><strong>LR2021 I/Q replay</strong></td>
      <td>433 MHz captures. OOK replay verified; FSK not yet verified over the air</td>
    </tr>
    <tr>
      <td><strong>C6 firmware update</strong></td>
      <td>From the SD card. Recovery still needs the UART header. <a href="c6_firmware/README.md">Details</a></td>
    </tr>
    <tr>
      <td><strong>Wi-Fi terminal</strong></td>
      <td>Telnet on 2323 with a password. SSH on 22 on ESP32-P4-NANO builds. USB is the tested link</td>
    </tr>
    <tr>
      <td><strong>LoRa bearing plots</strong></td>
      <td>RSSI by compass heading. Not validated direction finding</td>
    </tr>
    <tr>
      <td><strong>Location scanning</strong></td>
      <td>Mixed P25/FM lists, GPS filtering and CSV/JSON import. <a href="docs/LOCATION_SCAN.md">Details</a></td>
    </tr>
    <tr>
      <td><strong>RadioReference adapter</strong></td>
      <td>Needs an approved API key and Premium account. Live sign-in unverified</td>
    </tr>
  </tbody>
</table>

<p align="center">
  <img src="assets/lakeshark-divider.png" alt="" width="900">
</p>

## Build from source

ESP-IDF **5.5.4** with the six fixes in `bench/patches/`, applied once:

```powershell
pwsh -File bench/patch-idf.ps1 -IdfPath /path/to/esp-idf -Apply
```

One build directory per target:

```sh
# T-Display P4 AMOLED (568 × 1232, 16 MB)
idf.py -B build_tdp4 -D SDKCONFIG=build_tdp4/sdkconfig -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;boards/t_display_p4.defaults" build

# T-Display P4 TFT (HI8561, 540 × 1168)
idf.py -B build_tdp4_tft -D SDKCONFIG=build_tdp4_tft/sdkconfig -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;boards/t_display_p4.defaults;boards/t_display_p4_tft.defaults" build

# LakeShark LR: LR2021 only, no USB host, RTL-SDR, HackRF or SX1262
idf.py -B build_tdp4_lr -D SDKCONFIG=build_tdp4_lr/sdkconfig -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;boards/t_display_p4.defaults;boards/t_display_p4_lr2021.defaults" build

# Waveshare P4 headless
idf.py -B build_nano -D SDKCONFIG=build_nano/sdkconfig -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;boards/nano_headless_ble.defaults" build
```

`boards/` holds the target defaults and partition layouts; `main/idf_component.yml` pins dependencies. Host checks: `pwsh -File bench/verify.ps1 -Level host`

<p align="center">
  <img src="assets/lakeshark-divider.png" alt="" width="900">
</p>

## Contributors

<table align="center">
  <tbody>
    <tr>
      <td><a href="https://github.com/zenobik">@zenobik</a></td>
      <td>T-Display P4 TFT support (<a href="https://github.com/SAMS0N1TE/LakeShark/pull/8">#8</a>), UTF-8 text shown as ASCII letters (<a href="https://github.com/SAMS0N1TE/LakeShark/pull/9">#9</a>), LR2021 frequency correction for modules without a TCXO (<a href="https://github.com/SAMS0N1TE/LakeShark/pull/19">#19</a>)</td>
    </tr>
  </tbody>
</table>

## Credits and open-source projects

<table align="center">
  <thead>
    <tr>
      <th>Project</th>
      <th>Contribution</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td><a href="https://github.com/beala/lr2021-re">lr2021-re</a></td>
      <td>Alex Beal. Patch RAM programs for the LR2021's internal CPU that LakeShark runs on the radio: the on-chip FFT waterfall, the band sweep and the LoRa scanner. Our on-chip down-conversion is built with his ARC toolchain and loader. <a href="https://beala.substack.com/p/your-node-doesnt-need-a-microcontroller">Write-up</a>. MIT</td>
    </tr>
    <tr>
      <td><a href="https://osmocom.org/projects/rtl-sdr">rtl-sdr / librtlsdr</a></td>
      <td>Osmocom. RTL-SDR driver foundation. GPL-2.0+</td>
    </tr>
    <tr>
      <td><a href="https://github.com/XTR1984/xtrsdr">xtrsdr</a></td>
      <td>XTR1984. ESP32-optimized librtlsdr implementation, essential to this project</td>
    </tr>
    <tr>
      <td><a href="https://github.com/boatbod/op25">OP25</a></td>
      <td>Pavel Yazev's fixed-point <code>imbe_vocoder</code>. Experimental Phase II work by Max H. Parke, Graham J. Norbury and contributors. GPL-3.0+</td>
    </tr>
    <tr>
      <td><a href="https://github.com/watson/libmodes">libmodes</a> / <a href="https://github.com/antirez/dump1090">dump1090</a></td>
      <td>Thomas Watson and Salvatore Sanfilippo. Mode S decoder lineage. BSD-2-Clause</td>
    </tr>
    <tr>
      <td><a href="https://github.com/jstockdale/T-Display-P4/tree/adsb">ADS-B Scope</a></td>
      <td>John Stockdale and Off by One, Inc. Original implementation and single-frame CPR reference</td>
    </tr>
    <tr>
      <td><a href="https://github.com/szechyjs/dsd">DSD</a> / <a href="https://github.com/lwvmobile/dsd-fme">dsd-fme</a></td>
      <td>szechyjs and lwvmobile. P25 symbol and framing foundations. ISC-style notices</td>
    </tr>
    <tr>
      <td><a href="https://github.com/szechyjs/mbelib">mbelib</a></td>
      <td>szechyjs. Fallback vocoder. ISC</td>
    </tr>
    <tr>
      <td><a href="https://github.com/espressif/esp-idf">ESP-IDF</a></td>
      <td>Espressif. Embedded development framework</td>
    </tr>
    <tr>
      <td><a href="https://github.com/Xinyuan-LilyGO/lilygo_device_driver">LILYGO device drivers</a></td>
      <td>T-Display P4 and MIX-RF hardware drivers, wiring and power examples</td>
    </tr>
    <tr>
      <td><a href="https://github.com/meshcore-dev/MeshCore">MeshCore</a></td>
      <td>Scott Powell / Ripple Radios and contributors. Mesh radio stack</td>
    </tr>
    <tr>
      <td><a href="https://github.com/SAMS0N1TE/CartoTUI">CartoTUI</a> / <a href="https://github.com/SAMS0N1TE/ZeroMesh">ZeroMesh</a></td>
      <td>libcarto rendering and embedded PMTiles reader</td>
    </tr>
    <tr>
      <td><a href="https://github.com/protomaps/PMTiles">PMTiles</a></td>
      <td>Protomaps. Seekable, single-file offline map storage</td>
    </tr>
    <tr>
      <td><a href="https://www.openstreetmap.org/copyright">OpenStreetMap</a></td>
      <td>Open map data. ODbL</td>
    </tr>
    <tr>
      <td><a href="https://github.com/flipperdevices/flipperzero-firmware">Flipper Zero firmware</a></td>
      <td>Flipper Devices and contributors. Control-head platform and Crypto1 helpers</td>
    </tr>
    <tr>
      <td><a href="https://dejavu-fonts.github.io/">DejaVu</a></td>
      <td>Interface fonts</td>
    </tr>
    <tr>
      <td><a href="https://github.com/waveshareteam">Waveshare</a></td>
      <td>ESP32-P4 board support and hardware examples</td>
    </tr>
  </tbody>
</table>

The field-map design drew on [Cardputer offgrid map](https://github.com/yuiseki/m5-cardputer-offgrid-tiny-map) by yuiseki and [HikePod](https://github.com/nongxl/HikePod) by nongxl. No source code was copied from either.

<p align="center">
  <img src="assets/lakeshark-divider.png" alt="" width="900">
</p>

<p align="center"><strong>Open source. Built for experimentation.</strong></p>

<p align="center">
  <a href="https://terminalbay.com/?m=lakeshark">Install</a>
  &nbsp;·&nbsp;
  <a href="https://terminalbay.com/?m=wiki">Documentation</a>
  &nbsp;·&nbsp;
  <a href="https://github.com/SAMS0N1TE/LakeShark/releases">Releases</a>
  &nbsp;·&nbsp;
  <a href="https://terminalbay.com/?m=lakeshark-roadmap">Roadmap</a>
  &nbsp;·&nbsp;
  <a href="https://terminalbay.com/?m=lakeshark-roadmap#ask">Suggest a feature</a>
</p>

<p align="center">Contact: <a href="mailto:support@terminalbay.com">support@terminalbay.com</a></p>

<p align="center">
  LakeShark is licensed under <a href="LICENSE">GPL-3.0</a>.
  Third-party components retain their respective licenses.
  <a href="docs/THIRD_PARTY_NOTICES.md">Third-party license inventory</a>.
</p>

<p align="center">
  AI-assisted development and source-derived reimplementations:
  <a href="AI_USE.md">AI use and attribution policy</a>.
</p>

<p align="center"><sub>Radio reception and transmission are subject to local laws and regulations.</sub></p>
