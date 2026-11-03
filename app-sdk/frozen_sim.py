"""Build only the user's C files, importing the bundled simulator's app API."""
import os
from pathlib import Path
import subprocess

import host_toolchain
import lap


def build(sdk, app, say):
    import lsdk
    cc = host_toolchain.compiler_path()
    if not cc.is_file():
        message = "Get the simulator compiler (39 MB) from Studio's Build menu, then run again."
        return lsdk.BuildResult(False, [{"file": str(app), "line": 1, "col": 1,
                                       "severity": "error", "message": message}], message,
                                error_code="host_compiler_missing")
    out = Path(app) / "build" / "app.dll"
    out.parent.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, PATH=str(cc.parent) + os.pathsep + os.environ.get("PATH", ""))
    # Ignore host GCC configuration inherited from unrelated developer installs.
    for key in ("GCC_EXEC_PREFIX", "COMPILER_PATH", "LIBRARY_PATH", "CPATH", "C_INCLUDE_PATH"):
        env.pop(key, None)
    cmd = [str(cc), "-std=gnu11", "-O2", "-g", "-Wall", "-Wextra", "-shared", "-static-libgcc",
           "-Wl,--export-all-symbols", "-include", str(sdk / "tools" / "sim_alloc.h"), "-I", str(sdk / "include"), "-I", str(sdk / "lib"), "-I", str(app),
           *map(str, lap.project_sources(app) + sorted((sdk / "lib").glob("*.c"))), str(sdk / "sim" / "liblssim.a"),
           "-lm", "-o", str(out)]
    say("building app DLL with " + str(cc))
    r = subprocess.run(cmd, env=env, capture_output=True, text=True, errors="replace",
                       creationflags=subprocess.CREATE_NO_WINDOW)
    text = r.stdout + r.stderr
    problems = lsdk.parse_problems(text, app)
    if r.returncode and not any(p["severity"] == "error" for p in problems):
        problems.append({"file": str(app), "line": 1, "col": 1, "severity": "error",
                         "message": "Simulator app build failed: " + text[-2000:]})
    return lsdk.BuildResult(r.returncode == 0, problems, text, out if not r.returncode else None)
