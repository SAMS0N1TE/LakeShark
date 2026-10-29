/* Text packs (components/ls_textpack): what the firmware decodes is exactly what
   the compiler made of the literal the author wrote.

   ls_textpack_src.c is generated next to the pack from the very same tokens,
   copied as written, so the C compiler - not the generator - says what each
   literal means. This build links CartoCore's zstd decoder, as the
   T-Display's does, so every frame goes through the real decode path. */

#include "ls_test.h"

#include <stdint.h>
#include <string.h>

#include "ls_textpack.h"

extern const char *const ls_text_src[];
extern const char *const ls_text_names[];
extern const unsigned ls_text_count;
extern const ls_text_frame_t ls_text_frames[];
extern const unsigned ls_text_nframes;

LS_CASE(every_packed_text_equals_its_literal)
{
    LS_CHECK(ls_text_count == LS_TEXT_COUNT);
    LS_CHECK(ls_text_count > 0);
    for (unsigned i = 0; i < ls_text_count; i++)
        LS_CHECK_MSG(strcmp(ls_text_get(i), ls_text_src[i]) == 0,
                     "%s decodes to [%s], source says [%s]", ls_text_names[i],
                     ls_text_get(i), ls_text_src[i]);
}

LS_CASE(frames_are_compressed_and_fit_cartocore)
{
    unsigned packed = 0;
    for (unsigned f = 0; f < ls_text_nframes; f++) {
        LS_CHECK(ls_text_frames[f].raw_len <= 65536u);
        if (ls_text_frames[f].size < ls_text_frames[f].raw_len) {
            LS_CHECK(memcmp(ls_text_frames[f].data, "CZ7", 4) == 0);
            packed++;
        }
    }
    LS_CHECK(packed > 0);
}

LS_CASE(a_decoded_frame_is_kept_and_pointers_stay)
{
    const char *a = ls_text_get(0);
    unsigned bytes = 0;
    LS_CHECK(ls_text_decoded(&bytes) >= 1);
    LS_CHECK(bytes > 0);
    for (int k = 0; k < 5; k++) LS_CHECK(ls_text_get(0) == a);
    /* the same frame is not decoded again for another text in it */
    const unsigned before = ls_text_decoded(NULL);
    for (unsigned i = 0; i < ls_text_count; i++) (void)ls_text_get(i);
    LS_CHECK(ls_text_decoded(NULL) <= ls_text_nframes);
    LS_CHECK(ls_text_decoded(NULL) >= before);
    for (unsigned i = 0; i < ls_text_count; i++) {
        const char *p = ls_text_get(i);
        LS_CHECK(ls_text_get(i) == p);
    }
}

LS_CASE(warm_decodes_every_compressed_frame)
{
    unsigned compressed = 0, raw = 0;
    for (unsigned f = 0; f < ls_text_nframes; f++)
        if (ls_text_frames[f].size != ls_text_frames[f].raw_len) {
            compressed++;
            raw += ls_text_frames[f].raw_len;
        }
    ls_text_warm();
    unsigned bytes = 0;
    LS_EQ_UINT(ls_text_decoded(&bytes), compressed);
    LS_EQ_UINT(bytes, raw);
}

LS_CASE(out_of_range_ids_are_empty_never_null)
{
    LS_CHECK(ls_text_get(ls_text_count) != NULL);
    LS_EQ_STR(ls_text_get(ls_text_count), "");
    LS_EQ_STR(ls_text_get(0xFFFFFFFFu), "");
}

/* The table form: a tag resolves to the text, real pointers and NULL pass.
   This file is scanned along with the firmware (bench/CMakeLists.txt), so
   its own ids are in the pack; the escapes are the ones C reads. */
static const struct { const char *help; const char *hint; } k_table[] = {
    { LS_TXT(test_ls_text_table, "tab\there, \"quoted\", back\\slash, \x41\102C, "
                                 "bell\a? no: \x7e, octal \0101, ук ok, "
                                 "Українська °C"),
      "a plain literal" },
    { NULL, NULL },
};

LS_CASE(tags_resolve_and_pointers_pass_through)
{
    LS_CHECK((uintptr_t)k_table[0].help < LS_TEXT_TAG_SPAN);
    LS_EQ_STR(ls_text_str(k_table[0].help),
              "tab\there, \"quoted\", back\\slash, ABC, bell\a? no: ~, octal \0101, "
              "ук ok, Українська °C");
    LS_CHECK(ls_text_str(k_table[0].hint) == k_table[0].hint);
    LS_CHECK(ls_text_str(NULL) == NULL);
    LS_CHECK(ls_text_str(k_table[1].help) == NULL);
    LS_EQ_STR(LS_TEXT(test_ls_text_expr, "an expression, decoded on the spot"),
              "an expression, decoded on the spot");
}
