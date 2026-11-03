# LakeShark Windows installer

## Desktop updates

The exe's version is baked into `web/build.json` from `lakeshark.iss`'s
`AppVersion`, including release builds. Studio's Help menu offers **Check for
updates** and an **Automatic update checks** toggle. Desktop has the same
controls below the screen, including when connected to a board over Ethernet.
Automatic checks run at most daily across launches/processes; settings, last
attempt and the last valid manifest live in `updater.json` beside Studio's
`~/.lakeshark/studio.json` (or beside `LSDK_STUDIO_SETTINGS`). Offline checks
are silent and stop waiting after five seconds.

After building the installer, prepare the file to publish manually:

```powershell
python host/installer/make_latest_json.py build_installer/LakeShark-Setup-2.20.0.exe build_installer/latest.json --notes "Studio and Desktop improvements."
```

The owner publishes that JSON at `https://terminalbay.com/desktop/latest.json`.
The generator reads AppVersion and the installer's actual size/SHA-256. No tool
here uploads or publishes anything. Only HTTPS release-download URLs beneath
`github.com/SAMS0N1TE/LakeShark/releases/download/` are accepted. GitHub's HTTPS
asset-CDN redirects are permitted; other redirect destinations are refused.

Endpoints are `GET /api/updater/status` and `POST /api/updater/check`,
`/api/updater/settings`, `/api/updater/install` (under `/studio` when mounted
in the USB bridge). POSTs require `X-Studio: 1` and a local Host/Origin.
Check accepts `{"manual":true}`, settings accepts `{"automatic":false}`.
Installation is frozen-only. Source Studio offers the release page instead.
The installer is downloaded privately under TEMP with pollable byte progress,
checked for exact size and SHA-256, then rechecked immediately before launch.
It receives `/SILENT /SUPPRESSMSGBOXES /CLOSEAPPLICATIONS /RESTARTAPPLICATIONS
/CURRENTUSER /RELAUNCH=studio` (or `desktop`), and the old exe exits. Inno's
silent `[Run]` entries read `/RELAUNCH`; an open Studio window takes priority.

Headless checks (no installer is actually executed):

```powershell
python -m unittest discover -s host/installer -p test_updater.py -v
node host/installer/test_updater_browser.js
python host/installer/smoke_updater.py --exe build_app/dist/LakeShark.exe
```

The browser test uses a local fixture and an installed Chromium headless shell;
its optional argument selects another headless Chromium executable. The
`LSDK_UPDATER_TEST_URL=http://127.0.0.1:PORT/latest.json` override works only in
source runs, never in a frozen release; it does not relax installer URL checks.

Updater verification in this worktree (2026-11-01): 18 backend tests, nine
headless browser checks and the frozen `--studio-only` updater smoke passed.
`make_latest_json.py` also ran against the existing installer in
`build_installer/`. The requested buildenv Python could not start, so the
release exe was rebuilt with `C:/Espressif/tools/idf-python/3.11.2/python.exe`.
Its PYTHONPATH used the installed buildenv packages plus an isolated copy of
the matching `_cffi_backend.cp311-win_amd64.pyd` from the existing
`C:/Espressif/python_env/idf5.4.4_py3.11_env/Lib/site-packages`; no packages
were installed. `PYTHONNOUSERSITE=1` and a workspace `PYTHONUSERBASE` avoided
the inaccessible roaming site directory. The rebuilt exe contains the
AppVersion and all updater assets. ISCC could not start (sandbox access
denied); the installer was not rebuilt or executed, and real silent
installation/relaunch and the interactive WebView2 window remain unverified.

Build the release executable from the repository root:

```powershell
& "$env:LOCALAPPDATA/LakeShark/buildenv/Scripts/python.exe" host/lsterm/build_app.py --release
```

Build requirements: MinGW-w64, CMake, Ninja, PyInstaller, pywebview and pyserial.
`LSDK_MINGW` selects the build host's MinGW bin directory (default `C:/mingw64/bin`).
`build_app.py` builds an isolated `build_app/sim-host` with `LSSIM_DYNAMIC_APPS=ON`,
then bundles its executable, app API import library, SDK headers and runtime DLLs.
Checkout `bench/build-lsdk` is not used or changed by the release build.
The SDK uses an asset allowlist; no user apps, tests, caches or MCP server are shipped.
Simulator PE imports are followed recursively; unresolved non-system DLLs fail packaging.

Output: `build_app/dist/LakeShark.exe`. To build the installer separately, run
`host/installer/build_installer.py` on a host allowed to execute Inno Setup 6.
It rebuilds the release exe then invokes ISCC. `--python PATH` and `--iscc PATH`
select other installed build tools. No ISCC invocation is needed for exe testing.

The installer defaults to a per-user install under
`%LOCALAPPDATA%\Programs\LakeShark`, creates a Start menu shortcut, offers optional
desktop and compiler tasks, and includes an uninstaller. The compiler task waits
for `LakeShark.exe --get-compiler`. A failed compiler download is recorded in
`%LOCALAPPDATA%\LakeShark\toolchain\install-result.json`; it can be retried from
Studio's Build menu. Compiler files and user app folders are retained on uninstall.

Studio binds only `127.0.0.1` and uses
`%USERPROFILE%\Documents\LakeShark Apps`. `--studio-only --port 8811` serves Studio
without importing pywebview or starting a desktop window. Frozen SDK paths always
refer to bundled assets, regardless of desktop source/Python settings.

