#pragma once

#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------------ */
/* Base64 (RFC 4648) -- shared by OSC 52 and local selection copy/paste */
/*                                                                     */
/* screen_t.clipboard always holds base64, regardless of whether an    */
/* OSC 52 SET or a local selection-copy wrote it, so OSC 52 QUERY and  */
/* Shift+paste both work uniformly either way. See PLAN.md's "Mouse    */
/* support, pass 2" section.                                           */
/* ------------------------------------------------------------------ */

/* Encodes `data` (len bytes) as base64 into `out`, NUL-terminated if
   it fits. Returns bytes written (excluding the NUL), or 0 if out_len
   is insufficient for the full encoded length. */
size_t ghostcon_base64_encode(const uint8_t *data, size_t len, char *out, size_t out_len);

/* Decodes NUL-terminated base64 string `in` into `out`. Invalid
   characters (whitespace, newlines, anything outside the base64
   alphabet) are skipped rather than treated as an error -- a
   clipboard buffer set by an unrelated OSC-52-speaking app should
   degrade gracefully on paste, not silently paste nothing. Returns
   bytes written to `out`, truncated (not overflowed) if out_len is
   insufficient. */
size_t ghostcon_base64_decode(const char *in, uint8_t *out, size_t out_len);
