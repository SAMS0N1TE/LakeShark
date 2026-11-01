"""A subprocess with lssim's batch arguments and BMP output contract."""
import os
from pathlib import Path
import sys
from PIL import Image

args = sys.argv[1:]
assert args[0] == "lap" and args[args.index("-f") + 1].isdigit()
assert os.environ["LAPSIM"] == "demo"
assert "LSSIM_FRAMES" not in os.environ and "LSSIM_FEED_OUT" not in os.environ
script = os.environ.get("LAPSIM_SCRIPT", "")
if "rx=99999" in script:
    print("stub runtime failure", file=sys.stderr)
    raise SystemExit(1)
Image.new("RGB", (16, 12) if "-l" in args else (12, 16), "green" if script else "navy").save(Path(args[args.index("-o") + 1]))
print("stub lssim: " + script)
