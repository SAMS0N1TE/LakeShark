"""Compile DETECTOR/range production sources with pinned IDF headers; no link/flash.
Usage: python bench/tools/check_detector_range_compile.py build_tdp4_lr
Requires an existing IDF compile_commands.json. Exercises extended and legacy BLE.
"""
import json
import re
import subprocess
import sys
from pathlib import Path

root = Path(__file__).resolve().parents[2]
build = root / (sys.argv[1] if len(sys.argv) > 1 else "build_tdp4_lr")
output = build / "detector-range-compile"
output.mkdir(exist_ok=True)
entries = json.loads((build / "compile_commands.json").read_text())
names = ["ls_detector.c", "ls_detector_sinks.c", "ls_detector_ui.c", "ls_sweep.c",
         "ls_rid.c", "ls_lora.c", "ls_lora_sx126x.c", "ls_lora_lr20xx.c",
         "ble_link.c", "ble_link_observer.c", "headless_main.c", "ls_dronelink.c",
         "ble_link_legacy.c"]
failures = []
for name in names:
    template = {"ls_dronelink.c": "ls_detector.c", "ble_link_legacy.c": "ble_link.c"}.get(name, name)
    matches = [e for e in entries if Path(e["file"]).name == template]
    if name == "ls_sweep.c":
        matches = [e for e in matches if "apps/tui" in e["file"].replace(chr(92), "/")]
    entry = matches[0]
    variants = ["extended", "legacy"] if name == "ble_link.c" else ["extended"]
    for variant in variants:
        command = entry["command"]
        if name != template:
            command = command.replace(entry["file"], Path(entry["file"]).with_name(name).as_posix())
        artifact = output / (name + "." + variant)
        command = re.sub(r" -o [^ ]+", ' -o "' + artifact.as_posix() + '.obj"', command)
        config = output / (variant + "_config.h")
        config.write_text('#include "sdkconfig.h"\n#undef CONFIG_BT_NIMBLE_EXT_ADV\n#define CONFIG_BT_NIMBLE_EXT_ADV ' + ("1" if variant == "extended" else "0") + "\n")
        command += ' -include "' + config.as_posix() + '"' 
        result = subprocess.run(command, cwd=entry["directory"], capture_output=True, text=True)
        artifact.with_suffix(artifact.suffix + ".log").write_text(result.stdout + result.stderr)
        print(name, variant, "PASS" if result.returncode == 0 else "FAIL", flush=True)
        if result.returncode:
            failures.append(name + "/" + variant)
            print(result.stderr)
if failures:
    raise SystemExit("Compilation failed: " + ", ".join(failures))
