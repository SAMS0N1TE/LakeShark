/* A multi-line text being edited: a cursor in a byte buffer, laid out by
   word wrap at whatever width the pane has this frame. Pure logic, so the
   NOTES editor's behaviour is tested on the host rather than by typing. */

#ifndef LS_TEXTBUF_H
#define LS_TEXTBUF_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char *text;        /* caller's storage, always NUL-terminated */
    int len, cap;      /* cap includes the terminator */
    int cursor;        /* byte index, 0..len */
    int want_col;      /* column UP/DOWN aim for; -1 when it follows the cursor */
    int scroll;        /* first visual row shown */
    bool dirty;        /* changed since the last ls_textbuf_mark_clean */
} ls_textbuf_t;

typedef enum {
    LS_TB_LEFT, LS_TB_RIGHT, LS_TB_UP, LS_TB_DOWN,
    LS_TB_HOME, LS_TB_END, LS_TB_PAGE_UP, LS_TB_PAGE_DOWN,
    LS_TB_TOP, LS_TB_BOTTOM, LS_TB_WORD_LEFT, LS_TB_WORD_RIGHT
} ls_textbuf_move_t;

void ls_textbuf_init(ls_textbuf_t *b, char *storage, int cap, const char *initial);
/* Inserts at the cursor. Only printable ASCII and '\n' are kept; tabs become
   two spaces. Returns false, inserting nothing, when it would not all fit. */
bool ls_textbuf_insert(ls_textbuf_t *b, const char *s);
bool ls_textbuf_insert_char(ls_textbuf_t *b, char c);
bool ls_textbuf_backspace(ls_textbuf_t *b);
bool ls_textbuf_delete(ls_textbuf_t *b);
/* width is the wrap width in cells; rows the visible height for paging. */
void ls_textbuf_move(ls_textbuf_t *b, ls_textbuf_move_t how, int width, int rows);
void ls_textbuf_mark_clean(ls_textbuf_t *b);

/* Layout. A visual line is [start, end); `next` is where the following one
   begins, past the space or newline that ended this one. */
int  ls_textbuf_line(const char *text, int len, int start, int width, int *end);
int  ls_textbuf_rows(const ls_textbuf_t *b, int width);
void ls_textbuf_locate(const ls_textbuf_t *b, int width, int *row, int *col);
/* Byte index of the visual row/column, clamped into the text: a tap. */
int  ls_textbuf_index_at(const ls_textbuf_t *b, int width, int row, int col);
/* Start of visual row `row`, or len when the text has fewer rows. */
int  ls_textbuf_row_start(const ls_textbuf_t *b, int width, int row);
void ls_textbuf_keep_visible(ls_textbuf_t *b, int width, int rows);

#ifdef __cplusplus
}
#endif
#endif
