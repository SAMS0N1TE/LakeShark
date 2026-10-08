# Updates and calls

LakeShark 2.9.1 on the LilyGO T-Display-P4.

## UPDATE

From 2.8 or earlier, use the web flasher or USB once to install 2.9.1. This
installs the two app slots needed for updates over WiFi. A full install rewrites
internal storage. Settings and saved Wi-Fi are kept. Back up files first.
See [First flash](TDP4_FIRST_FLASH.md).

1. Open LINK and connect to a saved or new Wi-Fi network.
2. Open HOME > SYSTEM > UPDATE. The page checks for a newer build.
3. Choose INSTALL when offered. CHECK asks again; RETRY repeats a failed step.
4. Keep power connected while the download, checks and install finish. The board
   restarts into the new build.

Builds come from ota.terminalbay.com. The board checks the signed update offer
and the downloaded image. An invalid download is not selected for boot. If a
new build fails to start, rollback returns to the previous build.

SET > DEVICE > Update opens the same page. Check for updates selects Daily or
Off. Daily is the default. A newer build adds a NEW tag to SYSTEM and UPDATE.
Off disables background checks; UPDATE > CHECK still works. An update over
WiFi replaces the app and keeps internal storage, settings and SD files.

## CALLS

Open CALLS in P25 or FM, or press 5. OPTIONS > CALLS > Recent calls opens the
same archive. The list shows the newest recordings first. Select a row, then
PLAY or Enter. STOP returns to receive. DELETE asks before removing its WAV
and metadata. BACK returns to the receiver.

An SD card is required. Clear P25 calls and FM audio that passes squelch can be
recorded while listening. Encrypted P25 audio is not archived. Playback pauses
radio reception until STOP. It uses the current volume and mute setting.

CALLS > OPTIONS offers:

- Record P25 and Record FM: both on by default.
- Minimum call (ms): 500 by default, adjustable from 0 to 10000.
- Keep newest days: 0 by default, meaning keep all. Set a limit from 1 to 365
  UTC days to remove older dated recordings.
- Only listed talkgroups: off by default. Talkgroup list accepts up to 32 P25
  talkgroup numbers separated by commas.

Set **Keep newest days** to limit card use before leaving recording enabled for
long runs. Calls can use substantial card space. Without a known date, age
cleanup cannot treat a recording as an old dated call.

Files go under `/sdcard/lakeshark/calls`, grouped by day, with WAV audio and
metadata beside it. P25 uses 8 kHz, 16-bit mono WAV, about 1 MB per minute.
FM uses 16 kHz, 16-bit mono WAV, about 2 MB per minute. Copy the files from the
card to keep them before age cleanup or deletion.

DMR in P25 shows digital call activity, slots, colour codes and receive filters.
Voice playback and DMR recording are unavailable. Encrypted activity is marked
ENCRYPTED / MUTED.
