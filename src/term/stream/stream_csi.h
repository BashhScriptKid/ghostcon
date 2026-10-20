/* stream_csi.h
 *
 * Private header for the CSI dispatch module (stream_csi.c).
 *
 * The CSI parameter helpers live here rather than in the module's shared
 * preamble because only this module uses them.
 *
 * NOT part of the public API (it lives under src/, not include/)
 */

#pragma once

#include "stream_internal.h"

/* CSI parameter access */

/* Parse a collected CSI parameter. Returns default if empty. */
static inline int param(ghostcon_stream_t *st, int idx, int def)
{
    if (idx < st->params_idx && st->params[idx] != 0)
        return st->params[idx];
    return def;
}

#define PARAM(st, idx, def) param(st, idx, def)
#define PARAM1(st, def) param(st, 0, def)
#define PARAM2(st, def) param(st, 1, def)

/* Ghostty drops CSI sequences whose parameter count doesn't match the
 * handler's expected range (stream.zig `switch (input.params.len)` with an
 * `else => return`). mirror that here.
 */
static inline bool csi_param_count(ghostcon_stream_t *st, uint8_t minc, uint8_t maxc)
{
    return st->params_idx >= minc && st->params_idx <= maxc;
}

/* A raw CSI param can be up to UINT16_MAX (65535).
 *
 * Handlers that apply it via an O(n) per-step loop (rather than O(1) arithmetic
 * or a single memmove) must not loop that many times just to hang synchronously
 * in the single-threaded render/input loop
 *
 * clamp to the largest count that can still change anything observable.
 */
static inline int clamp_loop_count(int n, int max_useful)
{
    if (n > max_useful) return max_useful;
    if (n < 0) return 0;
    return n;
}

/* Output channel helper */

static inline void stream_output(ghostcon_stream_t *st, const char *s)
{
    if (st->output_fn)
        st->output_fn(st->output_userdata, (const uint8_t *)s, strlen(s));
}

/* Cross-module dispatch entry point */

typedef void (*gc_stream_csi_handler_t)(ghostcon_stream_t *,
                                        ghostcon_screen_t *);

/* Looks up the handler for a completed CSI sequence (final byte +
 * accumulated intermediates/params). Returns NULL for unknown/invalid
 * combinations.
 *
 * The state machine invokes the returned handler, so handlers may live
 * file-static in stream_csi.c.
 */
extern gc_stream_csi_handler_t
gc_stream_csi_dispatch(ghostcon_stream_t *st, uint8_t final_byte);

/* True when the CSI dispatch table is sorted by final byte and has no
 * two identical rows -- the invariant csi_lookup()'s binary search
 * depends on. See stream_csi.c.
 */
extern bool gc_stream_csi_table_sorted(void);
