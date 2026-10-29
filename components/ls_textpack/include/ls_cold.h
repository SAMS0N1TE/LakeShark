/* LS_COLD: a function that runs rarely, in a file the size build compiles
   -O2 for its hot loops.

   The size build (CONFIG_COMPILER_OPTIMIZATION_SIZE) keeps the DSP, radio,
   audio, renderer and MAP files at -O2 (components/lakeshark and
   components/apps CMakeLists.txt). Most of those files also hold code that
   runs once or on a key press: set-up and teardown, console commands and
   their printers, OPTIONS handlers, parsers of settings files, error paths.
   Marked LS_COLD, such a function is compiled for size, as the rest of the
   firmware is, and placed with the other unlikely code; the hot functions
   around it keep -O2. GCC also treats a call to it as the unlikely branch.

   Never on a function that runs per sample, per buffer, per cell or per
   frame, nor on anything such a function calls.

   Lives here because every firmware component and the host bench already
   have this include directory; it has nothing to do with text packing.
   Host builds and the -O2 (performance) build get an empty macro. */

#ifndef LS_COLD_H
#define LS_COLD_H

#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif

/* LS_COLD_FORCE=1 turns it on in a host build too, to measure what it costs
   the hot paths there (bench test_tui_render_cost). */
#if defined(__GNUC__) && !defined(__clang__) && \
    ((defined(CONFIG_COMPILER_OPTIMIZATION_SIZE) && CONFIG_COMPILER_OPTIMIZATION_SIZE) || \
     (defined(LS_COLD_FORCE) && LS_COLD_FORCE))
#define LS_COLD __attribute__((cold, optimize("Os")))
/* Every function defined between these two is compiled for size: for a run
   of cold code, such as a console command block or an included worker. */
#define LS_COLD_BEGIN _Pragma("GCC push_options") _Pragma("GCC optimize(\"Os\")")
#define LS_COLD_END   _Pragma("GCC pop_options")
#else
#define LS_COLD
#define LS_COLD_BEGIN
#define LS_COLD_END
#endif

#endif /* LS_COLD_H */
