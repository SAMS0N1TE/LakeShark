/* LS_TEST_SOURCES: ${APP}/rec/rec_storage_label.c */
/**/
/* Before this fix, AppREC::refreshFiles printed "%d files on SPIFFS" every
   time it redrew the FILES tab, but rec_dir() prefers /sdcard/lakeshark
   whenever an SD card is mounted at boot.  The visible label therefore
   contradicted where SAVE, list and DELETE were actually operating, and
   any troubleshooting off that label went to the wrong medium.

   The classifier and the note formatter live in their own file so this
   test can pin every case the app can present without a display, a card
   or a radio.  The AppREC side is only a one-line snprintf call now, so
   nobody can drift the phrasing of one backend out of sync with the
   other. */

#include "ls_test.h"
#include "rec_storage_label.h"

#include <string.h>

LS_CASE(sd_root_reports_sd)
{
    /* rec_dir() returns "/sdcard/lakeshark" when the card is present -
       the exact string BSP_SD_MOUNT_POINT "/lakeshark" resolves to. */
    LS_EQ_STR(rec_storage_label("/sdcard/lakeshark"), "SD");
}

LS_CASE(sd_bare_mount_also_reports_sd)
{
    /* Defensive: rec_dir() could reasonably decide to write straight into
       the SD root in a future revision.  The label must not fold that
       into a raw-path fallback and stop saying "SD". */
    LS_EQ_STR(rec_storage_label("/sdcard"), "SD");
}

LS_CASE(spiffs_root_reports_spiffs)
{
    /* The fallback path when the card is absent - what refreshFiles used
       to hard-code for every case.  BSP_SPIFFS_MOUNT_POINT is /spiffs. */
    LS_EQ_STR(rec_storage_label("/spiffs"), "SPIFFS");
}

LS_CASE(spiffs_subpath_still_reports_spiffs)
{
    /* Symmetry with the SD case: if the SPIFFS layout ever grows a
       subdir the label must not stop recognising it. */
    LS_EQ_STR(rec_storage_label("/spiffs/rec"), "SPIFFS");
}

LS_CASE(null_and_empty_are_not_labelled_as_a_backend)
{
    /* rec_dir() cannot legitimately return either of these, but if it
       ever did the FILES tab must not silently claim SD or SPIFFS
       ownership of files that live neither place. */
    LS_EQ_STR(rec_storage_label(NULL), "?");
    LS_EQ_STR(rec_storage_label(""), "?");
}

LS_CASE(unknown_backend_returns_the_raw_path)
{

    LS_EQ_STR(rec_storage_label("/nvs/capture"), "/nvs/capture");
}

LS_CASE(note_singular_sd_reads_naturally)
{
    /* The done-when case for the SD path: one capture on the card, and
       the tab must say "on SD" instead of "on SPIFFS". */
    char b[64];
    int w = rec_files_note(b, sizeof(b), "/sdcard/lakeshark", 1, 0);
    LS_CHECK(w > 0);
    LS_EQ_STR(b, "1 file on SD");
}

LS_CASE(note_plural_sd)
{
    /* Plural agreement is easy to break in a printf that grows a case;
       pin both sides so nobody says "1 files" or "3 file". */
    char b[64];
    rec_files_note(b, sizeof(b), "/sdcard/lakeshark", 3, 0);
    LS_EQ_STR(b, "3 files on SD");
}

LS_CASE(note_zero_files_still_names_the_storage)
{
    /* Empty directory is the state right after boot; the storage
       label is what tells the user whether their old captures ought
       to be somewhere else.  "0 files on SPIFFS" was correct only when
       the card was absent, so proving the format survives n=0 also
       proves the fix does not lie in the empty case. */
    char b[64];
    rec_files_note(b, sizeof(b), "/spiffs", 0, 0);
    LS_EQ_STR(b, "0 files on SPIFFS");
}

LS_CASE(note_singular_spiffs)
{
    /* The done-when case for the fallback: same tab, no card, one file
       on flash.  Must NOT say SD here - that is the mirror mistake of
       the one this task fixes. */
    char b[64];
    rec_files_note(b, sizeof(b), "/spiffs", 1, 0);
    LS_EQ_STR(b, "1 file on SPIFFS");
}

LS_CASE(note_appends_truncation_hint_when_asked)
{
    /* refreshFiles walks a fixed-size table of FILES_MAX rows; when
       rec_list() returned more names than the tab can show, the footer
       has to say so.  Pin the exact phrasing because the row buttons
       operate only on visible names, and a quietly truncated list is
       exactly the kind of "why can I not delete this?" bug that led
       to . */
    char b[64];
    rec_files_note(b, sizeof(b), "/sdcard/lakeshark", 40, 1);
    LS_EQ_STR(b, "40 files on SD (list truncated)");
}

LS_CASE(note_truncation_appears_on_spiffs_too)
{
    /* Same invariant on the fallback backend.  Before the label fix
       the whole footer was "N files on SPIFFS[ (list truncated)]"; the
       new formatter must still handle both suffixes together. */
    char b[64];
    rec_files_note(b, sizeof(b), "/spiffs", 40, 1);
    LS_EQ_STR(b, "40 files on SPIFFS (list truncated)");
}

LS_CASE(note_survives_a_short_buffer_without_writing_past_it)
{
    /* refreshFiles owns a 64-byte stack buffer, but a defensive
       formatter must still not touch beyond `len`.  Prove snprintf
       semantics survived the wrapper: buffer stays nul-terminated at
       len-1 and the return value reflects the would-be length. */
    char b[8];
    memset(b, 0x5A, sizeof(b));
    int w = rec_files_note(b, sizeof(b), "/spiffs", 12, 0);
    LS_CHECK(w > 0);
    LS_CHECK(b[sizeof(b) - 1] == '\0');
    /* First few chars are the truncated prefix "12 file" - not the
       whole note, but definitely not "on SPIFFS" pretending to have
       fit. */
    LS_CHECK(strncmp(b, "12 file", 7) == 0);
}

LS_CASE(note_rejects_null_output_buffer)
{

    LS_EQ_INT(rec_files_note(NULL, 32, "/spiffs", 1, 0), 0);
    char b[16];
    LS_EQ_INT(rec_files_note(b, 0, "/spiffs", 1, 0), 0);
}
