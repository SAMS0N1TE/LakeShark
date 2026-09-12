# Tests and builds

Run from the repository root with PowerShell 7, Python 3, CMake and a C/C++ compiler available.

```powershell
pwsh -NoProfile -File bench/verify.ps1 -Level host
pwsh -NoProfile -File bench/verify.ps1 -Level smoke
pwsh -NoProfile -File bench/verify.ps1 -Level full
```

The host gate runs decoder and UI tests, C++ header checks and source guards. The smoke gate also builds T-Display-P4. The full gate builds every enabled configuration in `bench/configs.json`.

Use `-Filter pocsag` to select tests or `-Sanitize` for address and undefined-behavior checks with a supported compiler.

Firmware builds require ESP-IDF 5.4.3. Set `IDF_PATH` to the local installation.

```powershell
pwsh -NoProfile -File bench/build.ps1 t-display-p4
pwsh -NoProfile -File bench/build.ps1 p4-nano
pwsh -NoProfile -File bench/build.ps1 p4-touch-lcd-43
```

Host tests do not verify RF reception or physical hardware.
