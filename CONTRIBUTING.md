# Contributing to LakeShark

Bug reports, hardware testing, documentation fixes and code contributions are welcome. For a large feature or a new board, open an issue first so we can agree on the scope before you spend time on it.

Please follow the [code of conduct](CODE_OF_CONDUCT.md). Report vulnerabilities through the [security policy](SECURITY.md).

## Reporting bugs

Search the existing issues, then use the bug report template. Include your board and panel variant, firmware tag or commit, connected radios, steps to reproduce, and what happened. For reception problems, include the receiver, mode, antenna and relevant settings.

Trim logs to the relevant section. Remove passwords, tokens, private messages and precise personal locations before uploading logs, screenshots or captures. Only share recordings and sample data you have permission to distribute.

## Getting set up

Start with the [README](README.md) for hardware requirements and board-specific build commands. Firmware builds use ESP-IDF 5.4.3. Keep a separate build directory and sdkconfig for each board; do not reuse a CMake cache from another machine or target.

The [test guide](bench/README.md) covers the host checks and firmware build gates. Host checks need PowerShell 7, Python 3, CMake, Ninja and a host C/C++ compiler. From the repository root:

```powershell
pwsh -NoProfile -File bench/verify.ps1 -Level host
```

With ESP-IDF set up, the smoke gate also builds the T-Display-P4:

```powershell
pwsh -NoProfile -File bench/verify.ps1 -Level smoke
```

Use `-Level full` for all enabled board configurations. A passing host test does not establish that a receiver works on live RF or that a change works on hardware.

## Making a change

1. Create a branch from the current `main`.
2. Keep the change focused and follow the surrounding code style.
3. Add a regression test when it can reproduce the bug or verify new behaviour. Update the relevant docs when controls, setup or behaviour change.
4. Run the checks relevant to your change. For documentation-only edits, check links, paths and commands. For firmware changes, run host checks and build the affected boards where possible.
5. Open a pull request with the reason for the change, test results and any limits on testing.

Keep board identity checks inside `components/lakeshark/board/`. Other code should use the capabilities derived in `ls_caps.h`. Variant headers describe physical hardware; they should not define `LS_HAS_*` capabilities directly. CI checks these rules.

Do not commit build output, local caches, credentials or private captures. Preserve third-party copyright and license notices. If you add a dependency or bring in code, identify its source and update [the third-party inventory](docs/THIRD_PARTY_NOTICES.md) where needed.

## Pull requests and hardware results

Describe what changed and why. List the commands you ran and whether they passed. For hardware tests, name the board, firmware revision, peripherals and the behaviour you observed. Screenshots help with UI changes.

If you could not run a check, say so. Mark unverified hardware and RF behaviour clearly, and keep existing experimental labels until there is evidence to remove them. Small, reviewable changes are easier to test and merge.
