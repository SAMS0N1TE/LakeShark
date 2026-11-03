"""Build the release desktop exe with its private buildenv, then Inno Setup 6."""
import argparse
import os
from pathlib import Path
import subprocess
import sys

REPO = Path(__file__).resolve().parents[2]
LOCAL = Path(os.environ.get("LOCALAPPDATA", Path.home() / "AppData" / "Local"))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--python", default=str(LOCAL / "LakeShark/buildenv/Scripts/python.exe"))
    ap.add_argument("--iscc", default=str(LOCAL / "Programs/Inno Setup 6/ISCC.exe"))
    a = ap.parse_args()
    subprocess.run([a.python, str(REPO / "host/lsterm/build_app.py"), "--release"], cwd=REPO, check=True)
    subprocess.run([a.iscc, str(REPO / "host/installer/lakeshark.iss")], cwd=REPO, check=True)
    installer = REPO / "build_installer/LakeShark-Setup-2.20.0.exe"
    print("Built %s (%d bytes)" % (installer, installer.stat().st_size))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, subprocess.CalledProcessError) as e:
        sys.exit("Installer build failed: %s" % e)
