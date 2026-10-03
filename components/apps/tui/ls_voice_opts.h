/* The spoken side of the unit as OPTIONS lists: the voice, its level, the
   speaker, a test, and what each app says aloud. One set of rows, reached
   from the app that talks (ADS-B, MESH) and from Settings. */
#ifndef LS_VOICE_OPTS_H
#define LS_VOICE_OPTS_H

#include "ls_options.h"

#ifdef __cplusplus
extern "C" {
#endif

/* What ADS-B calls out, with the voice rows: ADS-B > VOICE. */
extern const ls_opt_ctx_t ls_voice_ctx_adsb;
/* Which mesh messages are read out, with the voice rows: MESH > VOICE. */
extern const ls_opt_ctx_t ls_voice_ctx_mesh;
/* Settings > Voice: the voice rows, and each app's callouts a level down. */
extern const ls_opt_ctx_t ls_voice_ctx_all;

#ifdef __cplusplus
}
#endif

#endif /* LS_VOICE_OPTS_H */
