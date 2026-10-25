"""Reproduce host before/after timings, loop proxies and P4 stack/size evidence.

Run from the worktree: python bench/tools/l3bench_audit.py
Everything generated stays under ignored bench/build-lap3d. No hardware access.
The baseline revision is the source of l3bench_reference.h, never the worktree.
"""
import argparse
import json
import re
import statistics
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
ENGINE = ROOT / "components/apps/tui/lap3d"
BASELINE = "35c4e48b5f9887f7616aed3f6e7a08f7d0e67075"


def run(args, **kwargs):
    result = subprocess.run(args, text=True, capture_output=True, **kwargs)
    if result.returncode:
        raise RuntimeError(str(args[0]) + " failed:\n" + result.stdout + result.stderr)
    return result.stdout


def host(source, output, profile=False):
    output.mkdir(parents=True, exist_ok=True)
    bench = (ROOT / "bench/tools/l3bench.c").read_text()
    # Both executables use the same benchmark recipe, optimization and compiler.
    bench = bench.replace('../../components/apps/tui/lap3d/l3_private.h',
                          (source / "l3_private.h").as_posix())
    driver = output / "l3bench.c"
    driver.write_text(bench)
    exe = output / "l3bench.exe"
    args = ["gcc", "-O2", "-g", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
            "-Wno-unused-parameter", "-Wdouble-promotion", "-I" + str(ROOT / "app-sdk/include"),
            "-I" + str(ROOT / "bench/tools")]
    if profile:
        args.append("-DL3_PROFILE")
    run(args + [str(p) for p in sorted(source.glob("*.c"))] +
        [str(driver), str(ROOT / "bench/shims/lap3d_io.c"), "-lm", "-o", str(exe)], cwd=ROOT)
    return exe


def instrument_baseline(source):
    """Host-only counters on original code, without altering release timing."""
    header = source / "l3_private.h"
    text = header.read_text()
    current = (ENGINE / "l3_private.h").read_text()
    counts = re.search(r"typedef struct \{\n    uint32_t candidates.*?} l3_counts;",
                       current, re.S).group()
    text = text.replace("typedef struct {\n    l3_vec o, dir", counts +
                        "\ntypedef struct {\n    l3_counts *counts;\n    l3_vec o, dir")
    text = text.replace("    l3_work work[2];", "    l3_counts counts[2];\n    l3_work work[2];")
    header.write_text(text)
    p = source / "l3_frame.c"
    p.write_text(p.read_text().replace("    s->recording = false;",
                                      "    s->recording = false;\n    memset(s->counts, 0, sizeof(s->counts));"))
    p = source / "l3_raster.c"
    text = p.read_text().replace("    l3_work *w = &s->work[worker];",
                                "    l3_work *w = &s->work[worker];\n    w->counts = &s->counts[worker];")
    text = text.replace("    p->depth = w->hit;", "    s->counts[0].writes++;\n    p->depth = w->hit;")
    text = text.replace("    l3_mat *m = &s->material[mat];",
                        "    s->counts[0].lights++;\n    l3_mat *m = &s->material[mat];")
    text = text.replace("    float x, y;", "    ((l3_scene *)s)->counts[0].shadows++;\n    float x, y;", 1)
    text = text.replace("            int64_t e0 =", "            s->counts[0].edges++;\n            int64_t e0 =")
    text = text.replace("                a /= t->v[0].z;",
                        "                s->counts[0].triangle_reciprocals += 4;\n                a /= t->v[0].z;")
    text = text.replace("                l3_view_ray(&s->view,",
                        "                s->counts[0].candidates++;\n                l3_view_ray(&s->view,")
    text = text.replace("                if (l3_intersect(c, w, near_t, far_t))",
                        "                s->counts[0].intersections++;\n                if (l3_intersect(c, w, near_t, far_t))")
    p.write_text(text)
    p = source / "l3_geometry.c"
    text = p.read_text().replace("    for (int i = 0; i < 160 && lo <= hi; i++) {",
                                 "    for (int i = 0; i < 160 && lo <= hi; i++) {\n        w->counts->steps++;")
    text = text.replace("    w->hit =\n        ((c->a.x", "    w->counts->plane_reciprocals++;\n    w->hit =\n        ((c->a.x")
    p.write_text(text)
    p = source / "l3_resolve.c"
    p.write_text(p.read_text().replace("            const l3_pixel *p =",
                                      "            s->counts[0].resolves++;\n            const l3_pixel *p ="))


