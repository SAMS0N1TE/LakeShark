/* LS_TEST_SOURCES: ${FW}/components/apps/tui/ls_app_docs.c
 *
 * The app contract, enforced.
 *
 * Every app answers three questions where it is declared: what it is for,
 * what it keeps, and what it does with a position. Before this, an app's
 * whole self-description was a one-word tagline and a line of key hints, and
 * the [?] overlay said the same nine global keys in every app. The point of
 * a test rather than a convention is that a convention is what the
 * twenty-first app quietly skips.
 *
 * ls_app_register() refuses an incomplete descriptor at runtime. This is the
 * half that runs in the build, and it also checks the things a null-pointer
 * check cannot: that the prose is prose, and that an app claiming to keep
 * something says what. */

#include "ls_test.h"

#include "ls_app_docs.h"

#include <string.h>

/* Long enough that "todo", "n/a" and a copied tagline all fail. The shortest
   real purpose here is HOME's, comfortably over this. */
#define PURPOSE_MIN 40
#define NOTE_MIN    20

static bool ends_with_full_stop(const char *s)
{
    const size_t n = strlen(s);
    return n && s[n - 1] == '.';
}

LS_CASE(every_app_says_what_it_is_for)
{
    LS_CHECK(ls_app_docs_count > 0);
    for (int i = 0; i < ls_app_docs_count; i++) {
        const ls_app_doc_row_t *r = &ls_app_docs_all[i];
        LS_CHECK_MSG(r->id && r->id[0], "a doc row has no app id");
        LS_CHECK_MSG(r->doc != NULL, r->id);
        LS_CHECK_MSG(r->doc->purpose && r->doc->purpose[0], r->id);
        LS_CHECK_MSG(strlen(r->doc->purpose) >= PURPOSE_MIN, r->id);
        /* A sentence, not a label. The overlay wraps it as prose. */
        LS_CHECK_MSG(ends_with_full_stop(r->doc->purpose), r->id);
    }
}

LS_CASE(an_app_that_keeps_something_says_what_it_keeps)
{
    for (int i = 0; i < ls_app_docs_count; i++) {
        const ls_app_doc_row_t *r = &ls_app_docs_all[i];
        if (r->doc->records == LS_APP_RECORDS_NOTHING) {
            /* The inverse matters too: an app that keeps nothing must not
               carry a note describing what it keeps, or the two disagree and
               the overlay prints the wrong one. */
            LS_CHECK_MSG(r->doc->record_note == NULL, r->id);
            continue;
        }
        LS_CHECK_MSG(r->doc->record_note && r->doc->record_note[0], r->id);
        LS_CHECK_MSG(strlen(r->doc->record_note) >= NOTE_MIN, r->id);
    }
}

LS_CASE(an_app_that_uses_a_position_says_what_for)
{
    for (int i = 0; i < ls_app_docs_count; i++) {
        const ls_app_doc_row_t *r = &ls_app_docs_all[i];
        if (r->doc->gps == LS_APP_GPS_UNUSED) {
            LS_CHECK_MSG(r->doc->gps_note == NULL, r->id);
            continue;
        }
        LS_CHECK_MSG(r->doc->gps_note && r->doc->gps_note[0], r->id);
        LS_CHECK_MSG(strlen(r->doc->gps_note) >= NOTE_MIN, r->id);
    }
}

LS_CASE(app_ids_are_unique_and_lower_case)
{
    for (int i = 0; i < ls_app_docs_count; i++) {
        /* Lower case and digits: "p25" is an app id, not a typo. No spaces
           or capitals, because the id is what a console command and a saved
           setting use. */
        for (const char *c = ls_app_docs_all[i].id; *c; c++)
            LS_CHECK_MSG((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9'),
                         ls_app_docs_all[i].id);
        for (int j = i + 1; j < ls_app_docs_count; j++)
            LS_CHECK_MSG(strcmp(ls_app_docs_all[i].id,
                                ls_app_docs_all[j].id) != 0,
                         ls_app_docs_all[i].id);
    }
}

/* The four apps below are the ones a reader is most likely to assume record
   something, because their whole subject is an observation. They do not. The
   assertions exist so that the day one of them starts recording, this test
   fails and the contract is updated in the same commit - rather than the
   overlay quietly telling an operator that P25 keeps nothing while it fills
   the card. */
LS_CASE(the_receivers_that_keep_nothing_are_pinned_as_keeping_nothing)
{
    static const char *const silent[] = { "fm", "adsb", "falls" };
    for (unsigned i = 0; i < sizeof(silent) / sizeof(silent[0]); i++) {
        const ls_app_doc_t *d = NULL;
        for (int j = 0; j < ls_app_docs_count; j++)
            if (!strcmp(ls_app_docs_all[j].id, silent[i]))
                d = ls_app_docs_all[j].doc;
        LS_CHECK_MSG(d != NULL, silent[i]);
        LS_CHECK_MSG(d->records == LS_APP_RECORDS_NOTHING, silent[i]);
    }
}

LS_CASE(everything_that_keeps_an_observation_carries_a_position)
{
    /* The journal stamps a fix onto every entry already, so an app that
       records at all gets position for free. If one ever records without
       claiming a position, that is either a mistake or a deliberate
       exception - and it should have to be argued for here. */
    for (int i = 0; i < ls_app_docs_count; i++) {
        const ls_app_doc_row_t *r = &ls_app_docs_all[i];
        if (r->doc->records == LS_APP_RECORDS_NOTHING) continue;
        LS_CHECK_MSG(r->doc->gps != LS_APP_GPS_UNUSED, r->id);
    }
}
