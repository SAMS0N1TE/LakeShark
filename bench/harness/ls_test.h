/* Minimal test harness. No dependencies, nothing to install.
   A test file declares cases with LS_CASE and the runner collects them via a
   constructor, so there is no registration list to keep in sync. */
#ifndef LS_TEST_H
#define LS_TEST_H

#include <stdio.h>
#include <string.h>
#include <math.h>

typedef void (*ls_case_fn)(void);

void ls_register(const char *file, const char *name, ls_case_fn fn);
void ls_register_xfail(const char *file, const char *name, ls_case_fn fn,
                       const char *ticket);
void ls_fail(const char *file, int line, const char *fmt, ...);
void ls_note(const char *fmt, ...);

#define LS_CASE(name)                                                        \
    static void name(void);                                                  \
    static void ls_reg_##name(void) __attribute__((constructor));            \
    static void ls_reg_##name(void) { ls_register(__FILE__, #name, name); }  \
    static void name(void)

/* A case that documents a defect that is not fixed yet. It must fail; if it
   starts passing the suite goes red, which forces whoever fixed it to promote
   the case to a real LS_CASE instead of leaving a stale marker behind. */
#define LS_CASE_KNOWN_FAIL(name, ticket)                                     \
    static void name(void);                                                  \
    static void ls_reg_##name(void) __attribute__((constructor));            \
    static void ls_reg_##name(void)                                          \
        { ls_register_xfail(__FILE__, #name, name, ticket); }                \
    static void name(void)

#define LS_CHECK(cond)                                                       \
    do { if (!(cond)) ls_fail(__FILE__, __LINE__, "CHECK failed: %s", #cond); } while (0)

#define LS_CHECK_MSG(cond, ...)                                              \
    do { if (!(cond)) ls_fail(__FILE__, __LINE__, __VA_ARGS__); } while (0)

#define LS_EQ_INT(a, b)                                                      \
    do { long _a = (long)(a), _b = (long)(b);                                \
         if (_a != _b) ls_fail(__FILE__, __LINE__,                           \
            "%s == %s  (got %ld, want %ld)", #a, #b, _a, _b); } while (0)

#define LS_EQ_UINT(a, b)                                                     \
    do { unsigned long _a = (unsigned long)(a), _b = (unsigned long)(b);     \
         if (_a != _b) ls_fail(__FILE__, __LINE__,                           \
            "%s == %s  (got %lu / 0x%lx, want %lu / 0x%lx)",                 \
            #a, #b, _a, _a, _b, _b); } while (0)

#define LS_EQ_STR(a, b)                                                      \
    do { const char *_sa = (a), *_sb = (b);                                  \
         if (strcmp(_sa ? _sa : "(null)", _sb ? _sb : "(null)") != 0)        \
            ls_fail(__FILE__, __LINE__, "%s == %s  (got [%s], want [%s])",   \
                    #a, #b, _sa ? _sa : "(null)", _sb ? _sb : "(null)"); } while (0)

#define LS_NEAR(a, b, tol)                                                   \
    do { double _a = (double)(a), _b = (double)(b), _t = (double)(tol);      \
         if (fabs(_a - _b) > _t) ls_fail(__FILE__, __LINE__,                 \
            "%s ~= %s  (got %g, want %g +/- %g)", #a, #b, _a, _b, _t); } while (0)

/* Deterministic PRNG. Tests must never call rand(): a fixture that only fails
   on some runs is worse than no fixture at all. */
typedef struct { unsigned long long s; } ls_rng_t;

static inline void ls_rng_seed(ls_rng_t *r, unsigned long long s)
{
    r->s = s ? s : 0x9E3779B97F4A7C15ull;
}

static inline unsigned ls_rng_u32(ls_rng_t *r)
{
    r->s ^= r->s << 13;
    r->s ^= r->s >> 7;
    r->s ^= r->s << 17;
    return (unsigned)(r->s >> 32);
}

/* Sum of four uniforms: mean 0, sd about 0.29. Close enough to Gaussian for
   an SNR sweep, and it costs nothing. */
static inline float ls_rng_noise(ls_rng_t *r)
{
    float a = 0.0f;
    for (int i = 0; i < 4; i++)
        a += (float)(ls_rng_u32(r) & 0xFFFF) / 65535.0f - 0.5f;
    return a * 0.5f;
}

/* Captured diag_line() output, so a decoder's own diagnostics become
   assertable instead of something you squint at over serial. */
void        ls_diag_clear(void);
int         ls_diag_count(void);
const char *ls_diag_line(int i);
int         ls_diag_contains(const char *needle);

#endif
