# Deploying the LakeShark page to terminalbay.com

Everything in `dist/site/` is ready to upload.

## Upload

    lakeshark.html            ->  /lakeshark.html
    fw/lakeshark-lcd43/       ->  /fw/lakeshark-lcd43/
    tools/lakeshark-maps.zip  ->  /tools/lakeshark-maps.zip

The page expects the manifest at `/fw/lakeshark-lcd43/manifest.json`, the
firmware zip at `/fw/lakeshark-lcd43/lakeshark-lcd43.zip` and the map tools at
`/tools/lakeshark-maps.zip`. All three paths are relative to the site root, so
they work from the shell iframe as well as standalone.

`.bin` must be served as a plain file. If openresty is configured to gzip or
transform unknown types, exclude `/fw/`. esptool-js needs the bytes verbatim.

## What is on the page

| Panel | What it does |
|---|---|
| A RADIO | esp-web-tools flasher for the LCD-4.3, plus the manual zip |
| B HEAD | the Flipper app |
| C MAPS | works out the tile plan for an area and writes the two build commands |
| D PRESETS | makes a P25 profile and channel memory files in the browser |
| E USING IT | first run, getting files off it, troubleshooting |

C and D are plain client-side JavaScript. Nothing is uploaded and there is no
backend. The map panel only calculates and prints commands; the fetching and
packing happen on the visitor's own machine with the tools in the zip.

## Keeping it current

The firmware in `/fw/` is built from `build_lcd43`. After a release:

    cp build_lcd43/bootloader/bootloader.bin           dist/site/fw/lakeshark-lcd43/
    cp build_lcd43/partition_table/partition-table.bin dist/site/fw/lakeshark-lcd43/
    cp build_lcd43/lakeshark.bin                       dist/site/fw/lakeshark-lcd43/
    cp build_lcd43/storage.bin                         dist/site/fw/lakeshark-lcd43/

Then set `version` in `manifest.json`, drop in the release zip as
`lakeshark-lcd43.zip`, and regenerate `SHA256SUMS`. The offsets in the manifest
come from `build_lcd43/flasher_args.json` and change if the partition table
does.

## Add it to the shell

`index.html`, in the Module Selection fieldset, after the ZeroMesh entry:

    <label><input type="radio" name="m" value="lakeshark"> LakeShark</label>

The page redirects to `/?m=lakeshark` if it is opened outside the shell, so a
direct link still lands somewhere sensible.
