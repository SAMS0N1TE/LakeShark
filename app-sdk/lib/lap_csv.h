#ifndef LAP_CSV_H
#define LAP_CSV_H
#include <stddef.h>
#include <stdbool.h>
typedef struct { const char *data; size_t len, pos; bool trailing; } lap_csv_reader;
void lap_csv_reader_init(lap_csv_reader *r, const char *data, size_t len);
/* 1 field, 0 EOF, -1 malformed/overflow (reader position unchanged).
   Handles quoted commas, doubled quotes, CRLF and embedded newlines. */
int lap_csv_next(lap_csv_reader *r, char *out, size_t capacity, bool *end_record);
/* Append one field, then comma or CRLF. *used excludes terminating NUL.
   Returns false without changing output on capacity failure. */
bool lap_csv_write(char *out, size_t capacity, size_t *used, const char *field, size_t len, bool end_record);
#endif
