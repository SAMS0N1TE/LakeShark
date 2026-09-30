# LakeShark audio player

Based on Chris Morgan's esp-audio-player 1.0.7:
https://github.com/chmorgan/esp-audio-player

LakeShark modifications, September 2026:

- WAV playback configuration.
- MP3 decoder source and dependency excluded.
- PCM buffer capacity defined independently of MP3 decoder headers.
- WAV chunks bounded by RIFF/file lengths, odd padding handled, format restricted
  to 16-bit PCM mono/stereo at 8-48 kHz; decoding stops at the data chunk boundary.
- Decoder deletion retains live-task buffers when shutdown times out.

The upstream copyright notices and Apache-2.0 license are retained.
