#ifndef LS_RTERM_BUDGET_H
#define LS_RTERM_BUDGET_H
#include <stdbool.h>
#include <stddef.h>
#define LS_RTERM_SERIAL_PICTURE_BYTES 524288u
#define LS_RTERM_WEB_PICTURE_BYTES (2u * 1024u * 1024u)
static inline size_t ls_rterm_picture_budget(bool web)
{
    return web ? LS_RTERM_WEB_PICTURE_BYTES : LS_RTERM_SERIAL_PICTURE_BYTES;
}
#endif