def target(source, output, commands):
    output.mkdir(parents=True, exist_ok=True)
    compiler = None
    for command in commands:
        if "/lap3d/" not in command["file"].replace("\\", "/"):
            continue
        compiler, args = command["command"].split(" ", 1)
        name = Path(command["file"]).stem
        # Response files avoid cmd.exe's length limit and preserve IDF flags.
        args = re.sub(r" -o .*? -c .*$", " -fstack-usage -o " +
                      (output / (name + ".o")).as_posix() + " -c " +
                      (source / (name + ".c")).as_posix(), args)
        rsp = output / (name + ".rsp")
        rsp.write_text(args)
        run([compiler, "@" + str(rsp)], cwd=command["directory"])
    if not compiler:
        raise RuntimeError("No lap3d commands in target compile database")
    size = compiler.replace("gcc.exe", "size.exe")
    report = run([size, *map(str, sorted(output.glob("*.o")))])
    (output / "size.txt").write_text(report)
    frames = []
    for p in output.glob("*.su"):
        frames.extend(p.read_text().splitlines())
    frames.sort(key=lambda line: int(line.split("\t")[1]), reverse=True)
    (output / "stack.txt").write_text("\n".join(frames) + "\n")
    if any(int(line.split("\t")[1]) > 256 for line in frames):
        raise RuntimeError("Target stack frame exceeds 256 B")
    return report, frames[:6]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runs", type=int, default=5)
    parser.add_argument("--target-build", default="build_tdp4_lronly")
    args = parser.parse_args()
    output = ROOT / "bench/build-lap3d/audit"
    reference = output / "reference"
    reference.mkdir(parents=True, exist_ok=True)
    for p in sorted(ENGINE.glob("*")):
        if p.suffix not in (".c", ".h"):
            continue
        text = run(["git", "show", BASELINE + ":components/apps/tui/lap3d/" + p.name], cwd=ROOT)
        text = text.replace('../../../../app-sdk/include/lap3d.h',
                            (ROOT / "app-sdk/include/lap3d.h").as_posix())
        (reference / p.name).write_text(text)
    report = []
    for label, source in (("before", reference), ("after", ENGINE)):
        dest = output / label
        exe = host(source, dest)
        samples = {}
        last = ""
        for _ in range(args.runs):
            last = run([str(exe), str(dest), str(dest)], cwd=ROOT)
            for line in last.splitlines()[1:]:
                fields = line.split()
                key = tuple(fields[:2])
                samples.setdefault(key, []).append(int(fields[2]))
        (dest / "timing.txt").write_text(last)
        for key, values in samples.items():
            report.append(f"{label} {' '.join(key)} median_us={statistics.median(values):g}")
        database = ROOT / args.target_build / "compile_commands.json"
        if database.exists():
            size, frames = target(source, dest / "target", json.loads(database.read_text()))
            report.extend([label + " target object sizes:", size, *frames])
    instrument_baseline(reference)
    for label, source in (("before", reference), ("after", ENGINE)):
        dest = output / (label + "-profile")
        exe = host(source, dest, profile=True)
        proxy = run([str(exe), str(dest)], cwd=ROOT)
        (dest / "proxy.txt").write_text(proxy)
        report.extend(label + " " + line for line in proxy.splitlines() if line.startswith("proxy"))
    text = "\n".join(report) + "\n"
    (output / "report.txt").write_text(text)
    print(text)


if __name__ == "__main__":
    main()
