# LR2021 native POCSAG receive

PAGER RECON defaults to native clock-recovered streaming on an FSK stream
capable chip, and to packet receive on SX1262. Select NATIVE STREAM, PACKET,
or SIGN STREAM in OPTIONS > RECEIVE. Native probes use 512/1200/2400 baud,
4500 Hz deviation, the snapped 11700 Hz filter, a 16-bit preamble detector
when supported, and the selected normal or complemented 32-bit frame sync.
FLEX probes retain packet receive.

For the measured inverted VHF transmitter:

```
exp pagers start 152.6 1200I 11700 4500 16 stream
```

The same command without `stream` uses the saved receive method (native on
first use). A trailing `packet` or `sign` selects the comparison method.
The optional legacy batches argument (1-3) selects packet receive.

Start choices (plan, one-frequency Hz, probe including polarity and baud,
dwell, receive method) persist atomically as `sdr-tool/pagers_v1` through
the existing deferred settings queue. Boot loads and validates the value;
start reads the RAM cache. With no arguments, `exp pagers start` restores
that choice. BW/deviation/detector and packet burst length overrides remain
per-run. Message text stays opt-in on the existing PAGER RECON/Labs displays;
the new native console path counts pages without retaining or printing text.

The driver restores the hardware-stripped sync ahead of a new transmission,
MSB first. A FIFO-loss seam does not insert a sync; the decoder hunts the
next genuine batch sync. Readout reports complete BCH-qualified batches,
pages, BCH clean/fixed/lost words, decoder stream bytes (including restored
four-byte sync prefixes), starts, restarts, read errors and chip sync counts.
The old sign path retains density-based tuning; native receive uses chip AGC.

Host validation:

```
cmake --build bench/build --target test_pocsag test_pocsag_stream test_exp_pagers test_lr20xx_proto test_field test_lora_sx1262 test_settings_schema
ctest --test-dir bench/build -R "^(test_pocsag|test_pocsag_stream|test_exp_pagers|test_lr20xx_proto|test_field|test_lora_sx1262|test_settings_schema)$"
```

Synthetic tests cover a single preamble plus ten batches at every POCSAG
baud and polarity, corrected errors and gap recovery. PAGER RECON integration
also checks clean/fixed/lost counters and saved-start restoration. Driver
checks distinguish stripped sync restoration from FIFO loss. These are host
checks; RF sensitivity and hardware audio have not been validated here.
