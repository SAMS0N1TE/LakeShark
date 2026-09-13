# Experimental P25 Phase II decoder

Selected OP25 files from [boatbod/op25](https://github.com/boatbod/op25/tree/71abcd0ead32f86f51615ea6cc8a6a4dba4c949a), commit
`71abcd0ead32f86f51615ea6cc8a6a4dba4c949a`, under `op25/gr-op25_repeater/lib`.
The adapter also derives its burst, ACCH and ESS handling from `p25p2_tdma.cc`
and its scrambler initialization from `op25/gr-op25_repeater/apps/tdma/lfsr.py`.
OP25 credits Max H. Parke and Graham J. Norbury; individual files retain their
original author and license notices, including mbelib's ISC terms and ezpwd's
GPL terms. GPL v3 is included in `COPYING`.

Local changes to the vendored files:

- Prefix mbelib identifiers, its `Ws` table and Golay decode entry points to coexist with Phase I.
- Replace the ISCH map with an immutable lookup table containing the same entries.
- Add missing standalone header includes and the AMBE function declaration header.
- Use `uint32_t` IMBE frame locals for the ESP32-P4 ABI.
- Use decoder-local pseudorandom noise for audio synthesis.
- Bound synthesis phase and interpolate a cosine table; checked against libm
  across the working phase range (maximum absolute error below 0.00007).
- Use single-precision constants and math in the float vocoder on the P4.

The C interface owns one slot, with resettable protocol and voice state. It
requires CRC/FEC-validated clear-call metadata before audio. No decryption is
implemented. Recovered codewords can be logged in host builds with
`LS_P25P2_TRACE`; normal firmware does not log them.

This is a limited manual voice path, not a complete OP25 receiver. Automatic
Phase II trunk following, all ISCH variants, two-slot audio mixing and RF
qualification are not included. See [testing and controls](../../docs/P25_PHASE2.md).