The compiler pin comes from this machine's
`C:\esp\v5.5.4\esp-idf\tools\tools.json` (the requested path has an extra `esp-idf`
directory here), matching [ESP-IDF v5.5.4](https://github.com/espressif/esp-idf/blob/v5.5.4/tools/tools.json).
It is `esp-14.2.0_20260121`, win64, 705866147 bytes; URL and SHA-256 are constants in
`app-sdk/toolchain.py`. Downloads use HTTPS and must match both size and SHA-256.
The archive is extracted privately and becomes visible only when complete.
`lap.compiler()` checks `LAP_CC`, the per-user compiler, PATH, and IDF installations.

## Frozen Studio simulator

F5 builds only the user's top-level `.c` files into `APP/build/app.dll` with
w64devkit GCC. The DLL imports all public `lap_*` and `l3_*` API functions from
`lssim.exe` through the bundled `liblssim.a`. A private copy of the host and runtime
DLLs runs in `APP/build/sim-host`; `LAPSIM_DLL` selects the app with a Unicode
Windows path. The host uses the existing API adapters, renderer, radios, 3D engine,
services and cell-feed protocol. No bench sources, Python, CMake or existing GCC
installation are needed on the user's computer. The app DLL is retained until its
host process exits; Studio stops that process before rebuilding, avoiding locked DLLs.
App data stays in the existing `APP/build/sim/lapsim-data` location.

The Build menu offers separate board and simulator compiler downloads. F5 offers
"Get the compiler" automatically when the simulator compiler is missing.
`GET/POST /api/host-compiler` reports/starts its download, matching `/api/compiler`
for the board. The simulator compiler is always taken from the downloaded per-user
location; frozen builds do not discover MinGW, CLion or Espressif host tools.
Checkout `lsdk sim` keeps the existing CMake/static app build and terminal behavior.

The host compiler pin in `app-sdk/host_toolchain.py` is:

- w64devkit 2.4.0 x64 (GCC 15.2.0), 38,806,674 bytes (39 MB in Studio).
- https://github.com/skeeto/w64devkit/releases/download/v2.4.0/w64devkit-x64-2.4.0.7z.exe
- SHA-256: `80678c29a56982d7d3b342f12a0060157df5654c38d82d4adc40704070d6aafb`
- Upstream metadata: https://api.github.com/repos/skeeto/w64devkit/releases/tags/v2.4.0
- Destination: `%LOCALAPPDATA%/LakeShark/toolchain/w64devkit-2.4.0/w64devkit/bin/gcc.exe`.

The shared installer checks exact byte size and SHA-256 before running the console
self-extractor silently (`CREATE_NO_WINDOW`, `-y`, private `-o` directory). It then
checks expected tools and exposes the directory atomically. Download/extraction
failures are pollable and retryable. It changes neither system PATH nor registry.
The board compiler remains a separate 705,866,147-byte download (706 MB).

## Headless verification

```powershell
python -m unittest discover -s host/installer -p 'test_*toolchain.py' -v
node --check app-sdk/studio/web/studio.js
python host/installer/smoke_sim.py --exe build_app/dist/LakeShark.exe --download
python host/installer/smoke_test.py --exe build_app/dist/LakeShark.exe --port 8812 --compiler C:/path/to/riscv32-esp-elf-gcc.exe
```

`smoke_sim.py` launches `LakeShark.exe --studio-only --port 8811`, with isolated
USERPROFILE/LOCALAPPDATA under `build_installer/sim-smoke` and only System32 on PATH.
It tests the missing-compiler response, performs the real download and verification,
creates all five templates through Studio, and starts them through `/api/run`.
It reads six complete WebSocket cell frames for each, verifies their CRCs, decodes
the grids, counts visible app cells and compares frame hashes. It exercises blank
keyboard input, 3D camera input and a service decode, plus natural radio/dots animation.
An edited app with an extra C file also checks compiler diagnostics and DLL rebuilding.
It saves `.cells`, text grids, download status and `results.json` as evidence.
The owned process tree is stopped on success or failure. `--evidence DIR` selects a
new isolated profile for another real download. No desktop window is opened.

## Verification in this worktree, 2026-11-01

- The real frozen executable downloaded and SHA-256 verified w64devkit from GitHub.
- Blank, radio, dots, 3D and service each returned six nonempty 52x70 frames.
  Radio/dots animated; blank and 3D reacted to keys; the service consumed a TEST decode.
- Edited multi-file app, compiler error/recovery, DLL reload, board build, SDK/static
  assets and the existing frozen CLI failure handling passed.
- Seven offline compiler installer tests and JavaScript syntax checks passed.
- Source checkout `lsdk sim ... --frames 2` produced captured frames successfully.
- Final evidence: `build_installer/sim-smoke-verified/results.json`, `smoke-sim.log`,
  `smoke-exe.log`, `smoke-checkout-sim.log`, `build-sim-release.log`.
- The requested buildenv interpreter still cannot launch its base Python 3.12
  ("Unable to create process"). `build-requested-python.log` records the attempt.
  The release exe was built with accessible `C:/Espressif/tools/idf-python/3.11.2/python.exe`
  using the already installed local PyInstaller/webview packages; no packages were installed.
- ISCC, installer installation/uninstallation and the desktop WebView2 UI were not run.
