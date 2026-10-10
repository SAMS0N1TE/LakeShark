# Zstandard decoder 1.5.7

Source: https://github.com/facebook/zstd/releases/tag/v1.5.7

`zstddeclib.c` is the unmodified decoder amalgamation generated from the v1.5.7
source archive with upstream's `build/single_file_libs/combine.py`:

```
python combine.py -r ../../lib -x legacy/zstd_legacy.h -o zstddeclib.c zstddeclib-in.c
```

Distributed under the BSD license in `LICENSE`. The engine wrapper selects
only static, bufferless decode contexts, disables default heap allocation,
legacy zstd formats, assembly, tracing and runtime assertions. No compressor,
dictionary trainer, filesystem or network code is included in the IDF build.
The PC builder uses the separately installed Python `zstandard` package.

To update: regenerate from a pinned upstream release, retain the license,
repeat malformed-frame/arena tests, inspect undefined symbols for heap calls,
and remeasure host/RV32 context, stack and decoder sizes.
