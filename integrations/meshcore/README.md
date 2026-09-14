# T-Beam Supreme CELL WATCH receiver

The adjacent patch preserves the companion receiver changes used with this P4
build. Apply it to [MeshCore](https://github.com/meshcore-dev/MeshCore) commit
`d92964352441e53b93e8667b802e04f6e072b39e` with `git apply`, then build the
PlatformIO environment `LakeShark_TBeam_Supreme_receiver`.

Target hardware: LilyGO T-Beam S3 Supreme with SX1262. The environment enables
the USB companion protocol, disables advertisements on boot, and sets the
default frequency to 910.525 MHz. The tested runtime settings are 910.525 MHz,
SF7, 62.5 kHz and coding rate 4/5; configure them to match the P4 peer before
using the addressed reporting link. Preserve the upstream
project's license and notices.

The integrated ground station consumes addressed CW1 messages through this
receiver's USB interface. P4 normal-mode survey/synchronization reports are
wired; the focused HackRF capture screen currently saves its detailed MIB and
GPS/IMU evidence on SD and does not automatically forward that capture summary.
See `docs/CELL_WATCH.md` for the tested scope and build evidence.
