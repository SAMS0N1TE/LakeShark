#ifndef LS_TX_UNLOCK_H
#define LS_TX_UNLOCK_H
#include <stdbool.h>
#include "sdkconfig.h"

/* Two switches with one meaning: every software transmit limit is off.

   LS_TX_UNLOCKED_BUILD is the lab build (CONFIG_LS_TX_UNLOCKED, or
   -DLS_TX_UNLOCKED=1 on the host bench). It is a constant, so it is what
   sizes buffers, compiles the bench probes and sets the HF RF switch bits.

   LS_TX_UNLOCKED is what the limits ask. On the board it is the build OR the
   operator's own switch, SUB-GHZ SETTINGS > TX limits, kept in NVS
   (settings_set_tx_unlocked): power and duty like a HackRF, without
   flashing a different image. The hardware's own ceilings stay
   (boards/tx_unlocked.txt, "What is still enforced"). On the host it stays
   the constant, so the bench builds as it always has. */
#ifndef LS_TX_UNLOCKED_BUILD
#if defined(LS_TX_UNLOCKED)
#define LS_TX_UNLOCKED_BUILD (LS_TX_UNLOCKED)
#elif defined(CONFIG_LS_TX_UNLOCKED) && CONFIG_LS_TX_UNLOCKED
#define LS_TX_UNLOCKED_BUILD 1
#else
#define LS_TX_UNLOCKED_BUILD 0
#endif
#endif

#ifndef LS_TX_UNLOCKED
#ifdef ESP_PLATFORM
extern volatile bool ls_tx_unlock_runtime;
#define LS_TX_UNLOCKED (LS_TX_UNLOCKED_BUILD || ls_tx_unlock_runtime)
#else
#define LS_TX_UNLOCKED LS_TX_UNLOCKED_BUILD
#endif
#endif

#endif
