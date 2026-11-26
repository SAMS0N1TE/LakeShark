# Native ESP32-P4 game SDK

These components run in ESP-IDF firmware. The `game-firmware` example in the LakeShark source checkout shows how to set up the board and measure the frame loop. Loadable LakeShark apps use the palette-based renderer in [lap3d.h](../include/lap3d.h).

## RGB565 3D

Add `app-sdk/native/e3d` to `EXTRA_COMPONENT_DIRS` and add `e3d` to your component's `PRIV_REQUIRES`. See [e3d.h](e3d/include/e3d.h) for the API.

1. Create a scene with `e3d_create(width, height, 2)` on a dual-core P4.
2. Finish your meshes and share them across objects. Keep them alive through the end of each frame.
3. Call `e3d_begin` with an RGB565 target and its pixel stride, submit objects with `e3d_draw`, and call `e3d_end`.
4. Draw overlays, then send the buffer to your display pipeline.

The game renders at 400 × 240 and uses the P4 PPA to rotate and scale the frame into a separate scanout buffer. Reuse a scanout buffer only after it is released at refresh. `e3d_get_stats` reports geometry and raster time in microseconds.

For indexed textures, attach UVs in texels and an `e3d_texture` array to the mesh. Set a triangle's 1-based texture index and use its red channel as the brightness multiplier. The rasterizer corrects perspective at each eight-pixel span, interpolates between endpoints, and wraps each sample to the texture size. Set `masked` when palette index 255 represents a cutout. The mesh borrows the UVs, texture data, and palettes, which must stay alive through the end of the frame.

Set `e3d_mesh.lightmaps` to supply an optional `e3d_lightmap` pointer per triangle. Lightmaps are tightly packed RGB888 samples on a 16-texel grid, with `min_u` and `min_v` locating the first sample in the mesh's texture coordinates. The renderer bilinearly samples the grid and multiplies it into the base texture. Keep the lightmap descriptors and pixels alive through the end of the frame.

For a static opaque material, `e3d_lightmap_bake` fills a caller-owned RGB565 array with the same lit texels. `e3d_lightmap_texel_count` gives the capacity to allocate. Keep the cache alive with the corresponding texture and lightmap. The rasterizer samples the original lighting outside the cached texture bounds and on masked materials.

This component comes from [giltal/ESP32_P4_3D_Engine](https://github.com/giltal/ESP32_P4_3D_Engine/tree/c71786e47fa86180194a3725744d822a65fcb2c0/e3d) at `c71786e`. It uses two CPU cores, internal SRAM bands, P4 PIE lighting and RGB565 output. The [MIT license](e3d/LICENSE) covers the vendored component.

## BSP30 scenes

Add `app-sdk/native/bsp30` and `app-sdk/native/e3d` to `EXTRA_COMPONENT_DIRS` and add `bsp30` to your component's `PRIV_REQUIRES`. Pass the map bytes to `bsp30_open` and keep them alive until `bsp30_close`. For each camera position, call `bsp30_prepare_world` with the eye in BSP coordinates. Between `e3d_begin` and `e3d_end`, call `bsp30_draw_world` to submit geometry selected by BSP leaf visibility. Convert the camera to `e3d` coordinates as `(x, z, -y) * BSP30_E3D_SCALE`.

The loader uses embedded indexed textures and the first RGB lightmap style on each face. `bsp30_get_stats` reports face counts and the current leaf. See [bsp30.h](bsp30/include/bsp30.h) for the brush mesh and entity data APIs.

After preparing a leaf, call `bsp30_cache_visible_lightmaps` between frames to populate a PSRAM cache up to your byte budget. It can serve subsequent frames from the same area. When changing areas, call it during a loading interval to replace the cache, or pass a zero budget to release it.

## Analog joystick and buttons

Add `app-sdk/native/tk21` to `EXTRA_COMPONENT_DIRS` and add `tk21` to `PRIV_REQUIRES`. Call `tk21_init` with three ADC-capable GPIOs and the KEY GPIO while the stick is centered. Use the axis inversion flags for the enclosure. `tk21_poll` provides normalized axes, held buttons and rising edges. The component owns the ADC oneshot units it uses.

The TK21 has a resistor ladder for six buttons and a separate active-high click. The ladder distinguishes one button at a time. KEY uses the board's own pull-down. [Lonely Binary hardware guide](https://learn.lonelybinary.com/tinkerblock/blocks/slim-joystick/the-six-pins).

After `tk21_poll`, call `tk21_read_raw` for the filtered physical X/Y counts, the boot center and the ladder voltage in millivolts. The call reuses the current sample. Use it for calibration screens and diagnostics. LakeShark Game shows how to save center and range, apply the dead zone and inversion, and remap actions.

## Waveshare 4.3-inch microSD

Add `app-sdk/native/waveshare_p4_sd` to `EXTRA_COMPONENT_DIRS` and add `waveshare_p4_sd` to `PRIV_REQUIRES`. Call `waveshare_p4_sd_mount("/sd", 5)` before opening files through the normal ESP-IDF VFS. The driver powers the onboard slot through LDO4 and uses the four-bit SDMMC bus. It keeps the card mounted for the app's lifetime and never formats it on mount failure.

For exFAT with ESP-IDF 5.5, enable `CONFIG_FATFS_LFN_HEAP` in `sdkconfig.defaults` and include [fatfs-exfat.cmake](waveshare_p4_sd/fatfs-exfat.cmake) after `project()`. The build selects the same FatFs settings for the library and its callers.

## Waveshare 4.3-inch speaker

Add `app-sdk/native/waveshare_p4_audio` to `EXTRA_COMPONENT_DIRS` and add `waveshare_p4_audio` to `PRIV_REQUIRES`. Open the speaker at the application's sample rate and send signed 16-bit stereo PCM with `waveshare_p4_audio_write`. The write reports the number of frames accepted and takes a timeout in milliseconds. Call it from an audio task if rendering needs to proceed independently. Close the handle after the writer has stopped.

The driver configures the I2S transmitter and ES8311 playback codec. It can reuse the board I2C bus when a touch driver has already created it. Use `waveshare_p4_audio_set_volume` to set the speaker level from 0 to 100 on an open handle. See [waveshare_p4_audio.h](waveshare_p4_audio/include/waveshare_p4_audio.h) and the [Waveshare board pinout](https://components.espressif.com/components/waveshare/esp32_p4_wifi6_touch_lcd_4_3/versions/1.0.1/readme?language=en).

## Waveshare 4.3-inch touch input

Add `app-sdk/native/waveshare_p4_touch` to `EXTRA_COMPONENT_DIRS` and add `waveshare_p4_touch` to `PRIV_REQUIRES`. Initialize it before opening the speaker so both drivers share the board I2C bus. Poll `waveshare_p4_touch_read` for native portrait coordinates, a contact ID, and release reports. Keep your current contact when the driver returns `ESP_ERR_NOT_FOUND`. Invert the scanout rotation when converting touch coordinates to game coordinates. [Touch API](waveshare_p4_touch/include/waveshare_p4_touch.h)
