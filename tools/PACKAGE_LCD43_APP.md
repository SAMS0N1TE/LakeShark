# Local LCD4.3 application upgrade candidate

LOCAL PREVIEW ONLY. NOT CLEARED FOR PUBLIC DISTRIBUTION. The current SAM
notice explicitly reports unclear licensing; font provenance remains unresolved.
The supplied `lcd43-local-license-paths.json` is a proposed local notice list,
not a complete compliance audit or permission to publish. It preserves known
standalone notices but does not extract embedded notices from mbelib/IMBE source
or resolve external ESP-IDF/component licensing and corresponding-source duties.

This tool packages an existing build; it never builds, flashes, erases, accesses
ports, or uploads. Requires Python 3.9+ and Git. Use a newly built LCD4.3 GUI
image with its exact matching ELF and generated metadata.

```
python tools/package_lcd43_app.py --build-dir D:/path/to/build_lcd43 --output D:/local-previews/lcd43-candidate --license-list tools/lcd43-local-license-paths.json
python -m unittest discover -s bench/tests -p test_package_lcd43_app.py -v
```

Optionally append `--docs-list D:/release-review/docs-list.json --zip
D:/local-previews/lcd43-candidate.zip`. The docs list may select only these exact
source-relative paths (no absolute paths, traversal, aliases or symlinks):

```
["docs/LCD43_RELEASE_NOTES.md", "docs/LCD43_QUICKSTART.md", "docs/LCD43_TROUBLESHOOTING.md", "docs/THIRD_PARTY_REVIEW.md"]
```

Documents retain their `docs/` package paths and are included in the manifest
and checksums. They remain local-preview material, not public clearance.
The optional ZIP is created exclusively from the package's in-memory allowlist,
never by walking a directory. Entries are sorted, timestamps fixed to 1980,
permissions fixed, and bytes stored without compression: identical package
contents produce identical ZIP bytes. Separate packaging runs have different
manifest creation timestamps, so they are not promised identical archives.
ZIP output must not exist and must be outside source/build/package directories.
The ZIP includes SHA256SUMS.txt; compute its external SHA256 with
`Get-FileHash D:/local-previews/lcd43-candidate.zip -Algorithm SHA256` if needed.

The output must not exist and must be outside the source/build tree. The license
list is a reviewed JSON array of source-relative license/notice filenames, for
example `["LICENSE", "components/example/LICENSE"]`. Supply the complete list
from the release license audit, not that illustrative example. Public release
also requires fulfillment of applicable source-distribution obligations; this
tool neither audits licenses nor bundles private source patches.

The fixed output allowlist is app.bin, app.elf, manifest.json,
FLASH_INSTRUCTIONS.txt, SHA256SUMS.txt, and explicitly supplied license notices.
Only the four optional approved documents can be added.
No bootloader, partition-table image, NVS, storage, SD contents, coredump, RF
captures, or device credentials are copied. Do not add those manually.

Checks include LCD4.3-only GUI configuration and generated header, 32MB flash,
ESP32-P4 image identity, application integrity digest, embedded ELF SHA256,
project/version agreement, factory application offset/size, partition integrity,
and source/config freshness. A mismatched or missing artifact is refused before
the output is created. Failed filesystem writes can leave a partial directory;
do not distribute it without a complete checksum file and validation.

The manifest records Git HEAD and dirty state separately from a SHA256 inventory
of selected firmware inputs, including nonignored untracked inputs. Packaging
is not an independent reproducible-build attestation: ignored/vendor/toolchain
inputs are not inventoried, and HEAD does not describe uncommitted changes.
Keep the exact reviewed source snapshot privately, or make a curated source
checkpoint and rebuild before packaging. Never label a dirty candidate as an
unchanged Git release.

Application-only upgrades require verification of the device's existing
partition layout, not just its model name. The supplied instructions reject
unverified layout, secure boot, and encrypted flash. They write only app.bin at
the verified factory offset; normal application-sector erase is unavoidable,
but full-chip/data-partition erase is explicitly prohibited. This factory-slot
upgrade is not power-fail atomic. Hardware acceptance is separate from packaging.
