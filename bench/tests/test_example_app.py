"""Exercise the actual example in lssim, including measured touch rectangles."""
import pathlib
import re
import subprocess
import sys

exe = pathlib.Path(sys.argv[1]).resolve()
out = pathlib.Path(sys.argv[2]).resolve()
out.mkdir(parents=True, exist_ok=True)
# Match the firmware component's include root, not lssim's wider include list.
root = pathlib.Path(__file__).resolve().parents[2]
cc, cxx, nm = sys.argv[3:6]
source = root / "components/apps/tui/apps/example_app.c"
for flag in ([], ["-DCONFIG_LS_EXAMPLE_APP=0"], ["-DCONFIG_LS_EXAMPLE_APP=1"]):
    obj = out / "example-compile.o"
    subprocess.run([cc, "-std=c11", "-Werror", "-Wall", "-Wextra", *flag,
                    "-I", str(root / "components/apps"), "-c", str(source),
                    "-o", str(obj)], check=True)
    symbols = subprocess.check_output([nm, str(obj)], text=True)
    assert ("ls_scr_example" in symbols) == (flag == ["-DCONFIG_LS_EXAMPLE_APP=1"])
header = out / "example-header.cpp"
header.write_text('#include "tui/apps/example_app.h"\n'
                  'const ls_app_t app = LS_EXAMPLE_APP_ENTRY;\n')
subprocess.run([cxx, "-std=c++17", "-Werror", "-DCONFIG_LS_EXAMPLE_APP=1",
                "-I", str(root / "components/apps"), "-c", str(header),
                "-o", str(out / "example-header.o")], check=True)
print("PASS C enabled/default-off/explicit-off symbols and C++ registration header")

def run(posture, *args, name="probe"):
    image = out / f"example-{posture}-{name}.bmp"
    command = [str(exe), "example", "--tft", "-d", "-f", "20", "-o", str(image)]
    if posture == "landscape":
        command += ["-l"]
    result = subprocess.run(command + list(args), capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr
    return result.stdout

for posture in ("portrait", "landscape"):
    text = run(posture, name="idle")
    assert "Count: 0" in text
    assert "0 / 16 / 32 bits" in text
    rects = [tuple(map(int, m)) for m in re.findall(
        r"example-button (\d+) (\d+) (\d+) (\d+) (\d+)", text)]
    assert len(rects) == 3, text
    for i, x, y, w, h in rects:
        assert w >= 8 and h >= 3, rects
    taps = [f"{x+w//2}:{y+h//2}" for i, x, y, w, h in rects]
    assert "Count: 1" in run(posture, "-x", taps[0], name="count")
    assert "Count: 2" in run(posture, "-k", "cc")
    assert "Count: 0" in run(posture, "-k", "ccr")
    assert "Count: 0" in run(posture, "-x", taps[0]+","+taps[1])
    assert "Count:" not in run(posture, "-x", taps[2])
    assert "Count:" not in run(posture, "-k", "h")
    assert "Count: 0" in run(posture, "-k", "z")
    print(f"PASS {posture}: registration, count/reset/home by tap and key, 3x8 targets")

# Inspect private session logic in a tiny host translation unit; production
# callbacks and the real freshness helper are linked, unrelated UI is discarded.
harness = out / "example-state.c"
harness.write_text(r'''
#include <assert.h>
#include "tui/apps/example_app.c"
static int64_t now = 1000000;
int64_t esp_timer_get_time(void) { return now; }
const ls_app_t *ls_app_by_id(const char *id) { (void)id; return NULL; }
int ls_tui_screen_index_of(const ls_tui_screen_t *s) { (void)s; return -1; }
void ls_tui_screen_show(int i) { (void)i; }
int main(void) {
    enter();
    assert(s_count == 0 && s_used == 0 && !ls_fresh_level(&s_pulse, 600));
    assert(!activate(-1) && s_used == 0);
    for (unsigned i = 1; i <= 30; ++i) {
        assert(activate(0) && s_count == i);
        now += 1000;
    }
    assert(s_used == 24 && s_head == 6);
    for (int i = 0; i < 24; ++i) assert(s_history[(s_head+i)%24] == (unsigned)i+7);
    assert(ls_fresh_level(&s_pulse, 600) >= 128);
    now += 600000;
    assert(!ls_fresh_level(&s_pulse, 600));
    activate(1);
    assert(s_count == 0 && s_history[6] == 0 && s_last == 1);
    s_count = UINT_MAX;
    activate(0);
    assert(s_count == UINT_MAX && s_history[7] == UINT_MAX);
    enter();
    assert(s_count == 0 && s_used == 0 && s_head == 0 && !s_pulse.seeded);
    activate(0);
    assert(s_history[0] == 1 && s_used == 1);
    return 0;
}
''')
state_exe = out / "example-state.exe"
subprocess.run([cc, "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror",
                "-flto", "-ffunction-sections", "-fdata-sections", "-DCONFIG_LS_EXAMPLE_APP=1",
                "-I", str(root / "components/apps"), "-I", str(root / "bench/shims"),
                str(harness), str(root / "components/apps/tui/ls_motion.c"),
                "-Wl,--gc-sections", "-o", str(state_exe)], check=True)
subprocess.run([str(state_exe)], check=True)
print("PASS history chronology/rollover, saturation, reset, re-entry and real pulse expiry")

for posture in ("portrait", "landscape"):
    text = run(posture, "-k", "c" * 30 + "r")
    assert "Samples: 24/24  Scale: 30" in text and "Count: 0" in text
    text = run(posture, "-k", "c" * 30 + "r" + "c" * 24)
    assert "Samples: 24/24  Scale: 24" in text
    assert "[+] EVENT" in run(posture, "-A", "20", "-f", "1", "-k", "c")
    assert "[o] READY" in run(posture, "-A", "20", "-k", "c")
    run(posture, "-A", "20", "-k", "ccrccc", name="still-a")
    run(posture, "-A", "20", "-k", "ccrccc", "-x", "~40", name="still-b")
    assert (out / f"example-{posture}-still-a.bmp").read_bytes() == (out / f"example-{posture}-still-b.bmp").read_bytes()
    print(f"PASS {posture}: history window scale and no frame-generated samples/motion")
