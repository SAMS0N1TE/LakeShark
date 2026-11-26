# Release ELFs: every release keeps the ELF of each image it ships

A field report's crash (`crash:` task, pc, ra, `elf-sha=`, stack-scanned code
addresses) decodes only with the exact ELF of the image that crashed. Up to
3.1.0 a release kept only `.bin` images, so a 3.1.0 crash
(report 0mwwa3t1x-c6e88b, image `3.1.0-g5d4c0f3b85a9`) could not be decoded:
its ELF was not kept anywhere.

## Where they go

**On the GitHub release, as one asset: `lakeshark-<version>-elf.zip`.**
It holds `<sha12>.elf` for every distinct image the release ships, plus
`ELFS.txt` / `ELFS.json` (full sha256, image version, the release files each
ELF belongs to). It is listed in the release's `SHA256SUMS` like every other
asset.

Why GitHub and not R2 or the private artifacts branch:
- the public release is where the images are, so the ELF stays next to the
  image it decodes, for as long as that image is downloadable;
- anyone with the public source can rebuild and read these symbols anyway
  (see below), so nothing is gained by hiding them;
- `tools/ls_crash_decode.py --release` fetches it without credentials. The
  hub's R2 bucket would need a wrangler login, and the gate's
  `release/<ver>-artifacts` branches on the private remote are per hotfix,
  incomplete for 3.x, and private.

One zip instead of one `.elf` per image: since 3.0 the tdp4, tdp4lr, tdp4net
and tdp4u images are byte-identical (one ELF), and an ELF is about 46 MB
(about 17 MB zipped).

## Release step (on the PC that publishes)

After the images are attached (`tools/ota_github_assets.py ... --upload vX.Y.Z`):

```
python tools/release_elfs.py pack --version X.Y.Z --out dist \
    <the release's .bin files or gated images/> <the build folders or lakeshark.elf.zip files> \
    --sums dist/SHA256SUMS --upload vX.Y.Z
```

- Images are matched to ELFs by the ELF sha256 every ESP app image carries in
  its app descriptor (`esp_app_desc_t.app_elf_sha256`), the same hash the
  board reports as `elf-sha=`. Names and folders do not matter: give it the
  build folders (`build_tdp4/lakeshark.elf`, `build_*/elf-archive/`) or the
  gate's `images/<edition>/lakeshark.elf.zip`.
- An image without its ELF stops the pack. A rebuild does not help: a rebuild
  has a new build time, so a new image and a new ELF. Keep the build folder
  (or the gate's ELF zip) of the exact images that are published.
- An image carried over from an older release (tdp4tft 2.23.2 on v3.1.0)
  is named with `--carried <file name>`; its ELF is on the release that built it.
- `--sums` adds the zip's line to the release's `SHA256SUMS` and uploads that
  file too. The zip is byte-reproducible, so a rerun gives the same line.

## Public/private split

ELF symbols and DWARF name every compiled function, source file and path.
Since 3.0.0 the public release tree is the private release tree (v3.1.0 on
SAMS0N1TE/LakeShark is the private `release/3.1.0` tree, byte for byte), so a
release ELF names nothing the public source does not. `--upload` checks this
every time: the commit in the image version (`-g<sha12>`) must differ from the
public tag only under `bench/`, `docs/`, `tools/`, `flipper-app/`, `.github/`,
`assets/` or in top-level `.md` files. A dirty build (`-dirty`) is refused.
If the split ever comes back (the 2.x `make_public.py` that dropped
private paths), the check fails and lists the firmware
paths the ELF would reveal, rather than uploading.

## Decoding

```
python tools/ls_crash_decode.py --report <id> --release auto
python tools/ls_crash_decode.py report.json --release v3.2.0
```

`--release` is only used when no local ELF matches. It downloads the
release's `*-elf.zip` into `build_release/elf-archive/` (ignored by git, and
already on the decoder's search path), so later decodes of that release need
no network. `auto` takes the tag from the report's version.
Releases up to and including v3.1.0 have no ELF zip.
