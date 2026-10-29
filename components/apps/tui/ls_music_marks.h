#ifndef LS_MUSIC_MARKS_H
#define LS_MUSIC_MARKS_H
#include <stdbool.h>
#include <stdint.h>
typedef struct ls_music_marks ls_music_marks;
ls_music_marks *ls_music_marks_new(const char *directory);
void ls_music_marks_free(ls_music_marks *m);
/* Prepare only after the preceding decoder has parked. No file IO here. */
bool ls_music_marks_prepare(ls_music_marks *m, const char *source);
bool ls_music_marks_add(ls_music_marks *m, uint32_t at_ms);
void ls_music_marks_clear(ls_music_marks *m);
unsigned ls_music_marks_count(const ls_music_marks *m);
bool ls_music_marks_get(const ls_music_marks *m, unsigned index,
                        uint32_t *at_ms);
/* Worker only. Load/save small per-file sidecars. False means an IO error. */
bool ls_music_marks_service(ls_music_marks *m);
#endif
