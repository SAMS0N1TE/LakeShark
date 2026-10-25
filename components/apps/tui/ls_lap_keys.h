/* The public halves of the keys that sign store apps. Separate from the OTA
 * keys on purpose: an app-signing key that leaks cannot sign firmware.
 * The first is the store key; the second is an offline spare, kept away from
 * the machine that signs. Rotation follows ls_ota_key.h: ship a build that
 * trusts both, wait, then sign with the new one. */
#ifndef LS_LAP_KEYS_H
#define LS_LAP_KEYS_H
#include <stddef.h>

static const char *const LS_LAP_PUBLIC_KEYS[] = {
    "-----BEGIN PUBLIC KEY-----\n"
    "MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEhcLdY/ko0VVSRZBZS6Zqyulx/XZ6\n"
    "uUikDjLVlsXTKP3KDSjmUzYImMhhK4ZTLfz4LfG/wIgCKo+XGXWdCBm+6Q==\n"
    "-----END PUBLIC KEY-----\n",
    "-----BEGIN PUBLIC KEY-----\n"
    "MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAET0un5qyHmJqYYpDwdrN9XROGUxcY\n"
    "H9sXvhJVvhHkpmfDGu1f8VZxyJK4zmFBQFNcuXbWdWYdSi7+tn0+jWVyYw==\n"
    "-----END PUBLIC KEY-----\n",
};
#define LS_LAP_PUBLIC_KEY_COUNT (sizeof(LS_LAP_PUBLIC_KEYS) / sizeof(LS_LAP_PUBLIC_KEYS[0]))
#endif
