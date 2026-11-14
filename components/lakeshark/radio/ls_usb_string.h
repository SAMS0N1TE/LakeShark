/* SPDX-License-Identifier: GPL-3.0-or-later
   LakeShark original. A USB string descriptor as a C string. Pure, so the
   bench can check it without a USB host. */
#ifndef LS_USB_STRING_H
#define LS_USB_STRING_H

#include <stddef.h>
#include <stdint.h>

/* `desc` is the raw descriptor: bLength, bDescriptorType, then UTF-16LE code
   units. bLength counts the two header bytes, so the string is
   (bLength - 2) / 2 code units long, and whatever follows the descriptor in
   memory is not part of it. Each code unit keeps its low byte, as before.
   Writes at most cap - 1 characters, always terminates when cap > 0, and
   returns the number written. */
static inline size_t ls_usb_string_ascii(const uint8_t *desc, char *out, size_t cap)
{
    if (!out || cap == 0) return 0;
    size_t n = 0;
    if (desc && desc[0] >= 2) {
        const size_t units = (size_t)(desc[0] - 2) / 2;
        while (n < units && n + 1 < cap) {
            out[n] = (char)desc[2 + 2 * n];
            ++n;
        }
    }
    out[n] = '\0';
    return n;
}

#endif
