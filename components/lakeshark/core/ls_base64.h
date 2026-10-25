/* Strict, bounded console base64 codec, shared by sd put and native app upload. */
#ifndef LS_BASE64_H
#define LS_BASE64_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
bool ls_base64_decode(const char *text, uint8_t *out, size_t cap, size_t *written);
#ifdef __cplusplus
}
#endif
#endif
