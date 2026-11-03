#ifndef LS_LAP_FEATURES_H
#define LS_LAP_FEATURES_H

/* sdkconfig leaves disabled booleans undefined. Host builds default to the
 * full SDK, but can set each CONFIG value to 0 to exercise the board stubs. */
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif
#if defined(CONFIG_LS_LAP_WORKER_DSP)
#define LAP_WORKER_DSP CONFIG_LS_LAP_WORKER_DSP
#elif defined(ESP_PLATFORM) && !defined(LS_LAP_HOST_TEST)
#define LAP_WORKER_DSP 0
#else
#define LAP_WORKER_DSP 1
#endif
#if defined(CONFIG_LS_LAP_HACKRF_TX)
#define LAP_HACKRF_TX CONFIG_LS_LAP_HACKRF_TX
#elif defined(ESP_PLATFORM) && !defined(LS_LAP_HOST_TEST)
#define LAP_HACKRF_TX 0
#else
#define LAP_HACKRF_TX 1
#endif
#if defined(CONFIG_LS_LAP_NFC)
#define LAP_NFC CONFIG_LS_LAP_NFC
#elif defined(ESP_PLATFORM) && !defined(LS_LAP_HOST_TEST)
#define LAP_NFC 0
#else
#define LAP_NFC 1
#endif
#if defined(CONFIG_LS_LAP_MDNS)
#define LAP_MDNS CONFIG_LS_LAP_MDNS
#elif defined(ESP_PLATFORM) && !defined(LS_LAP_HOST_TEST)
#define LAP_MDNS 0
#else
#define LAP_MDNS 1
#endif

#endif
