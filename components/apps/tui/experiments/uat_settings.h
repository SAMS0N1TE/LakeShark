/* Versioned UAT experiment settings, parsed off the UI task. */
#ifndef UAT_SETTINGS_H
#define UAT_SETTINGS_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef struct { bool stream; uint32_t rate; } uat_settings_t;
/* Strict v1 JSON object; failure leaves out unchanged. */
bool uat_settings_parse(const char *, size_t, uat_settings_t *out);
#endif
