#include "switch_budget.h"
#include <stdbool.h>
static bool watching;
static size_t largest;
void switch_budget_begin(void) { largest = 0; watching = true; }
size_t switch_budget_end(void) { watching = false; return largest; }
static void allocation(size_t n) { if (watching && n > largest) largest = n; }
void *__real_malloc(size_t);
void *__real_calloc(size_t, size_t);
void *__real_realloc(void *, size_t);
void *__wrap_malloc(size_t n) { allocation(n); return __real_malloc(n); }
void *__wrap_calloc(size_t n, size_t s) { allocation(n * s); return __real_calloc(n, s); }
void *__wrap_realloc(void *p, size_t n) { allocation(n); return __real_realloc(p, n); }
