/* No finger on the glass, for suites that draw lists without touch. */
#include "ls_tui_touch.h"
#include <stddef.h>

bool ls_tui_touch_held(int *start_col, int *start_row, int *col, int *row)
{
    (void)start_col; (void)start_row; (void)col; (void)row;
    return false;
}
