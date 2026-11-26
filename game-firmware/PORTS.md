# ESP32-P4 game and emulator ports

| Project | What applies to LakeShark |
| --- | --- |
| [giltal's ESP32-P4 3D Engine](https://github.com/giltal/ESP32_P4_3D_Engine/tree/c71786e47fa86180194a3725744d822a65fcb2c0) | The native SDK uses this RGB565 renderer. Its SRAM bands, PIE lighting and two-core geometry and raster paths suit games that redraw each frame. LakeShark's palette renderer now transforms the shared corners of generated boxes once before triangle submission. |
| [gpSP ESP32](https://github.com/midworld/gpsp-esp32/tree/0015ff0eea6f8d21973beb7776682277e9ecefe5) | This supplies the GBA core and open BIOS. The v1.3 P4 trapped on Zba `sh3add`. The game build excludes those bit-manipulation instructions from both GCC and the RV32 JIT on pre-v3 parts. |
| [ESP32-P4 Launcher](https://github.com/giltal/ESP32_P4_Launcher) | Its relocatable PAPP interface separates the host's display, sound, input and threads from individual ports. Its existing Genesis, SNES and Neo Geo ports give concrete baselines for larger emulators. PAPP binaries have their own ABI. |
| [Basilisk II for M5Stack Tab5](https://github.com/amcchord/M5Tab-Macintosh) | An ESP32-P4 68040 emulator that boots classic Mac OS. Its hot interpreter handlers run from internal memory and it tracks dirty video spans for much smaller scanout updates. |
| [PCSX-ReARMed](https://github.com/notaz/pcsx_rearmed/blob/master/Makefile) | A PlayStation candidate for a separate experiment. Its ARM dynarec and the alternative Lightrec path need different porting work. The first P4 benchmark would compare the interpreter with an RV32-capable recompilation path and measure the software GPU separately. |
| [Espressif's Quake](https://github.com/espressif/esp32-quake) and [ESP32 Quake II](https://github.com/alexkid77/ESP32_QUAKE2) | Working BSP-based shooters on P4. Quake II's software renderer and PPA presentation are useful examples for a larger native port. |
| [Duke Nukem 3D on ESP32-P4](https://github.com/raoulh/esp_duke/blob/master/docs/02-architecture.md) | The port hands off a palette framebuffer by index, converts it on the presentation core, and rotates it through the PPA. It also documents an ES8311/I2S sound path on Waveshare hardware. These are concrete references for a port's video and audio paths. |

## Hardware references and licenses

- The ST7701 register table comes from the [Waveshare 4.3-inch BSP](https://github.com/waveshareteam/Waveshare-ESP32-components/blob/23a6012d5e87d27399cf90fb851cab3689e29159/bsp/esp32_p4_wifi6_touch_lcd_4_3/esp32_p4_wifi6_touch_lcd_4_3.c) under [Apache-2.0](LICENSE.waveshare). The game driver uses the ESP-IDF LCD APIs directly.
- The included e3d component is [MIT-licensed](../app-sdk/native/e3d/LICENSE).
- The optional upstream [gpSP source](https://github.com/midworld/gpsp-esp32/tree/0015ff0eea6f8d21973beb7776682277e9ecefe5) includes the core and BIOS license notices. Signal Catch is an original test cartridge with source in this project.
