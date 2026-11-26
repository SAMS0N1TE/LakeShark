/* The desk build's atlas for the home base: WARDRIVE is linked into the
   Nano's image (no firmware modules there), so the link reaches wd_atlas
   directly. Overrides the weak ls_lsx_default_store in ls_lsx_svc.c. Not in
   the host bench: lssim's home base runs on test records. */

#include "../ls_lsx_svc.h"
#include "../wardrive/wd_atlas.h"

static uint32_t a_seq(void *ctx)
{
    (void)ctx;
    return wd_atlas_seq();
}

static size_t a_delta(void *ctx, uint32_t since, uint8_t *out, size_t cap, uint32_t *next)
{
    (void)ctx;
    return wd_atlas_delta(since, out, cap, next);
}

static int a_apply(void *ctx, const uint8_t *buf, size_t len, uint32_t origin)
{
    (void)ctx;
    return wd_atlas_apply(buf, len, origin);
}

static const ls_lsx_store_t ATLAS = { NULL, a_seq, a_delta, a_apply };

const ls_lsx_store_t *ls_lsx_default_store(void) { return &ATLAS; }

/* Called from ls_desk_home.c so the linker brings this file in (a weak
   default alone would not pull it out of the archive). */
const ls_lsx_store_t *dk_home_store(void) { return &ATLAS; }
