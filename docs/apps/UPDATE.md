# UPDATE

Install new LakeShark builds over WiFi.

| | |
|---|---|
| HOME group | SYSTEM |
| Radio | Wi-Fi (the Wi-Fi and Bluetooth co-processor) |

<img src="../screenshots/tdp4/motion/update.gif" width="300" alt="UPDATE checking for a new build">

## What it does

UPDATE asks the LakeShark update server for a newer build. When one is
offered, INSTALL downloads it, checks that LakeShark signed it, writes it to
the other half of the flash and restarts into it. If the new build fails to
start, the board puts the previous build back.

An update over WiFi replaces the app only. Internal storage, settings and SD
card files are kept.

Opening the page checks for an update straight away, unless one is already
found or being installed.

## Radio

UPDATE uses Wi-Fi. Connect to a network in LINK first. While it works, the
page shows "joining WiFi", then "checking".

## How to use it

| Key | Touch | Action |
|---|---|---|
| Enter | The second button, or the large button | Do the step on the button |
| Esc | BACK | Back to HOME |

The second button names the one thing a press does now:

- **CHECK**: ask the server again.
- **INSTALL**: download and install the offered build. Its size is shown.
- **RETRY**: try again after a failed step.
- **RESTART**: restart into the new build now. It also restarts by itself
  after a short countdown.
- **WAIT**: a step is running, or a new build is proving itself. Nothing to
  press.

SET > DEVICE > Update opens the same page. SET > DEVICE > Check for updates
selects Daily or Off for background checks.

## What the screen shows

- A picture of the update server and this board, joined by a pipe that fills
  while the build downloads. The board shows the version it runs.
- The steps: CHECK, GET, VERIFY, INSTALL and BOOT, with the current one lit.
- During download: megabytes received, percent, speed and time left.
- **running**: the build on the board now.
- **server**: the build on offer, "not checked yet" or "nothing published yet".
- **last try**: shown when a new build did not start and the previous one came
  back.
- **trial**: after an update, a countdown while the new build proves itself,
  then "kept".
- A state line at the bottom, for example "is the latest" or "ready: tap
  INSTALL".

## Tips and limits

- Keep the board powered until it restarts into the new build.
- A board on 2.8 or earlier needs one install with the web flasher or USB
  before updates over WiFi work.
- Every download is checked before it is used. A build that does not pass is
  never started.

## More guides

- [Updates and calls](../TDP4_UPDATES_AND_CALLS.md)
- [LINK](LINK.md)
- [Controls and navigation](../UI_CONTROLS.md)
- [All apps](README.md)
