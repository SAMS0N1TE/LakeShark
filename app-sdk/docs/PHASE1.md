# Simulator contracts and Phase 1 tooling

Generate the current API version, per-call metadata and coverage table:

```text
python app-sdk/apidoc.py --generate
```

`api_registry.py` enriches declarations from the public headers and checks
them against `exports.txt`. `apidoc.entries()` supplies the same version,
capability and simulator fields to Studio completion/documentation and CLI
snippets. `docs/api.json`, `docs/simulator.json`, `docs/SIMULATOR.md` and the
marked README version block are generated views.

The default API is 2.3, derived from the public header; lap.py accepts 1.0–1.3 and 2.0–2.3 manifests. Studio reads the same generated registry metadata through apidoc.

The simulator compiles the firmware validators. Synchronous inputs and outputs accept app image, tracked app allocations, or the active callback stack. This includes radio metadata/sequence outputs, input/capability snapshots, dialog results, save reads, events, widgets, audio and socket copies. SD read/list jobs retain destination pointers until poll completion; those buffers must be app image or tracked app allocations. Allocation redirection in sim_alloc.h models the board resolver. MORSE and SPECTRE use synchronous stack outputs and need no app changes. Callback code must remain in the app image; lap3d retains certain arrays through rendering as its header documents.

`LAPSIM_CONSENT=grant` (the default headless loader answer) approves only
manifest-declared flags. `deny` cancels a privileged launch before `enter`.
Networking is first initialized denied, then granted only for a declared
network app, matching the loader sequence. Host mesh sends also require TX
capability and an armed/running synthetic mesh. Consent is scripted; it does
not exercise device signatures, grant persistence or trusted prompt UI.

Scripts support `focus=0|1`, `keydown=C`, `keyup=C` (also `@left`, `@right`, `@up`, `@down`, etc.),
`pointer=x,y,buttons`, `buttondown=x,y,button,buttons`,
`buttonup=x,y,button,buttons`, `wheel=steps` and `resize=w,h`. Legacy `key=`
emits a down/up pulse; held keys use the explicit edge commands. These feed
the shared input channel. `resize=` injects an event; actual client geometry
still comes from host rendering.

`LAPSIM_SCRIPT` supports `N:hide`, `N:show`, `N:stop`, `N:switch` and
`N:revoke`. A hidden service keeps ticking; reopening preserves its generation
and calls `enter` again. Stop/switch/revocation ends the service and unloads.
The normal host UI polling path also pumps services while another screen is
shown. Close runs `leave`, ends shared services, drains worker cancellation,
flushes cached files and finally frees generation allocations. Process exit
runs the same final cleanup. `/sdcard/...` maps to the private working
directory's `sdcard/`; app-data cache files live in `sdcard/apps_data/<app-id>/`,
using the shared atomic staging/backup/replacement/recovery path.

Workers run deterministic passes at frame/poll boundaries instead of separate
device tasks. Service deadlines use virtual frame time; tick budgets include
elapsed host wall time. Host performance does not predict board budgets.
Static apps share the executable's image range; DLL apps use their own PE
image. RV32 relocations, ABI, signature verification, hardware arbitration,
wireless delivery, device timing and FAT power-loss recovery need board or
dedicated loader tests. Per-call `simulator limit:` messages print once.

Targeted verification (headless, no board connection):

```text
cmake --build bench/build --target lssim test_lap_api test_lap_radio test_lap_net test_sdk_libraries
ctest --test-dir bench/build -R "^(test_lap_(api|radio|net)|test_sdk_libraries)$" --output-on-failure
python -m unittest discover -s app-sdk/tests -p test_phase1.py
python app-sdk/tests/test_sim_runtime.py
# PowerShell: $env:LSDK_TEST_DYNAMIC="1"; repeat test_sim_runtime.py for DLL apps
python app-sdk/tests/test_example_contracts.py --frames 80 --build-dir bench/build
```

Every template is scaffolded, and every template/bundled app is target-built
with `lap.py`, then run through launch and hide/reopen/stop scenarios.
`bench/build/sdk-contracts/results.json` and per-app logs hold results.
All 15 templates and bundled apps must PASS, with no expected-failure exemptions.
Synchronous stack outputs and rejected retained stack pointers are tested alongside configurable hardware inventories, held keys, input events, dialog answers/cancel and durable save/settings round trips.

`LAPSIM_HW=wifi,ble,gps,imu,sd,desktop` sets the exact host hardware inventory; the default is `wifi,ble,imu,sd,desktop`, and `none` clears it. Radio presence remains independent (`LAPSIM_RADIO=none|sim`, or script `radio=none|sim`). Script `caps=...` changes the hardware inventory. Script answers `dialogtext=Grace`, `dialogpick=1` (zero based), and `dialogcancel` complete the shared modal runtime without opening desktop windows. Saves/settings remain asynchronous flush jobs; poll their returned IDs with lap_sd_poll.
