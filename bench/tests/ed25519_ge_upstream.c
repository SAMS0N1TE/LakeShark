/* upstream ge.c as the firmware builds it: its ge_scalarmult_base renamed so
   components/meshcore/ls_ed25519_base.c provides the real one. The renamed
   original stays callable here, for test_ed25519_base to compare against. */
#define ge_scalarmult_base ls_upstream_ge_scalarmult_base
#include "ge.c"
