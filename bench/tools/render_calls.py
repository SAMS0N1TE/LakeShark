"""Render the production CALLS overlay with host-only fixtures; BMP, PNG and text grids."""
import os
import pathlib
import subprocess
from PIL import Image

ROOT = pathlib.Path(__file__).resolve().parents[2]
OUT = ROOT / "bench/build"
SCENES = {
    "portrait": ["-k", "p"],
    "landscape": ["-l", "-k", "p"],
    "paused": ["-k", "pp"],
    "seek": ["-k", "pp]"],
    "guard": [],
    "options": ["-k", "o"],
    "delete": ["-k", "d"],
    "empty": ["-e"],
    "touch": ["-l", "-x", "25:6,30:15,+p,~10"],
}

def main():
    env = dict(os.environ)
    env["PATH"] = "C:/mingw64/bin;" + env.get("PATH", "")
    for scene, args in SCENES.items():
        scene_env = dict(env)
        if scene == "guard": scene_env["LSSIM_CALLS_LOW"] = "1"
        result = subprocess.run([str(OUT / "lssim.exe"), "calls", *args,
                                 "-A", "80", "-f", "30", "-d", "-o",
                                 str(OUT / f"calls-{scene}.bmp")],
                                cwd=ROOT, env=scene_env, text=True, capture_output=True, check=True)
        (OUT / f"calls-{scene}.txt").write_text(result.stdout)
        im = Image.open(OUT / f"calls-{scene}.bmp")
        if "-l" in args: im = im.transpose(Image.Transpose.ROTATE_270)
        im.save(OUT / f"calls-{scene}.png")
        print(OUT / f"calls-{scene}.png")

if __name__ == "__main__": main()
