#include "ls_c6_core.h"
#include <stdio.h>
#include <string.h>

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static bool text32(const uint8_t *p)
{
    if (!p[0]) return false;
    for (int i = 0; i < 32; ++i) {
        if (!p[i]) return true;
        if (p[i] < 32 || p[i] > 126) return false;
    }
    return false;
}

const char *ls_c6_validate(const uint8_t *d, size_t n, size_t slot,
                          ls_c6_hash_fn hash, ls_c6_image_t *out)
{
    if (out) memset(out, 0, sizeof(*out));
    if (!d || !hash || !out || n < 24) return "truncated image header";
    if (n > slot) return "image exceeds C6 OTA slot";
    if (d[0] != 0xe9) return "bad ESP image magic";
    if (d[12] != 13 || d[13] != 0) return "image is not ESP32-C6";
    if (!d[1] || d[1] > 16) return "invalid segment count";
    if (d[23] != 1) return "appended SHA-256 required";
    size_t p = 24;
    uint8_t checksum = 0xef;
    for (unsigned i = 0; i < d[1]; ++i) {
        if (p > n || n - p < 8) return "truncated segment header";
        size_t len = le32(d + p + 4);
        p += 8;
        if (len > n - p) return "segment exceeds file bounds";
        if (i == 0) {
            if (len < 256 || le32(d + p) != 0xabcd5432)
                return "esp_app_desc_t missing";
            if (!text32(d + p + 16) || !text32(d + p + 48))
                return "invalid app project/version";
            memcpy(out->version, d + p + 16, 32);
            memcpy(out->project, d + p + 48, 32);
            if (strcmp(out->project, "network_adapter")) return "wrong app project";
        }
        for (size_t j = 0; j < len; ++j) checksum ^= d[p + j];
        p += len;
    }
    /* Checksum is the last byte of the next 16-byte boundary; SHA covers
     * header, segment headers/data, padding and checksum. No trailing payload. */
    size_t hashed = (p + 16u) & ~(size_t)15u;
    if (hashed > n || n - hashed != 32) return "truncated hash or trailing data";
    if (d[hashed - 1] != checksum) return "bad image checksum";
    uint8_t digest[32];
    if (hash(d, hashed, digest) || memcmp(digest, d + hashed, 32))
        return "appended SHA-256 mismatch";
    return NULL;
}

static bool version(const char *s, uint32_t v[3])
{
    if (!s) return false;
    for (int i = 0; i < 3; ++i) {
        v[i] = 0;
        if (*s < '0' || *s > '9') return false;
        do {
            if (v[i] > 429496729u || (v[i] == 429496729u && *s > '5')) return false;
            v[i] = v[i] * 10u + (unsigned)(*s++ - '0');
        } while (*s >= '0' && *s <= '9');
        if (i < 2 && *s++ != '.') return false;
    }
    return !*s;
}

bool ls_c6_version_allowed(const char *candidate, const char *running, bool force)
{
    uint32_t a[3], b[3];
    if (force) return true;
    /* Unknown/non-release versions need explicit force, never lexical order. */
    if (!version(candidate, a) || !version(running, b)) return false;
    for (int i = 0; i < 3; ++i) if (a[i] != b[i]) return a[i] > b[i];
    return false;
}

static void report(const ls_c6_transport_t *io, ls_c6_result_t *r,
                   ls_c6_stage_t stage, const char *message)
{
    r->stage = stage;
    snprintf(r->message, sizeof(r->message), "%s", message);
    io->report(io->ctx, r);
}

static bool fail(const ls_c6_transport_t *io, ls_c6_result_t *r, const char *why, int error)
{
    r->stage = LS_C6_FAILED;
    r->error = error;
    snprintf(r->message, sizeof(r->message), "%.86s (%d); %s", why, error,
             r->boot == LS_C6_OLD_ACTIVE ? "old image still active" :
             r->boot == LS_C6_ACTIVATED ? "activation accepted; boot unverified" :
             "activation attempted; boot selection unknown");
    io->report(io->ctx, r);
    return false;
}

bool ls_c6_run(const uint8_t *data, size_t size, bool force, ls_c6_hash_fn hash,
               const ls_c6_transport_t *io, ls_c6_result_t *r)
{
    memset(r, 0, sizeof(*r));
    r->size = size;
    report(io, r, LS_C6_VALIDATE, "Validating SD image");
    ls_c6_image_t image;
    const char *why = ls_c6_validate(data, size, LS_C6_SLOT_SIZE, hash, &image);
    if (why) return fail(io, r, why, -1);
    char message[160];
    snprintf(message, sizeof(message), "Validated %s %s", image.project, image.version);
    report(io, r, LS_C6_VALIDATE, message);
    int e = io->power(io->ctx);
    if (e) return fail(io, r, "Battery below 20% or gauge unavailable", e);
    e = io->version(io->ctx, r->old_version);
    if (e) return fail(io, r, "Cannot read running C6 version", e);
    if (!ls_c6_version_allowed(image.version, r->old_version, force))
        return fail(io, r, "Same/older or unordered version; force required", -1);
    report(io, r, LS_C6_STOP, "Stopping Wi-Fi/BLE; kept off until P4 reboot");
    e = io->stop(io->ctx);
    if (e) return fail(io, r, "Cannot quiesce radios; users remain inhibited", e);
    report(io, r, LS_C6_BEGIN, "Preparing inactive C6 slot");
    e = io->begin(io->ctx);
    if (e) return fail(io, r, "C6 OTA begin failed", e);
    uint64_t start = io->millis(io->ctx);
    while (r->sent < size) {
        if (io->millis(io->ctx) - start >= 600000)
            return fail(io, r, "C6 transfer timed out; reboot P4 before retry", -1);
        size_t n = size - r->sent;
        if (n > LS_C6_CHUNK) n = LS_C6_CHUNK;
        e = io->write(io->ctx, data + r->sent, n);
        if (e) return fail(io, r, "C6 write failed; reboot P4 before retry", e);
        r->sent += n;
        report(io, r, LS_C6_WRITE, "Writing inactive C6 slot");
    }
    report(io, r, LS_C6_END, "C6 validating written image");
    e = io->end(io->ctx);
    if (e) return fail(io, r, "C6 OTA end failed; reboot P4 before retry", e);
    report(io, r, LS_C6_ACTIVATE, "Activating C6 image");
    r->boot = LS_C6_BOOT_UNKNOWN; /* lost response / timer error can follow set_boot_partition */
    e = io->activate(io->ctx);
    if (e) return fail(io, r, "C6 activate failed; inspect/recover before retry", e);
    r->boot = LS_C6_ACTIVATED;
    report(io, r, LS_C6_WAIT, "Waiting for a new C6 boot (45 seconds)");
    e = io->wait(io->ctx, 45000, r->new_version);
    if (e) return fail(io, r, "C6 reboot/version timeout; UART recovery may be needed", e);
    if (strcmp(image.version, r->new_version)) {
        snprintf(message, sizeof(message), "Unexpected version: %s -> %s; expected %s",
                 r->old_version, r->new_version, image.version);
        return fail(io, r, message, -1);
    }
    snprintf(message, sizeof(message), "%s -> %s; verified. Reboot P4 to use radios",
             r->old_version, r->new_version);
    report(io, r, LS_C6_DONE, message);
    return true;
}
