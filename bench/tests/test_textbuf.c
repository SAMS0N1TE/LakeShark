#include "ls_test.h"
#include "ls_textbuf.h"
#include <string.h>

static char store[256];
static ls_textbuf_t b;

static void with(const char *text) { ls_textbuf_init(&b, store, sizeof(store), text); }

LS_CASE(insert_keeps_printable_ascii_and_newlines_only)
{
    with("");
    LS_CHECK(ls_textbuf_insert(&b, "a\tb\x01\n\xc3\xa9z"));
    LS_EQ_STR(store, "a  b\nz");
    LS_EQ_INT(b.cursor, 6);
    LS_CHECK(b.dirty);
}

LS_CASE(insert_that_does_not_fit_changes_nothing)
{
    char small[8];
    ls_textbuf_init(&b, small, sizeof(small), "abc");
    b.cursor = 3;
    LS_CHECK(!ls_textbuf_insert(&b, "defgh"));
    LS_EQ_STR(small, "abc");
    LS_CHECK(ls_textbuf_insert(&b, "defg"));
    LS_EQ_STR(small, "abcdefg");
}

LS_CASE(backspace_and_delete_edit_around_the_cursor)
{
    with("hello");
    b.cursor = 2;
    LS_CHECK(ls_textbuf_backspace(&b));
    LS_EQ_STR(store, "hllo"); LS_EQ_INT(b.cursor, 1);
    LS_CHECK(ls_textbuf_delete(&b));
    LS_EQ_STR(store, "hlo");
    b.cursor = 0; LS_CHECK(!ls_textbuf_backspace(&b));
    b.cursor = 3; LS_CHECK(!ls_textbuf_delete(&b));
}

LS_CASE(words_wrap_at_spaces_and_long_words_break)
{
    /* Width 6 lays out 5 cells, leaving the cursor a cell. */
    with("one two three abcdefghij");
    LS_EQ_INT(ls_textbuf_rows(&b, 6), 6);
    LS_EQ_INT(ls_textbuf_row_start(&b, 6, 1), 4);   /* "two"   */
    LS_EQ_INT(ls_textbuf_row_start(&b, 6, 2), 8);   /* "three" */
    LS_EQ_INT(ls_textbuf_row_start(&b, 6, 3), 14);  /* "abcde" */
    LS_EQ_INT(ls_textbuf_row_start(&b, 6, 4), 19);  /* "fghij" */
}

LS_CASE(the_cursor_after_a_trailing_newline_or_full_row_is_on_a_new_row)
{
    int row, col;
    with("abc\n"); b.cursor = 4;
    LS_EQ_INT(ls_textbuf_rows(&b, 10), 2);
    ls_textbuf_locate(&b, 10, &row, &col); LS_EQ_INT(row, 1); LS_EQ_INT(col, 0);
    with("abcd"); b.cursor = 4;
    ls_textbuf_locate(&b, 5, &row, &col); LS_EQ_INT(row, 1); LS_EQ_INT(col, 0);
    b.cursor = 3;
    ls_textbuf_locate(&b, 5, &row, &col); LS_EQ_INT(row, 0); LS_EQ_INT(col, 3);
    with(""); LS_EQ_INT(ls_textbuf_rows(&b, 10), 1);
    ls_textbuf_locate(&b, 10, &row, &col); LS_EQ_INT(row, 0); LS_EQ_INT(col, 0);
}

LS_CASE(no_cursor_position_ever_lands_outside_the_pane)
{
    with("the quick brown fox jumps over\nthe lazy dog\n\nxxxxxxxxxxxxxxxxxxxxxx end");
    for (int width = 2; width < 30; width++)
        for (int i = 0; i <= b.len; i++) {
            int row, col;
            b.cursor = i;
            ls_textbuf_locate(&b, width, &row, &col);
            LS_CHECK_MSG(col >= 0 && col < width, "width %d index %d col %d", width, i, col);
            LS_CHECK(row < ls_textbuf_rows(&b, width));
        }
}

LS_CASE(up_and_down_keep_the_column_across_short_lines)
{
    with("abcdefgh\nab\nabcdefgh");
    b.cursor = 6;                                   /* row 0, col 6 */
    ls_textbuf_move(&b, LS_TB_DOWN, 20, 10);
    LS_EQ_INT(b.cursor, 11);                        /* end of "ab" */
    ls_textbuf_move(&b, LS_TB_DOWN, 20, 10);
    LS_EQ_INT(b.cursor, 18);                        /* col 6 again */
    ls_textbuf_move(&b, LS_TB_LEFT, 20, 10);
    ls_textbuf_move(&b, LS_TB_UP, 20, 10);
    LS_EQ_INT(b.cursor, 11);
    ls_textbuf_move(&b, LS_TB_UP, 20, 10);
    LS_EQ_INT(b.cursor, 5);
}

LS_CASE(home_end_and_word_moves)
{
    with("alpha beta gamma");
    b.cursor = 8;
    ls_textbuf_move(&b, LS_TB_HOME, 40, 5); LS_EQ_INT(b.cursor, 0);
    ls_textbuf_move(&b, LS_TB_END, 40, 5);  LS_EQ_INT(b.cursor, 16);
    ls_textbuf_move(&b, LS_TB_WORD_LEFT, 40, 5); LS_EQ_INT(b.cursor, 11);
    ls_textbuf_move(&b, LS_TB_WORD_LEFT, 40, 5); LS_EQ_INT(b.cursor, 6);
    ls_textbuf_move(&b, LS_TB_WORD_RIGHT, 40, 5); LS_EQ_INT(b.cursor, 11);
}

LS_CASE(a_tap_places_the_cursor_and_clamps_past_the_line_end)
{
    with("short\nlonger line");
    LS_EQ_INT(ls_textbuf_index_at(&b, 20, 0, 2), 2);
    LS_EQ_INT(ls_textbuf_index_at(&b, 20, 0, 15), 5);
    LS_EQ_INT(ls_textbuf_index_at(&b, 20, 1, 3), 9);
    LS_EQ_INT(ls_textbuf_index_at(&b, 20, 9, 0), b.len);
}

LS_CASE(scroll_follows_the_cursor_and_never_shows_past_the_end)
{
    with("1\n2\n3\n4\n5\n6\n7\n8\n9");
    b.cursor = b.len;
    ls_textbuf_keep_visible(&b, 10, 4);
    LS_EQ_INT(b.scroll, 5);
    b.cursor = 0;
    ls_textbuf_keep_visible(&b, 10, 4);
    LS_EQ_INT(b.scroll, 0);
    with("1\n2");
    b.scroll = 7; ls_textbuf_keep_visible(&b, 10, 4);
    LS_EQ_INT(b.scroll, 0);
}

LS_CASE(page_moves_by_the_visible_height)
{
    with("0\n1\n2\n3\n4\n5\n6\n7\n8\n9");
    b.cursor = 0;
    ls_textbuf_move(&b, LS_TB_PAGE_DOWN, 10, 4);
    int row; ls_textbuf_locate(&b, 10, &row, NULL); LS_EQ_INT(row, 3);
    ls_textbuf_move(&b, LS_TB_PAGE_DOWN, 10, 4);
    ls_textbuf_move(&b, LS_TB_PAGE_DOWN, 10, 4);
    ls_textbuf_move(&b, LS_TB_PAGE_DOWN, 10, 4);
    ls_textbuf_locate(&b, 10, &row, NULL); LS_EQ_INT(row, 9);
    ls_textbuf_move(&b, LS_TB_PAGE_UP, 10, 4);
    ls_textbuf_locate(&b, 10, &row, NULL); LS_EQ_INT(row, 6);
}
