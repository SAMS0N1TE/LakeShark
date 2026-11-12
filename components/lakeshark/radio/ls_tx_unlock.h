#ifndef LS_TX_UNLOCK_H
#define LS_TX_UNLOCK_H
#include "sdkconfig.h"
#ifndef LS_TX_UNLOCKED
#if defined(CONFIG_LS_TX_UNLOCKED) && CONFIG_LS_TX_UNLOCKED
#define LS_TX_UNLOCKED 1
#else
#define LS_TX_UNLOCKED 0
#endif
#endif
#endif
