#ifndef MESH_PHRASE_H
#define MESH_PHRASE_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* What the voice says for an incoming MeshCore message ("name: message"):
   "MESSAGE FROM NAME." or "DIRECT MESSAGE FROM NAME.", and with full set,
   the message after it. Anything the voice cannot say becomes a pause. */
void mesh_phrase(char *out, size_t n, const char *text, bool direct, bool full);

#ifdef __cplusplus
}
#endif

#endif
