

#include "ls_stroke.h"

char ls_stroke_glyph(bool tl, bool tr, bool bl, bool br)
{
    const int n = (tl ? 1 : 0) + (tr ? 1 : 0) + (bl ? 1 : 0) + (br ? 1 : 0);

    if (n == 0) return 0;

    /* Three or four quarters is where strokes meet. A plus reads as a
       junction, which at this scale is usually what it is - a crossroads, or
       a road meeting the shore. */
    if (n >= 3) return '+';

    if (n == 2) {

        if (tl && tr) return '-';
        if (bl && br) return '_';
        /* One above the other: a vertical run. */
        if (tl && bl) return '|';
        if (tr && br) return '|';
        /* Opposite corners: a diagonal, and WHICH diagonal matters. Top-left
           to bottom-right descends to the right, which is a backslash. */
        if (tl && br) return '\\';
        if (tr && bl) return '/';
    }

    /* A single quarter. Not a stroke at all - a road that only clips the
       corner of a cell, or the last cell of a shoreline. A full stop is the
       smallest mark the font has and does not pretend to a direction the
       ink does not have. */
    return '.';
}
