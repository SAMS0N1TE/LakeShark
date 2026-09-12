/* The smallest Arduino.h that satisfies vendored MeshCore.

   LS-995  Exactly one upstream file reaches for this:
   `helpers/AdvertDataHelpers.cpp`, and only for
   `AdvertTimeHelper::formatRelativeTimeDiff`, which calls `sprintf` and
   nothing else. Checked the whole file - no String, no millis(), no Serial,
   no F().

   So this is not an Arduino compatibility layer and must not grow into one.
   It is four standard headers behind the name upstream happens to use, which
   keeps `upstream/` byte-identical. The moment something here needs a real
   Arduino type, that is the signal to implement it deliberately in
   `ls_mesh.cpp` rather than to widen this file - the same rule Stream.h,
   AES.h and SHA256.h are written to. */
#ifndef LS_COMPAT_ARDUINO_H
#define LS_COMPAT_ARDUINO_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#endif /* LS_COMPAT_ARDUINO_H */
