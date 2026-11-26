"""Rebuild the original Signal Catch GBA smoke-test cartridge with Clang and LLD."""

from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent
FLAGS = ["--target=arm-none-eabi", "-mcpu=arm7tdmi", "-ffreestanding", "-fno-builtin", "-Os"]

with tempfile.TemporaryDirectory() as work:
    work = Path(work)
    for src in ("header.S", "signal_catch.c"):
        subprocess.run(["clang", *FLAGS, "-c", str(ROOT / src), "-o", str(work / (src + ".o"))], check=True)
    link = work / "rom.ld"
    link.write_text("SECTIONS { . = 0x08000000; .rom : { *(.header) *(.text*) *(.rodata*) } "
                    "/DISCARD/ : { *(.ARM.exidx*) *(.ARM.attributes*) } }", encoding="utf-8")
    rom_path = work / "rom.gba"
    subprocess.run(["lld", "-flavor", "gnu", "-m", "armelf", "-T", str(link), "--oformat=binary", "-o", str(rom_path),
                    str(work / "header.S.o"), str(work / "signal_catch.c.o")], check=True)
    rom = bytearray(rom_path.read_bytes())
    if len(rom) > 32768 or rom[0xB2] != 0x96:
        raise ValueError("Invalid cartridge header or size")
    rom[0xBD] = (-0x19 - sum(rom[0xA0:0xBD])) & 0xFF
    # The read-only embedded filesystem supplies the remaining cartridge padding.
    (ROOT / "signal_catch.gba").write_bytes(rom)
