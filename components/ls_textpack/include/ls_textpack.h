/* Long read-only text kept out of the image's string literals.

   The OTA app slot is full, and a good part of it is prose nobody reads every
   frame: console help, app descriptions, usage text. Written with these
   macros, such text still sits in the source as a plain literal, but the
   build (gen/textpack_gen.py) moves it into zstd frames (CartoCore's CZ7,
   decoded by the zstd that MAP already links) and the firmware decodes them
   into PSRAM. ls_text_warm() does all of them at console start, on the boot
   task, so no capture, audio or UI task ever pays for a decode; after that a
   lookup is an index read. A decoded frame stays for the life of the boot,
   so a pointer handed out is good forever, like a literal's.

   Adding text: write the literal inside one of these, with a new id.

     LS_TEXT(id, "text")   an expression: the text. Wherever a const char *
                           is computed: puts(LS_TEXT(...)), a return value.
     LS_TXT(id, "text")    a constant for static initialisers (tables). It is
                           a tagged value, not a pointer: whoever reads the
                           field resolves it with ls_text_str(), which passes
                           real pointers (and NULL) through unchanged. The
                           esp_console help/hint fields are resolved for you
                           (main/ls_console_text.c), and so are ls_app_doc_t's.
     LS_PRINTF / LS_FPRINTF / LS_SNPRINTF   printf-family calls, below.
     LS_PUTS(id, "text"), LS_FPUTS(id, "text", stream),
     LS_SNPUTS(id, buf, n, "text")   puts, fputs and snprintf(buf, n, "%s",
                           text): smaller at the call site than the same
                           call with LS_TEXT.

   id is a C identifier, unique across the firmware (main/, components/apps,
   components/lakeshark); the same id with the same text may appear twice,
   with different text it is a build error. The literal follows C's rules
   (adjacent literals join, \n, \xNN, \uNNNN escapes); no macros inside it.
   Nothing else to do: the next build packs it, and test_ls_textpack checks
   that what comes out equals what the compiler reads from the literal.

   Also packed by the same generator, with no change for their authors:
   the Ukrainian half of tui/ls_i18n_uk.h (ls_i18n.c, LS_I18N_PACKED) and
   the glyph bitmaps of the four font files (components/apps/CMakeLists.txt).

   Builds without the decoder (no CartoCore zstd: Nano headless) and the host
   bench compile the literal itself, unchanged. */

#ifndef LS_TEXTPACK_H
#define LS_TEXTPACK_H

#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One frame as the generator wrote it: CZ7 (CartoCore's zstd container), or
   the text itself when that was no bigger (size == raw_len). */
typedef struct {
    const uint8_t *data;
    uint32_t size;
    uint32_t raw_len;
} ls_text_frame_t;

/* The decoded frame, decoding it on the first call (PSRAM, kept). `slot` is
   the frame's cache pointer, NULL until then. NULL only when memory or the
   frame is bad. Safe from any task; two first calls at once decode twice and
   keep one. */
const char *ls_text_frame(const ls_text_frame_t *f, const char **slot);

/* Decode a frame straight into `dst` (exactly raw_len bytes), keeping
   nothing else: for tables that live in a PSRAM array of their own, like the
   glyph bitmaps (gen/textpack_gen.py fonts). false if it did not fit or
   could not be decoded. */
int ls_text_unpack_into(const ls_text_frame_t *f, void *dst, uint32_t n);

/* Text `id` (LS_TXT_<name>), "" for an id that does not exist. */
const char *ls_text_get(unsigned id);

/* How many LS_TEXT frames are decoded right now, and the bytes they hold. */
unsigned ls_text_decoded(unsigned *bytes);

/* Decode every LS_TEXT frame now. The console start calls it at boot, so no
   capture, audio or UI task ever pays for a first decode (a message printed
   from such a task would otherwise be the one to decode its frame). */
void ls_text_warm(void);

/* Values below this are LS_TXT tags: nothing is mapped there on the chip or
   on a host, so no real string can live at such an address. */
#define LS_TEXT_TAG_SPAN 4096u

/* printf-family calls with the format in a pack. The literal is still checked
   against the arguments (-Wformat), in a dead call the compiler drops along
   with the literal; the live call takes the packed copy. At least one
   argument after the format.

     LS_PRINTF(id, "fmt", ...)            printf
     LS_FPRINTF(id, stream, "fmt", ...)   fprintf
     LS_SNPRINTF(id, buf, n, "fmt", ...)  snprintf
*/
#if defined(LS_TEXT_PACKED) && LS_TEXT_PACKED

#include "ls_textpack_ids.h"

#define LS_TEXT(id, lit) ls_text_get(LS_TXT_##id)
#define LS_TXT(id, lit)  ((const char *)(uintptr_t)(LS_TXT_##id + 1u))
/* printf, fprintf and snprintf with the format looked up by id inside: the
   call site hands its arguments straight on instead of keeping them live
   across a lookup. Use them through the macros below. */
int ls_text_printf(unsigned id, ...);
int ls_text_fprintf(unsigned id, FILE *f, ...);
int ls_text_snprintf(unsigned id, char *buf, size_t n, ...);

#define LS_PRINTF(id, fmt, ...) \
    ((void)(0 && printf(fmt, __VA_ARGS__)), ls_text_printf(LS_TXT_##id, __VA_ARGS__))
#define LS_FPRINTF(id, f, fmt, ...) \
    ((void)(0 && fprintf(f, fmt, __VA_ARGS__)), ls_text_fprintf(LS_TXT_##id, f, __VA_ARGS__))
#define LS_SNPRINTF(id, b, n, fmt, ...) \
    ((void)(0 && snprintf(b, n, fmt, __VA_ARGS__)), ls_text_snprintf(LS_TXT_##id, b, n, __VA_ARGS__))

/* puts, fputs and snprintf(b, n, "%s", text) of a packed text, the same way. */
int ls_text_puts(unsigned id);
int ls_text_fputs(unsigned id, FILE *f);
int ls_text_snputs(unsigned id, char *buf, size_t n);

#define LS_PUTS(id, lit)         ls_text_puts(LS_TXT_##id)
#define LS_FPUTS(id, lit, f)     ls_text_fputs(LS_TXT_##id, f)
#define LS_SNPUTS(id, b, n, lit) ls_text_snputs(LS_TXT_##id, b, n)

/* A field that may hold an LS_TXT tag, as text. */
static inline const char *ls_text_str(const char *p)
{
    const uintptr_t v = (uintptr_t)p;
    return (v - 1u < LS_TEXT_TAG_SPAN) ? ls_text_get((unsigned)(v - 1u)) : p;
}

#else

#define LS_TEXT(id, lit) (lit)
#define LS_TXT(id, lit)  (lit)
#define LS_PRINTF(id, fmt, ...)         printf(fmt, __VA_ARGS__)
#define LS_FPRINTF(id, f, fmt, ...)     fprintf(f, fmt, __VA_ARGS__)
#define LS_SNPRINTF(id, b, n, fmt, ...) snprintf(b, n, fmt, __VA_ARGS__)
#define LS_PUTS(id, lit)                puts(lit)
#define LS_FPUTS(id, lit, f)            fputs(lit, f)
#define LS_SNPUTS(id, b, n, lit)        snprintf(b, n, "%s", lit)

static inline const char *ls_text_str(const char *p) { return p; }

#endif

#ifdef __cplusplus
}
#endif

#endif /* LS_TEXTPACK_H */
