# Deploying the LakeShark page to terminalbay.com

Everything in `dist/site/` is ready to upload. Nothing here has been published.

## Read this first

**Do not put the firmware online until the third-party review closes.** Serving
`lakeshark.bin` for download is distribution, and that triggers the GPL's
corresponding-source obligation for the whole image plus every notice
obligation in `docs/THIRD_PARTY_REVIEW.md`. Still open there:

- SAM speech synthesiser — its own notice says permission is unresolved.
- The Consolas bitmap fonts in `components/apps/sdr_ui/` — no provenance.
- Helix MP3 — the inner RealNetworks terms are not the outer Apache-2.0 ones.
- The DSD-derived P25 helpers — see `components/lakeshark/apps/p25/UPSTREAM.md`.

The page itself is fine to publish now. The `/fw/` directory is the part that
waits. If you want the page up first, delete the `/fw/` upload and the flasher
degrades to a plain "no image yet" state rather than a broken button.

The storage image in this bundle is **not** the one the bench board runs: it is
rebuilt from `spiffs/panels/` only, with `spiffs/music/` left out because the
five MP3s there have no established rights. It was flashed and booted on the
LCD 4.3 to confirm the board comes up without them.

## Upload

    lakeshark.html            ->  /lakeshark.html
    fw/lakeshark-lcd43/       ->  /fw/lakeshark-lcd43/

The page expects the manifest at `/fw/lakeshark-lcd43/manifest.json` and the
zip at `/fw/lakeshark-lcd43/lakeshark-lcd43.zip`. Both paths are relative to
the site root, so they work from the shell iframe as well as standalone.

`.bin` must be served as a plain file. If openresty is configured to gzip or
transform unknown types, exclude `/fw/` — esptool-js needs the bytes verbatim.

## Add it to the shell

`index.html`, in the Module Selection fieldset, after the ZeroMesh entry:

```html
                            <label>
                                <input type="radio" name="viewMode" value="lakeshark.html">
                                LakeShark
                            </label>
```

The page redirects a standalone visit to `/?m=lakeshark`, matching what
`zeromesh.html` does, so the shell has to know that value or the redirect
lands on the dashboard.

## Check after uploading

1. `https://terminalbay.com/?m=lakeshark` renders inside the shell.
2. `https://terminalbay.com/lakeshark.html` redirects into the shell.
3. `curl -I https://terminalbay.com/fw/lakeshark-lcd43/manifest.json` → 200,
   `application/json`.
4. In Chrome, "Connect and install" opens the serial port picker. It needs
   HTTPS; the live site has it, `http://` test hosts do not.
5. Flash one real board from the page end to end before telling anyone.

## What was verified here

- The page renders in the site's own idiom inside an iframe, no console errors,
  and `esp-web-install-button` upgrades and draws its button.
- `ESP32-P4` is a supported `chipFamily` in esp-web-tools 10 — checked against
  the `ChipFamily` union in the project's `src/const.ts`, not assumed.
- Manifest offsets match `build_lcd43/flash_args` exactly: 0x2000, 0x8000,
  0x10000, 0xe10000.
- The music-free storage image uses the same SPIFFS geometry as the build
  (page 256, obj name 32, meta 4, magic + magic length) and boots.

Not verified: a real flash driven from the page. That needs the files actually
hosted over HTTPS, so it is the first thing to do after upload.
