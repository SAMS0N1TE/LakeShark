# LR2021 live FIFO service

Live capture reserves task notification index 0. After draining the observed
backlog in reads of at most 256 bytes, it reads the residual level again. Only
levels <=256 bytes permit a blocking wait. An esp_timer one-shot ISR alarm wakes
the task after 400 us, using the timer ISR yield API. A new alarm is armed only
after the previous notification is consumed. Timer creation precedes radio
configuration; every exit deletes the timer. Finite captures do not use it.

At 500 bytes/ms, 256 bytes leaves 1536 us to full. Subtracting the requested
400 us wait leaves 1136 us for scheduling and initial SPI service. This is a
headroom budget, not a hardware guarantee. IRQ masking, bus stalls or BUSY can
still exceed it; full FIFO and sticky flags continue to fail the session.
The host model covers 1000 us additional wake delay and deliberately fails at
2500 us. It models continuous production during SPI and blocked time, validates
every sleep's residual, and checks normal blocked time exceeds half the session.
That is an idle opportunity test, not an ESP scheduler or watchdog measurement.

LRIQ WARN/INFO session lines now include wake_us (maximum task resume lateness
relative to the requested alarm deadline, including timer-arm overhead),
wakefifo (maximum first polled level after resume), and wakes (completed waits).
Existing service gap, FIFO maximum, sticky flags, queue/consumer and loss checks
retain their meaning. IQ bytes and continuity boundaries are unchanged.

All three capture tasks use core 0, configMAX_PRIORITIES-1. The source audit of
components, main and managed_components found no other application task at that
priority after lowering the optional top_probe diagnostic by one. ESP-IDF IPC
is an exception: its system tasks are created at the maximum priority and use
caller priority for calls under the existing ESP_IPC_USES_CALLERS_PRIORITY
configuration. No runtime task inventory or hardware timing was performed.

The acquisition loop, per-poll helpers, transport wrappers and queue sinks are
IRAM attributed. Defaults enable SPI master and timer IRAM, ISR timer dispatch
on CPU0, GPIO IRAM, and disable FreeRTOS task-function flash placement (required
by SPI_MASTER_IN_IRAM). Error logging and setup/recovery remain ordinary code.
P25 embeds its queue in its internally allocated capture context. The separate
pager adapter still allocates its 32-slot queue in PSRAM despite its comment;
that is not the P25 capture path audited here.

Size estimate from the existing build_tdp4/lakeshark.map (not rebuilt): 20248
bytes of FreeRTOS text, 566 bytes of SPI API text and 2506 bytes of existing
radio/sink text are candidates for relocation, about 22.8 KiB total, plus the
new loop/callback. These are internal-RAM costs, not equivalent app-image
growth: relocated text was already stored in the image. Each stream result
adds 16 bytes and live capture allocates one esp_timer object. Exact IRAM and
app-slot deltas require a target link/map comparison; only host builds/tests
were authorized. CPU load and overflow elimination require field validation.

## Waterfall contention follow-up (host only)

`ls_pwf_compose` writes only new lines, but `ls_tui.c:image_shift` must move
all retained pixels to keep newest-at-top scrolling. The old edge-cell repaint
then copied up to a full cell height over those pixels again. It now copies
only the exact new band. A 460x510 portrait fixture, two new rows, measures:

- Composition: 1,840 destination bytes/update.
- Framebuffer copy: 469,200 destination bytes/update (previous formula: 483,000).
- Row writeback requested: 579,360 bytes/update (568-pixel native stride).
- At ten updates/second: 4,692,000 copy B/s and 5,793,600 writeback B/s.
- CW/CCW 800x408 fixture: 652,800 copy and 908,800 writeback B/update;
  6,528,000 and 9,088,000 B/s at ten updates/second.

These are host-counted destination/request bytes, not measured external-bus
transactions: copying also reads the source, cache-line rounding may expand
writeback, and continuous panel scan/DMA traffic is additional. The writeback
count is for all dirty rows in the present. Disjoint changes no longer flush
clean intervening rows (the two-cell test flushes 34 rather than 527 rows).
`ls_tui_last_traffic` exposes the most recent image-copy and row-flush counts.

LR IQ composition uses a 100 ms minimum interval; pending lines coalesce,
including a backlog larger than the ring. Explicit appearance changes still
repaint immediately. No samples or accepted history rows are decimated.

Local IDF 5.5.4 `esp_mm/esp_cache_msync.c:s_c2m_ops` holds its cache spinlock
for each writeback chunk. The board default is reduced from 16 KiB to 1 KiB.
This bounds each operation, not total display bandwidth. Apply the new default
to the target sdkconfig when building; no target config/link or hardware run
was performed here. The panel already uses in-framebuffer row writeback;
there is no additional DPI-driver framebuffer copy on that path.

P25's producer context/embedded SPSC queue, acquisition buffers, static SPI
descriptor, and DMA packet buffer are internal. `lr_spec_feed` is consumer-side.
The local patched IDF accepts non-word-sized internal DMA transfers without
allocating alignment bounce buffers. No application panel/TUI mux is shared
with capture; cache synchronization and kernel internals remain shared.
Nested radio lock calls now increment owner-local depth; only outer acquire
and final release enter the recursive kernel mutex. The threaded radio test
still verifies command exclusion and frame non-interleaving.

LRIQ session lines add `drain_us`, `drain_spi` (bytes in the worst-time drain),
and `max_drain_spi` (maximum bytes in any drain). Timing starts before the
initial level query and ends after FIFO reads, validation and sink delivery,
including failed/partial drains; residual recheck and sleep are excluded.
SPI bytes count attempted wire bytes, including query/status headers, not
just IQ payload, using a locked transport counter with modulo subtraction.

RAM: retain all four IRAM options. FreeRTOS is the dominant cost (~20 KiB of
text in the existing map); IDF's SPI_MASTER_IN_IRAM depends on disabling
FreeRTOS flash placement. ISR sleeping does not remove SPI's kernel calls.
ESP_TIMER_IN_IRAM still covers per-wait start_once/list manipulation and ISR
dispatch; GPIO_CTRL_FUNC_IN_IRAM covers every BUSY read (all selected GPIO
helpers total 412 bytes in the existing map). Neither is safely redundant.
No RAM saving is claimed. Added raw storage: 256-byte dirty bitmap, two
4-byte traffic counters, one 4-byte SPI counter, and 16 bytes per stream
result, plus code. Exact linked RAM changes need the user's target build.
