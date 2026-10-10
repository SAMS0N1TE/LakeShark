# SD reads on T-Display-P4 LR2021

Run `carto sdbench [MB]` in the firmware console (default 8 MiB, range
1..256). It reads `/sdcard/maps/central_nh_compact.ctile`, or the first
existing `.ctile` in that directory, without writing anything. The file must
be at least 128 KiB. Sequential samples stop at the smaller of MB and file size.
The full matrix can take several minutes on a slow card.

The matrix compares 4/32/128 KiB sequential requests using `fread` with default,
unbuffered, explicit 4 KiB and 32 KiB `setvbuf` settings; `read` into PSRAM;
and `read` through 64-byte-aligned internal DMA buffers followed by memcpy.
Unavailable internal allocations are reported as skipped, not benchmarked as
another path. Every case also runs the same 64 unaligned random requests at
16, 32 and 64 KiB and reports reads/s and decimal MB/s. Checksums must match
the first fread case. Seek and memcpy are timed; checksum computation and task
yields are excluded. FAT/card caches may be warm. Other SD users, including
C6 traffic on the shared controller, can affect results.

The fastest verified fd path for random 32 KiB requests is selected for this
boot, applied to the active source if its allocation succeeds and to later
opens. Any benchmark read/checksum failure prevents selection. Default streaming
uses a 32 KiB internal buffer; allocation failures try 4 KiB and then direct
PSRAM. Each CartoCore tile blob remains one callback request, with one seek and
sector-multiple read chunks, without FILE buffering. Unaligned edges are copied
from padded sector reads; the final file sector is bounded by EOF. Prefetch
queues a neighbour halo after initial show/goto and predicted neighbours after
pan/zoom. Idle consumes one hint per turn using the existing decoded cache.

## Clock and board evidence

`CONFIG_LS_SD_MAX_FREQ_KHZ` defaults to 40000 on T-Display-P4 and 20000 elsewhere.
Set it to 20000 in menuconfig to retain the original cap. This is 3.3 V SDR;
no UHS voltage switching or DDR is enabled. Init or mount verification failures
retry once with a 20 MHz cap. Verification reads eight sectors twice at the
start, middle and end of the card, comparing bytes and checking SDMMC errors.
A failed retry leaves the card unmounted; formatting is always disabled.
Verification samples are not a full-card integrity or sustained-load test.

Evidence inspected:

- Local `../vendor/T-Display-P4/project/T-Display-P4_V1.0(H0410S001AMT001-V0)_202601061148.pdf`:
  SD1 uses GPIO39..44 (IDF slot 0), external 10 kOhm pulls, a switched
  Low_3V3 supply through Q4/AO3401A and SD_PWEN, and 10 uF/100 nF decoupling.
  The GPIO39..48 bank uses ESP_LDO_VO4. `ls_panel.c` already acquires LDO4
  at 3300 mV; `ls_board_hw.c` enables the card's active-low expander supply.
- [LilyGO V1 board driver](https://github.com/Xinyuan-LilyGO/lilygo_device_driver/blob/master/src/device/t_display_p4/v1/driver.cpp)
  initializes LDO4 at 3300 mV and requests SDMMC_FREQ_52M. This supports trying
  the more conservative 40 MHz cap; performance/reliability on this particular
  card still requires a hardware run.
- The LakeShark variant and hosted config place C6 on slot 1, GPIO14..19.
  Installed IDF 5.5.4 saves divider/delay/timeout settings per slot and restores
  them when switching transactions. Both slots share a transaction mutex.
  SD cleanup explicitly uses `sdmmc_host_deinit_slot`, preserving C6 rather
  than deinitializing the entire host. No changes to hosted timing or pins.

## Verification and expectations

`python bench/tools/test_carto_sd.py` passes production-reader byte equality,
unaligned boundaries, EOF, short reads, EINTR, I/O failure, loader retention,
allocation cleanup, central-NH renders at z10..15 (warm blob reads zero), the
benchmark matrix/checksum failure guard, and existing transfer regressions.

The t-display-p4-lr2021 IDF 5.5.4 build passes with the regenerated 40000 setting.
The app is 0x466880 bytes, with 0x19780 bytes remaining in its app partition.
The original local build and test logs are unavailable in this checkout;
the build figures above are historical observations, not reproducible evidence.

40 MHz doubles the bus clock ceiling. Bulk aligned transfers aim to avoid
single-sector bounce overhead; idle prefetch helps subsequent neighbouring
frames, not the first cold frame. No hardware throughput or new frame latency
is claimed. No flashing, commits or pushes were performed.
