#define _DEFAULT_SOURCE
#include "stream_internal.h"
#include <stdlib.h>
#include <string.h>

/* APC (Kitty graphics) + DCS (sixel) payload dispatch
 *
 * Split out of stream.c per PLAN.md "Phase 2".
 *
 * The state machine accumulates the opaque payload and confirms its ST
 * terminator.
 *
 * This module hands the completed payload to the image decoders and applies
 * both decoders' shared "advance past the placement via real linefeed, then
 * land at its right edge" cursor behavior.
 */

void gc_stream_apc_dispatch(ghostcon_stream_t *st, ghostcon_screen_t *s)
{
    if (st->apc_introducer != '_')
        return; /* SOS/PM: no known use, discard as before */

    ghostcon_kitty_cursor_move_t move;
    ghostcon_kitty_graphics_handle(ghostcon_screen_active_kitty_graphics(s),
                                   st->buf, st->apc_len,
                                   s->cursor.x, s->cursor.y,
                                   st->cell_w, st->cell_h,
                                   st->output_fn, st->output_userdata, &move);

    /* Default Kitty cursor movement after a placement (Ghostty's cursor_movement
     * == .after, suppressed only by an explicit C=1): advance via real linefeed
     * (so it scrolls correctly at a scroll region boundary, unlike just bumping
     * cursor.y) then land the cursor just past the placement's right edge.
     */
    if (move.moved)
    {
        for (int32_t i = 0; i < move.rows; i++)
            ghostcon_screen_linefeed(s);
        int32_t col = move.col;
        if (col < 0) col = 0;
        if (col >= (int32_t)s->cols) col = (int32_t)s->cols - 1;
        s->cursor.x = (int16_t)col;
    }
}

/* Hard cap on how large *buf is allowed to grow to hold an unchunked DCS payload
 * (sixel)
 *
 * see stream.h's own doc comment on buf_cap for why growth exists at all.
 *
 * Generous enough for a large, photographic sixel image (which can easily run
 * into the hundreds of KB of run-length-encoded text) while still bounding the
 * worst case a hostile/broken client sending an unterminated DCS q could force
 * ghostcon to allocate.
 */
#define GC_STREAM_DCS_MAX_BUF (32u * 1024u * 1024u)

/* Grows st->buf (transitioning off local_buf on the first call) to fit at least
 * one more byte, up to GC_STREAM_DCS_MAX_BUF.
 *
 * Returns false (leaving st->buf/buf_cap unchanged) if already at cap or
 * allocation fails
 *
 * the caller's only recourse at that point is to stop accepting more bytes, same
 * as any other overflow case in this file.
 */
bool gc_stream_dcs_buf_grow(ghostcon_stream_t *st)
{
    if (st->buf_cap >= GC_STREAM_DCS_MAX_BUF)
        return false;
    uint32_t new_cap = st->buf_cap * 2;
    if (new_cap > GC_STREAM_DCS_MAX_BUF)
        new_cap = GC_STREAM_DCS_MAX_BUF;
    char *new_buf = st->buf == st->local_buf ? malloc(new_cap) : realloc(st->buf, new_cap);
    if (!new_buf)
        return false;
    if (st->buf == st->local_buf)
        memcpy(new_buf, st->local_buf, st->dcs_len);
    st->buf = new_buf;
    st->buf_cap = new_cap;
    return true;
}

void gc_stream_dcs_dispatch(ghostcon_stream_t *st, ghostcon_screen_t *s)
{
    if (st->dcs_final != 'q')
        return; /* only sixel is implemented; every other DCS command
                    (DECRQSS, etc.) is silently discarded, same as before
                    this function existed */

    int32_t params[GC_STREAM_MAX_PARAMS];
    for (uint8_t i = 0; i < st->params_idx; i++)
        params[i] = st->params[i];

    ghostcon_sixel_cursor_move_t move;
    ghostcon_sixel_decode(ghostcon_screen_active_sixel_state(s),
                          params, st->params_idx,
                          st->buf, st->dcs_len,
                          s->cursor.x, s->cursor.y,
                          st->cell_w, st->cell_h, &move);

    /* Same "advance past the image via real linefeed, then land at its right
     * edge" behavior as Kitty's apc_dispatch() above
     *
     * sixel has no C=1-equivalent suppression key, so this always applies when a
     * placement was actually made.
     */
    if (move.moved)
    {
        for (int32_t i = 0; i < move.rows; i++)
            ghostcon_screen_linefeed(s);
        int32_t col = move.col;
        if (col < 0) col = 0;
        if (col >= (int32_t)s->cols) col = (int32_t)s->cols - 1;
        s->cursor.x = (int16_t)col;
    }
}

/* ------------------------------------------------------------------ */
