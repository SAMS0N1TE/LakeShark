#include "ls_test.h"
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

int ls_shim_log_enabled = 0;

#define LS_MAX_CASES 256

typedef struct {
    const char *file;
    const char *name;
    ls_case_fn  fn;
    const char *xfail;   /* NULL for an ordinary case */
} ls_case_t;

static ls_case_t s_cases[LS_MAX_CASES];
static int       s_n_cases;
static int       s_failed_this_case;
static int       s_n_failures;

static const char *basename_of(const char *path)
{
    const char *a = strrchr(path, '/');
    const char *b = strrchr(path, '\\');
    const char *p = (a > b) ? a : b;
    return p ? p + 1 : path;
}

static ls_case_t *alloc_case(const char *file, const char *name, ls_case_fn fn)
{
    if (s_n_cases >= LS_MAX_CASES) {
        fprintf(stderr, "ls_test: too many cases, raise LS_MAX_CASES\n");
        exit(2);
    }
    ls_case_t *c = &s_cases[s_n_cases++];
    c->file  = basename_of(file);
    c->name  = name;
    c->fn    = fn;
    c->xfail = NULL;
    return c;
}

void ls_register(const char *file, const char *name, ls_case_fn fn)
{
    alloc_case(file, name, fn);
}

void ls_register_xfail(const char *file, const char *name, ls_case_fn fn,
                       const char *ticket)
{
    alloc_case(file, name, fn)->xfail = ticket;
}

void ls_fail(const char *file, int line, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("    %s:%d  ", basename_of(file), line);
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    s_failed_this_case = 1;
    s_n_failures++;
}

void ls_note(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("    . ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}

int main(int argc, char **argv)
{
    const char *filter = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0) ls_shim_log_enabled = 1;
        else                            filter = argv[i];
    }

    int run = 0, passed = 0, failed = 0, xfailed = 0, xpassed = 0;
    for (int i = 0; i < s_n_cases; i++) {
        if (filter && !strstr(s_cases[i].name, filter)
                   && !strstr(s_cases[i].file, filter)) continue;
        run++;
        s_failed_this_case = 0;
        ls_diag_clear();
        printf("  %-26s %s\n", s_cases[i].file, s_cases[i].name);
        fflush(stdout);
        int before = s_n_failures;
        s_cases[i].fn();
        int broke = s_failed_this_case;

        if (s_cases[i].xfail) {
            /* An expected failure is not a failure, and its assertion output
               is not evidence of anything, so it does not count either. */
            s_n_failures = before;
            if (broke) {
                xfailed++;
                printf("    XFAIL (known: %s)\n", s_cases[i].xfail);
            } else {
                xpassed++;
                failed++;
                printf("    XPASS - this is fixed now. Promote the case to a\n"
                       "           plain LS_CASE and close %s\n", s_cases[i].xfail);
            }
        } else if (broke) {
            failed++;
            printf("    FAIL\n");
        } else {
            passed++;
        }
        fflush(stdout);
    }

    printf("\n%d run, %d passed, %d failed", run, passed, failed);
    if (xfailed) printf(", %d known-failing", xfailed);
    if (xpassed) printf(", %d UNEXPECTEDLY FIXED", xpassed);
    printf(" (%d assertions failed)\n", s_n_failures);
    if (run == 0) { printf("no cases matched filter\n"); return 3; }
    return failed ? 1 : 0;
}
