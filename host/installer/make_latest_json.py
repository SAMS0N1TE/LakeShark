"""Generate the owner's latest.json from a built installer; never uploads it."""
import argparse
import hashlib
import json
from pathlib import Path
import re

REPO = Path(__file__).resolve().parents[2]


def make_latest(installer, output, notes="", channel=None):
    version = re.search(r'^#define AppVersion "([^"]+)"',
                        (REPO / "host/installer/lakeshark.iss").read_text("utf-8"), re.M)[1]
    installer, output = Path(installer), Path(output)
    if installer.resolve() == output.resolve():
        raise ValueError("output must not overwrite the installer")
    expected = "LakeShark-Setup-" + version + ".exe"
    if installer.name != expected:
        raise ValueError("installer must be named " + expected)
    with installer.open("rb") as f:
        digest = hashlib.file_digest(f, "sha256").hexdigest()
    data = {"version": version, "channel": channel or ("pre-release" if "-" in version else "stable"),
            "url": "https://github.com/SAMS0N1TE/LakeShark/releases/download/v" + version + "/" + expected,
            "sha256": digest, "size": installer.stat().st_size, "notes": " ".join(notes.split())}
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
    return data


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("installer", type=Path)
    ap.add_argument("output", type=Path)
    ap.add_argument("--notes", default="")
    ap.add_argument("--channel", choices=("stable", "pre-release"))
    a = ap.parse_args()
    print(json.dumps(make_latest(a.installer, a.output, a.notes, a.channel), indent=2))


if __name__ == "__main__":
    main()
