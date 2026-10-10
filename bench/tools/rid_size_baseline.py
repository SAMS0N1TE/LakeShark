"""Measure HEAD baseline at identical build paths, always restore feature files.

Run only after the feature build has stopped. Uses no Git writes or hardware.
"""
from pathlib import Path
import json
import shutil
import subprocess

root = Path(__file__).resolve().parents[2]
git = ["git", "-c", "safe.directory=" + root.as_posix()]
out = root / "bench/out/rid/baseline"
out.mkdir(parents=True, exist_ok=True)
tracked = ["main/ble_link.c", "main/compact_ui.cpp", "main/headless_main.c",
           "components/apps/tui/screens/ext/scr_wireless.c"]
added = ["components/apps/tui/ls_rid.c", "components/apps/tui/ls_rid_ui.c"]
paths = [root / name for name in tracked + added]
for p in paths:
    assert p.resolve().is_relative_to(root)
saved = {p: p.read_bytes() for p in paths}
# Keep recovery files as well as the in-memory snapshot.
for p, content in saved.items():
    (out / (p.name + ".feature")).write_bytes(content)
head = {root / name: subprocess.check_output(git + ["show", "HEAD:" + name], cwd=root)
        for name in tracked}
try:
    for p, content in head.items():
        p.write_bytes(content)
    for name in added:
        (root / name).write_text("/* Baseline measurement: feature excluded. */\n")
    with (out / "build.log").open("w") as log:
        result = subprocess.run(["pwsh", "-NoProfile", "-File", "bench/build.ps1",
                                 "t-display-p4"], cwd=root, stdout=log, stderr=subprocess.STDOUT)
    if result.returncode:
        raise RuntimeError("Baseline build failed; inspect baseline/build.log")
    for name in ["lakeshark.map", "lakeshark.elf", "lakeshark.bin"]:
        shutil.copyfile(root / "build_tdp4" / name, out / name)
    (out / "manifest.json").write_text(json.dumps({
        "head": subprocess.check_output(git + ["rev-parse", "HEAD"], cwd=root).decode().strip(),
        "reverted": tracked, "excluded": added,
        "bin_bytes": (out / "lakeshark.bin").stat().st_size,
    }, indent=2))
finally:
    for p, content in saved.items():
        p.write_bytes(content)
print("Baseline saved and feature sources restored.")
