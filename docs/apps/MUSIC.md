# MUSIC

Plays WAV tracks from the SD card, shows the sound as meters and spectra, and records from the microphone.

| | |
|---|---|
| HOME group | RADIO |
| Radio | None. Plays WAV files from the SD card |

<img src="../screenshots/tdp4/motion/music-scan-waterfall.gif" width="300" alt="MUSIC playing a track with the SCAN view">

## What it does

- Plays 16-bit PCM WAV files through the board audio output.
- Shows what is playing in one of four views: LEVEL, BANDS, SCAN and WATERFALL.
- Listens to the board microphone live, or records it to a new WAV file on the card.
- MUSIC writes to the card only when you record.

## Where the files go

- Put tracks in a folder named `music` at the top of the SD card: `/sdcard/music`.
- MUSIC lists the files in that folder only. Subfolders are not searched.
- Only files ending in `.wav` (any case) are listed.
- A file named `Artist - Song.wav` shows the artist and the song on separate lines.
- After copying new files, press RESCAN (R) to read the folder again.

Supported WAV files:

- PCM, 16-bit, mono or stereo.
- Sample rate from 8 kHz to 48 kHz.

Any other WAV (8-bit, 24-bit, float, compressed) shows "Unsupported or damaged audio file". Convert it to 16-bit PCM first.

SOURCE switches the list between the SD card (`SD / MUSIC`) and the internal flash (`FLASH / MUSIC`). The firmware ships no tracks in flash, so the SD card is the normal source.

## Radio

MUSIC uses no radio. Opening MUSIC parks the radio receiver so the audio path is free for playback.

## How to use it

Open **HOME > RADIO > MUSIC**. The player opens with the tracks from the card loaded. Press TRACKS (L) to see the list, or PLAY to start the first track.

### Player keys

| Key | Does |
|---|---|
| Space or P | PLAY / PAUSE. Plays the selected track if nothing is playing |
| B / N, or Left / Right | PREV / NEXT track (wraps around the list) |
| [ / ] | Jump back / forward 5 seconds |
| - / + (or =) | VOL - / VOL + in 5% steps |
| M | MUTE / UNMUTE |
| S | STOP playback and the microphone |
| L | TRACKS (the track list) |
| 1, 2, 3, 4 | LEVEL, BANDS, SCAN, WATERFALL |
| V | Next view |
| C | Next colour palette |
| Z | SPAN (waterfall time per column) |
| E | TONE: WARM or FLAT |
| D | SOURCE: SD or FLASH |
| R | RESCAN the music folder |
| I | MIC: listen live |
| W | REC: record the microphone |
| O | OPTIONS |
| Tab, then Enter | Move between buttons and press the highlighted one |
| Esc | Close the track list, then leave MUSIC |

With a keyboard fitted, each button shows its key in brackets.

### Touch

- Every control is a button: PREV, PLAY / PAUSE, NEXT, VOL -, MUTE, VOL +, MIC, REC, TRACKS and OPTIONS.
- Tap the view tabs (LEVEL, BANDS, SCAN, WATERFALL) to change the view.
- Tap the progress bar to jump to that point in the track.
- On SCAN and WATERFALL, tap the colour button to change the palette. WATERFALL also has a SPAN button.

### Track list (TRACKS)

- Up / Down select a track. Enter or a tap plays it.
- U / J, or PREVIOUS PAGE / NEXT PAGE, move a page at a time.
- TONE, SOURCE and RESCAN buttons sit at the bottom.
- BACK or Esc returns to the player.

### Microphone and recording

- MIC (I) shows the live microphone in the views. Nothing is saved. The screen shows LISTENING.
- REC (W) records the microphone. The screen shows RECORDING and the time recorded.
- Press REC again to stop. The new file is selected in the list, so PLAY replays it.
- Recordings are mono, 16-bit, 44.1 kHz WAV files named `REC-001.wav`, `REC-002.wav` and so on, in `/sdcard/music`.
- Starting the microphone stops the track that is playing.

### OPTIONS

O or the OPTIONS button opens the PLAYER list. BACK goes up one level.

- **VIEW**: LEVEL, BANDS, SCAN or WATERFALL.
- **COLOUR**: RAINBOW, FIRE, NEON, ICE or PHOSPHOR.
- **SPAN**: how much time each waterfall column covers. At 44.1 kHz the three steps are about 46, 92 and 185 ms per column.
- **TONE**: WARM adds a gentle treble roll-off. FLAT plays the file as it is. WARM never boosts the level.
- **SOURCE**: SD or FLASH.
- **STOP**: stops playback and the microphone.
- **REPAIR WAV**: fixes the header of a recording that could not be finished, for example after a card write error.

## What the screen shows

- **Status**: READY, PLAYING, PAUSED, LISTENING or RECORDING.
- **Track**: artist and song (or "SD MUSIC" and the file name), elapsed and total time, a progress bar and TRACK n/N.
- **Format**: sample rate, bit depth and STEREO or MONO, plus the output mode and TONE.
- **Volume**: VOLUME as a percentage and in dB, with a 20-segment bar.

The four views:

- **LEVEL**: left and right peak meters in dBFS, from -40 to 0, with PEAK HOLD, a balance indicator and an over lamp. It reads the file's own channels before mixing and volume.
- **BANDS**: a 2/3-octave spectrum from 63 Hz to 16 kHz.
- **SCAN**: many thin bars on a log scale from 40 Hz up to 20 kHz, -84 to -12 dB, with falling peak dots.
- **WATERFALL**: a scrolling spectrogram from 0 Hz to half the sample rate, -100 to -20 dB. Pause the track to hold the picture.

The bottom line of each view says what it reads: the output mix ("L+R AFTER TONE") or the MICROPHONE.

Landscape puts the views on the left and the track, buttons and volume on the right. Portrait stacks the track at the top, the view in the middle and the transport at the bottom.

## Tips and limits

- The board output is mono. Stereo tracks are mixed to mono for the speaker, and the LEVEL meters still show both channels.
- Leaving MUSIC stops playback.
- The list follows the order of the files in the folder. It is not sorted.
- If the card is missing or has no `music` folder, the player shows "Music folder missing / unreadable". Add the folder and press RESCAN.
- "Save failed ... OPTIONS > REPAIR WAV" means a recording was cut short. REPAIR WAV makes it playable.

## More guides

- [Controls and navigation](../UI_CONTROLS.md)
- [T-Display-P4 screens](../TDP4_SCREENS.md)
- [All apps](README.md)
