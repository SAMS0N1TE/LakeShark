/* The public halves of the keys that sign update manifests. The private
   halves never leave the release machines; a manifest that none of these
   keys signed is refused, and any one of them is enough.

   Spare key: generate a second pair offline, append its public PEM to
   LS_OTA_PUBLIC_KEYS below, and ship that build. Keep the spare private key
   somewhere other than the release machine.

   Rotating: ship a build that trusts both the old and the new key, and wait
   until boards have taken it. Then sign manifests with the new key. Only a
   later build drops the old key; a board that never took the both-keys build
   can no longer be updated over WiFi and needs USB. */
#pragma once

#include <stddef.h>

static const char *const LS_OTA_PUBLIC_KEYS[] = {
    "-----BEGIN PUBLIC KEY-----\n"
    "MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEkAIsN98pagIzoLaThdZVKTXh1v4k\n"
    "VRLXajCgd5C6AoXzpvWl70NenSciUzCX38NZ6P3MVFlL2rxs3kawJKfYfw==\n"
    "-----END PUBLIC KEY-----\n",
    "-----BEGIN PUBLIC KEY-----\n"
    "MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEgvkGPrIjMNm7+96CdH1vJyV/2uLS\n"
    "JXk5CGZNM3zKmaBXNvKmQw3m+xIKQYVpmMSC3NGS8QHCyhfbV9tKrWQ5CA==\n"
    "-----END PUBLIC KEY-----\n",
};

#define LS_OTA_PUBLIC_KEY_COUNT (sizeof(LS_OTA_PUBLIC_KEYS) / sizeof(LS_OTA_PUBLIC_KEYS[0]))
