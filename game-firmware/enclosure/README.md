# LakeShark handheld enclosure

The landscape enclosure uses a raised octagonal display housing, recessed control decks, and tapered rear edges. The TK21s mount vertically in opposite orientations. The left stick sits above its buttons and the right stick sits below its buttons. The faceplate exposes the original controls through recessed wells.

The [render](handheld-preview.png) shows a charcoal paint finish, teal accents, and the tram frame captured from COM4.

| Part | Nominal envelope |
| --- | --- |
| Closed shell | 208 × 95 × 32.3 mm, plus the joystick caps |
| Waveshare glass | 114.4 × 66.8 mm, with a 94.4 × 56.96 mm active area |
| TK21 PCB | 62.4 × 30.4 mm each, with four Ø4.8 mm holes on a 56 × 24 mm grid |
| Measured MakerChoice pack envelope used in the CAD | 112 × 65 × 8.8 mm |

The Waveshare board drawing shows a 102.5 × 60 mm rear PCB. The TK21 drawing gives 15.15 mm from the bottom of its PCB to the top of the stick cap. [Waveshare dimension drawing](https://docs.waveshare.com/assets/images/ESP32-P4-WIFI6-Touch-LCD-4.3-2-fcd5fe9becbef3a55d4a7bd83ce205f3.webp) · [TK21 mechanical drawings](https://learn.lonelybinary.com/tinkerblock/blocks/slim-joystick#downloads)

## Adjust the fit

Open [handheld.scad](handheld.scad) in OpenSCAD. The supplied meshes use the measured pack dimensions. Adjust `pack_width`, `pack_height` and `pack_depth` for a different pack. Measure `display_stack` from the front of the glass to the deepest fitted connector or wire on the board. Check `glass_thickness` at the outer edge where the retaining pads contact. The source derives the shell depth from those dimensions. Set `part = "section"` to inspect the divider and battery clearances in the assembled section.

The STLs assume a 16 mm display-and-harness stack and a 3 mm retaining edge. Check those dimensions, the battery lead's exit, and how far the connector bends before committing to a full print. The 208 mm width fits a 220 mm bed with 6 mm per side for a brim.

## Print and assemble

1. Export `front`, `rear`, `lcd_clip` and `divider` by selecting the corresponding `part` value, rendering with F6 and exporting STL. The included [STLs](print) keep their print orientation. Use build-plate support under the recessed grip faces on the front part.
2. Fit the display into the bezel from the rear and check the active image and touch edges. Print two clips and mirror one along X in the slicer. Fit soft, nonconductive pads at the clip tips, then tighten only enough to retain the display at its outer edge. Confirm that the tips bear on clear frame or glass border areas and clear the circuit board and ribbon.
3. Fit each TK21 from the rear. Use short M2.5 screws and insulating washers that cover the Ø4.8 mm PCB holes. The standoffs accommodate the stick housing and connector pocket. Test full stick tilt, stick click, and all six buttons before closing the shell.
4. Seat the protected pack in the rear cradle. The default design provides 1.5 mm of clearance per side and 1.8 mm above the pack. Route the lead through the divider notch and secure the divider with four short M2.5 screws in the side rails. Check that the assembled parts leave the pouch and leads free of pressure.
5. Dry-fit the microSD and USB access before closing. The top opening serves the microSD and the board buttons. The rear side windows route low-profile USB-C leads. Board orientation and the plugs determine the final cable bend. The default depth uses eight M3 × 28 mm rear shell screws. Recheck screw length after changing the shell depth.

The rear shell has a pocket for the battery and a separate divider between the pouch and the electronics. Waveshare specifies a nominal 3.7 V single-cell battery for the 2-pin MX1.25 BAT header. Confirm the pack's cell configuration, protection, compatible charge specifications, and connector polarity before hooking it up. [Waveshare hardware and battery connector](https://docs.waveshare.com/ESP32-P4-WIFI6-Touch-LCD-4.3)

## Second TK21 wiring

The second module has three available analog pins and a separate digital stick-click input. Connect it with the board powered off. Use the silkscreen to confirm the header orientation before counting pins.

| Second TK21 lead | Waveshare 40-pin header | ESP32-P4 signal |
| --- | ---: | --- |
| X | 34 | GPIO50 / ADC2 channel 1 |
| Y | 36 | GPIO51 / ADC2 channel 2 |
| BTNS | 38 | GPIO52 / ADC2 channel 3 |
| KEY | 12 | GPIO3, active-high digital input |
| 3V3 | 2 | 3.3 V |
| GND | 33 | Ground |

The existing single-module driver must reuse the same ADC2 unit handle for GPIO49 to 52 when a second module is added. Each TK21 retains its own resistor ladder and can report one pressed ladder button at a time independently of the other module. [Waveshare header pinout](https://docs.waveshare.com/ESP32-P4-WIFI6-Touch-LCD-4.3#pinout-definition) · [Espressif ADC pin assignments](https://docs.espressif.com/projects/esp-hardware-design-guidelines/en/latest/esp32p4/schematic-checklist-esp32p4.html#adc) · [TK21 pin guide](https://learn.lonelybinary.com/tinkerblock/blocks/slim-joystick/the-six-pins)

For the vertical mounts, apply the opposite quarter-turns to the two sticks' X/Y input in firmware. In the right pod, the Xbox face positions correspond to U7 for A at the bottom, U5 for B on the right, U8 for X on the left, and U6 for Y at the top. The horizontal single-module profile remains the setup for an unrotated TK21. [TK21 control positions](https://learn.lonelybinary.com/tinkerblock/blocks/slim-joystick/what-is-where)
