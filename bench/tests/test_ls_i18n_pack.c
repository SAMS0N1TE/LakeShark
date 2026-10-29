/* The Ukrainian table as the T-Display builds it (LS_I18N_PACKED): English
   keys as literals, Ukrainian decoded from zstd frames. Every entry must be
   the one ls_i18n_uk.h holds, in the same order, and a lookup must give what
   the plain table gives. (test_ui_language_packed runs the existing language
   suite against this same build.) */

#include "ls_test.h"

#include <string.h>

#include "ls_i18n.h"
#include "tui_core.h"

typedef struct {
    const char *en;
    const char *uk;
} ls_i18n_pair_t;
#include "ls_i18n_uk.h"

#define N_SRC ((int)(sizeof(UK) / sizeof(UK[0])))

LS_CASE(packed_table_is_the_source_table)
{
    LS_EQ_INT(ls_i18n_entries(), N_SRC);
    for (int i = 0; i < N_SRC; i++) {
        LS_EQ_STR(ls_i18n_entry_en(i), UK[i].en);
        LS_EQ_STR(ls_i18n_entry_uk(i), UK[i].uk);
    }
    LS_CHECK(ls_i18n_entry_uk(N_SRC) == NULL);
    LS_CHECK(ls_i18n_entry_en(-1) == NULL);
}

LS_CASE(lookups_give_the_plain_tables_cells)
{
    ls_i18n_load(LS_LANG_UK);
    LS_EQ_INT(ls_i18n_dropped(), 0);
    char want[512];
    for (int i = 0; i < N_SRC; i++) {
        tui_utf8_to_cells(want, sizeof(want), UK[i].uk);
        const char *got = ls_tr(UK[i].en);
        /* a key listed twice would answer with its first entry */
        int first = i;
        for (int j = 0; j < i; j++)
            if (!strcmp(UK[j].en, UK[i].en)) { first = j; break; }
        if (first == i) LS_EQ_STR(got, want);
    }
    const char *radio = "a string with no entry";
    LS_CHECK(ls_tr(radio) == radio);
    ls_i18n_load(LS_LANG_EN);
}
