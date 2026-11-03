#ifndef LAP_JSON_H
#define LAP_JSON_H
#include <stddef.h>
#include <stdbool.h>
typedef enum { LAP_JSON_OBJECT, LAP_JSON_ARRAY, LAP_JSON_STRING, LAP_JSON_NUMBER, LAP_JSON_BOOL, LAP_JSON_NULL } lap_json_kind;
/* String spans exclude quotes and retain JSON escapes; offsets refer to input. */
typedef struct { lap_json_kind kind; size_t start, len; int parent; } lap_json_token;
/* Full-document validation. Returns token count, -1 syntax/depth, -2 capacity.
   Maximum nesting is 32. Input and token storage stay owned by the caller. */
int lap_json_read(const char *text, size_t len, lap_json_token *tokens, size_t capacity);
typedef struct { char *data; size_t len, capacity; bool failed; } lap_json_writer;
void lap_json_writer_init(lap_json_writer *w, char *data, size_t capacity);
/* Append syntax/literals supplied by the caller, or an escaped string value.
   Sticky overflow: no truncation is reported as success. No malloc. */
bool lap_json_raw(lap_json_writer *w, const char *text, size_t len);
bool lap_json_string(lap_json_writer *w, const char *text, size_t len);
#endif
