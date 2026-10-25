# LakeShark map worker

`LakeSharkMaps.exe` builds the US map cells that LakeShark boards download and
publishes them to the map host. As cells go live, the globe on
[terminalbay.com/?m=lakeshark-maps](https://terminalbay.com/?m=lakeshark-maps)
fills in. Copy the one EXE to any Windows 10/11 PC and run it; several PCs can
run at once and share the work.

## Run it

Double-click `LakeSharkMaps.exe`, or from PowerShell:

```powershell
.\LakeSharkMaps.exe              # build and publish until stopped
.\LakeSharkMaps.exe status       # progress, which PCs are working, failures
.\LakeSharkMaps.exe selftest     # 10-second offline check that building works on this PC
.\LakeSharkMaps.exe token        # enter the Cloudflare token file again
.\LakeSharkMaps.exe check        # compare the ledger with what the map host serves (no upload)
.\LakeSharkMaps.exe publish      # publish this PC's finished cells now (or refresh the index files)
```

Options: `--work D:\LakeSharkMaps` picks the work folder (remembered), `--jobs 3`
builds more blocks at once on a PC with spare RAM, `--once` stops after the
first publish.

Ctrl-C stops after the current step and still publishes whatever finished.
Press Ctrl-C twice to quit at once; the unfinished block goes back to the pool.

What a PC needs: about 60 GB free disk, about 6 GB of free RAM for one job
(the builder measured 3.8-5.3 GB peaks), and a good connection. With less free
RAM the worker says so and waits, retrying every 10 minutes; it never counts
that against a block. The first run downloads the US OpenStreetMap extract from Geofabrik
(about 12 GB, resumable) and spends a few minutes checking it.

## First run: the token

On first run the worker asks for the Cloudflare API token file (the `cfat_...`
file, `CF_TB_API.txt` on the workshop PC). Type or drag in its path. The path is
remembered in `%APPDATA%\LakeSharkMaps\settings.json`; nothing is stored next to
the EXE and the token is never printed. On a PC without a copy of the file you
can paste the token itself at the same prompt; it is then stored encrypted with
Windows DPAPI, readable only by that Windows user on that PC.

The token needs D1 Edit and Cloudflare Pages Edit on the account (the current
account token has both).

## Sharing the work across PCs

Every PC works from one ledger, the D1 database `lakeshark-maps` on the
Cloudflare account:

- The nation is cut into blocks of up to four z8 cells (1,140 cells in all).
  A PC claims a block, builds it, publishes it, and claims the next. A claim
  is atomic, so two PCs never build the same block.
- A claim is held by a heartbeat. If a PC is switched off mid-block, the block
  returns to the pool after two hours and another PC picks it up.
- A block that fails three times is parked; `status` lists it with the error.
- D1's free tier allows 100,000 rows written a day for the whole account,
  the website's writes included. A publish writes only the files that changed,
  a heartbeat writes only when something changed (or every 10 minutes), and
  each PC stops writing at 20,000 rows a day (`LSMAPS_D1_WRITES` changes it)
  and waits for 00:00 UTC while it keeps building. The count is in
  `%APPDATA%\LakeSharkMaps\d1-writes.json`.
- Cells built on a PC but not yet published are kept in its work folder
  (`unpublished.json`) and go out on its next start.
- Publishing is incremental: the worker takes a short lock, uploads only its
  new cell parts plus the small catalog and state files, and deploys the full
  file list from the ledger. Nothing published by another PC is dropped, and no
  PC needs the others' files.
- A PC prefers blocks near the ones it already cut out of the US extract, so
  its region extracts get reused.

The map host is the `cartocore-maps` Pages project, `preview` branch
(`preview.cartocore-maps.pages.dev`), which is where the US cells and the site's
map viewer already live. The worker never deploys terminalbay.com itself.

## What the site shows

The maps page reads `/lakeshark-maps/coverage.json` and `/lakeshark-maps/build.json`.
The `lakeshark-maps-data` Worker in the terminalbay repo
(`workers/lakeshark-maps-data/`) serves both live from the ledger: coverage
changes as each publish lands, and the BUILDING strip shows the states being
worked on while any PC has checked in within 15 minutes. The map host also
gets `site/coverage.json` and `site/build.json` with every publish.

## Building the EXE

On the workshop PC (Python 3.12, MSYS2 UCRT64 gcc, CMake):

```powershell
python -m venv C:\Users\DEV\.lsmaps-build\venv
C:\Users\DEV\.lsmaps-build\venv\Scripts\pip install -r requirements.txt
C:\Users\DEV\.lsmaps-build\venv\Scripts\python build_exe.py
```

`build_exe.py` compiles CartoCore's `cartocore.exe` and
`cartocore_validate_cells.exe`, collects `osmium.exe` with its DLLs (conda-forge
`osmium-tool`, e.g. `micromamba create -p C:\Users\DEV\.lsmaps-build\osm -c conda-forge osmium-tool`),
bundles CartoCore's US builder (`CartoCore/tools/build_us.py` and friends) and the Natural
Earth boundaries, and writes `dist/LakeSharkMaps.exe`. It then runs the EXE's
selftest from a clean PATH. The CartoCore commit it was built from is inside the
EXE (`CARTOCORE_COMMIT.txt`).

CartoCore source: branch `work/maps-worker-20261023` (the Plus-by-zoom US
builder from `work/us-cells-20261019` with the portable launcher from
`work/portable-builder-20261020`).

## One-time setup (already done)

`LakeSharkMaps.exe bootstrap` created the ledger from what the map host served
on 2026-10-23: every live file (hashed and checked against Cloudflare's store),
the 41 built cells, the 52 states and the block plan. It refuses to run again
unless given `--force`.
