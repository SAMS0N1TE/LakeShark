#include "ls_music_marks.h"
#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#define MARK_ALLOC(n)                                                          \
  heap_caps_calloc(1, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define MARK_FREE(p) heap_caps_free(p)
#else
#define MARK_ALLOC(n) calloc(1, n)
#define MARK_FREE(p) free(p)
#endif
#ifdef _WIN32
#include <direct.h>
#define MAKE_DIR(p) _mkdir(p)
#else
#define MAKE_DIR(p) mkdir(p, 0777)
#endif
#define MARKS 32
enum { LOAD = 1, ADD = 2, CLEAR = 4, SAVE = 8 };
struct ls_music_marks {
  char root[256], source[512], sidecar[320], io[1024], stdio_buffer[1024];
  atomic_uint count, pending, at, marks[MARKS];
  uint64_t hash, size, mtime;
};
ls_music_marks *ls_music_marks_new(const char *directory) {
  if (!directory || strlen(directory) >= 256)
    return NULL;
  ls_music_marks *m = MARK_ALLOC(sizeof(*m));
  if (m)
    snprintf(m->root, sizeof(m->root), "%s", directory);
  return m;
}
void ls_music_marks_free(ls_music_marks *m) { MARK_FREE(m); }
bool ls_music_marks_prepare(ls_music_marks *m, const char *source) {
  if (!m || !source || strlen(source) >= sizeof(m->source))
    return false;
  snprintf(m->source, sizeof(m->source), "%s", source);
  atomic_store(&m->count, 0);
  atomic_store(&m->pending, LOAD);
  return true;
}
bool ls_music_marks_add(ls_music_marks *m, uint32_t at_ms) {
  if (!m || !m->source[0] || atomic_load(&m->count) >= MARKS)
    return false;
  atomic_store(&m->at, at_ms);
  atomic_fetch_or(&m->pending, ADD);
  return true;
}
void ls_music_marks_clear(ls_music_marks *m) {
  if (m)
    atomic_fetch_or(&m->pending, CLEAR);
}
unsigned ls_music_marks_count(const ls_music_marks *m) {
  return m ? atomic_load(&m->count) : 0;
}
bool ls_music_marks_get(const ls_music_marks *m, unsigned index,
                        uint32_t *at_ms) {
  if (!m || !at_ms || index >= atomic_load(&m->count))
    return false;
  *at_ms = atomic_load(&m->marks[index]);
  return true;
}
static void make_parents(ls_music_marks *m) {
  snprintf(m->io, sizeof(m->io), "%s", m->root);
  for (char *c = m->io + 1; *c; c++)
    if (*c == '/' || *c == '\\') {
      char old = *c;
      *c = 0;
      MAKE_DIR(m->io);
      *c = old;
    }
  MAKE_DIR(m->io);
}
static void load(ls_music_marks *m) {
  m->hash = UINT64_C(14695981039346656037);
  for (const unsigned char *c = (const unsigned char *)m->source; *c; c++) {
    m->hash ^= *c;
    m->hash *= UINT64_C(1099511628211);
  }
  struct stat st;
  bool exists = stat(m->source, &st) == 0;
  m->size = exists ? (uint64_t)st.st_size : 0;
  m->mtime = exists ? (uint64_t)st.st_mtime : 0;
  snprintf(m->sidecar, sizeof(m->sidecar), "%s/%016" PRIx64 ".marks", m->root,
           m->hash);
  atomic_store(&m->count, 0);
  FILE *f = fopen(m->sidecar, "rb");
  if (!f)
    return;
  setvbuf(f, m->stdio_buffer, _IOFBF, sizeof(m->stdio_buffer));
  uint64_t hash = 0, size = 0, mtime = 0;
  bool valid = fgets(m->io, sizeof(m->io), f) &&
               sscanf(m->io, "LSMARK1 %" SCNx64 " %" SCNu64 " %" SCNu64, &hash,
                      &size, &mtime) == 3 &&
               hash == m->hash && size == m->size && mtime == m->mtime;
  unsigned count = 0;
  uint32_t last = 0;
  while (valid && count < MARKS && fgets(m->io, sizeof(m->io), f)) {
    char *end;
    unsigned long at = strtoul(m->io, &end, 10);
    if (end == m->io || at > UINT32_MAX || (count && at <= last)) {
      valid = false;
      break;
    }
    last = (uint32_t)at;
    atomic_store(&m->marks[count++], last);
  }
  fclose(f);
  if (valid)
    atomic_store(&m->count, count);
}
static bool save(ls_music_marks *m) {
  make_parents(m);
  FILE *f = fopen(m->sidecar, "wb");
  if (!f)
    return false;
  setvbuf(f, m->stdio_buffer, _IOFBF, sizeof(m->stdio_buffer));
  bool ok = fprintf(f, "LSMARK1 %016" PRIx64 " %" PRIu64 " %" PRIu64 "\n",
                    m->hash, m->size, m->mtime) > 0;
  unsigned count = atomic_load(&m->count);
  for (unsigned i = 0; i < count && ok; i++)
    ok = fprintf(f, "%" PRIu32 "\n", (uint32_t)atomic_load(&m->marks[i])) > 0;
  if (fflush(f))
    ok = false;
  if (fclose(f))
    ok = false;
  return ok;
}
bool ls_music_marks_service(ls_music_marks *m) {
  if (!m)
    return false;
  unsigned pending = atomic_exchange(&m->pending, 0);
  if (pending & LOAD)
    load(m);
  if (pending & CLEAR)
    atomic_store(&m->count, 0);
  if (pending & ADD) {
    unsigned count = atomic_load(&m->count), pos = 0;
    uint32_t at = atomic_load(&m->at);
    while (pos < count && atomic_load(&m->marks[pos]) < at)
      pos++;
    if (count < MARKS && (pos == count || atomic_load(&m->marks[pos]) != at)) {
      for (unsigned j = count; j > pos; j--)
        atomic_store(&m->marks[j], atomic_load(&m->marks[j - 1]));
      atomic_store(&m->marks[pos], at);
      atomic_store(&m->count, count + 1);
    }
  }
  if (pending & (ADD | CLEAR | SAVE)) {
    if (save(m))
      return true;
    atomic_fetch_or(&m->pending, SAVE);
    return false;
  }
  return true;
}
