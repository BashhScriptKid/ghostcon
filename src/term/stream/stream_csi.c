#define _DEFAULT_SOURCE /* CLOCK_MONOTONIC under -std=c11 without this */
#include "stream_internal.h"
#include "ghostcon/core/log.h"
#include <ghostty/vt/sgr.h>
#include <ghostty/vt/osc.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

/* CSI dispatch handlers + table
 *
 * Split out of stream.c per PLAN.md "Phase 2: table-driven dispatch": the
 * state machine collects params/intermediates and hands completed sequences
 * here via gc_stream_csi_dispatch().
 *
 * Adding a new sequence is one handler function plus one table line below,
 * NOT another nested switch case in the monolithic state machine.
 */

static void handle_csi_cursor_up(ghostcon_stream_t *st, ghostcon_screen_t *s)
{
    if (!csi_param_count(st, 0, 1)) return;
    int n = PARAM1(st, 1);
    ghostcon_screen_cursor_up(s, n);
}

static void handle_csi_cursor_down(ghostcon_stream_t *st, ghostcon_screen_t *s)
{
    if (!csi_param_count(st, 0, 1)) return;
    int n = PARAM1(st, 1);
    ghostcon_screen_cursor_down(s, n);
}

static void handle_csi_cursor_forward(ghostcon_stream_t *st, ghostcon_screen_t *s)
{
    if (!csi_param_count(st, 0, 1)) return;
    int n = PARAM1(st, 1);
    ghostcon_screen_cursor_right(s, n);
}

static void handle_csi_cursor_back(ghostcon_stream_t *st, ghostcon_screen_t *s)
{
    if (!csi_param_count(st, 0, 1)) return;
    int n = PARAM1(st, 1);
    ghostcon_screen_cursor_left(s, n);
}

static void handle_csi_cursor_next_line(ghostcon_stream_t *st, ghostcon_screen_t *s)
{
    if (!csi_param_count(st, 0, 1)) return;
    int n = clamp_loop_count(PARAM1(st, 1), s->rows_visible);
    for (int i = 0; i < n; i++)
        ghostcon_screen_cursor_next_line(s);
}

static void handle_csi_cursor_prev_line(ghostcon_stream_t *st, ghostcon_screen_t *s)
{
    if (!csi_param_count(st, 0, 1)) return;
    int n = clamp_loop_count(PARAM1(st, 1), s->rows_visible);
    for (int i = 0; i < n; i++)
        ghostcon_screen_cursor_prev_line(s);
}

static void handle_csi_cursor_horizontal_abs(ghostcon_stream_t *st, ghostcon_screen_t *s)
{
    if (!csi_param_count(st, 0, 1)) return;
    int x = PARAM1(st, 1) - 1;
    ghostcon_screen_cursor_horizontal_abs(s, x);
}

static void handle_csi_cursor_vertical_abs(ghostcon_stream_t *st, ghostcon_screen_t *s)
{
    /* VPA (CSI Ps d): set row, column unchanged. */
    if (!csi_param_count(st, 0, 1)) return;
    int y = PARAM1(st, 1) - 1;
    ghostcon_screen_cursor_vertical_abs(s, y);
}

static void handle_csi_tab_forward(ghostcon_stream_t *st, ghostcon_screen_t *s)
{
    /* CHT (CSI Ps I): move forward Ps tab stops. */
    if (!csi_param_count(st, 0, 1)) return;
    int n = clamp_loop_count(PARAM1(st, 1), s->cols);
    for (int i = 0; i < n; i++)
        ghostcon_screen_tab(s);
}

static void handle_csi_tab_back(ghostcon_stream_t *st, ghostcon_screen_t *s)
{
    /* CBT (CSI Ps Z): move backward Ps tab stops. */
    if (!csi_param_count(st, 0, 1)) return;
    int n = clamp_loop_count(PARAM1(st, 1), s->cols);
    for (int i = 0; i < n; i++)
        ghostcon_screen_tab_back(s);
}

static void handle_csi_repeat(ghostcon_stream_t *st, ghostcon_screen_t *s)
{
    /* REP (CSI Ps b): repeat preceding graphic character Ps times. Ghostty:
     * @max(count, 1). No last-char → no-op.
     */
    if (!csi_param_count(st, 0, 1)) return;
    if (s->last_codepoint == 0) return;
    int n = PARAM1(st, 1);
    if (n == 0) n = 1;
    /* Beyond one full screen, further repeats just keep scrolling the same
     * character
     *
     * clamp instead of hanging synchronously.
     */
    n = clamp_loop_count(n, (int)s->cols * (int)s->rows_visible);
    for (int i = 0; i < n; i++)
        ghostcon_screen_put_char(s, s->last_codepoint);
}

static void handle_csi_cursor_position(ghostcon_stream_t *st, ghostcon_screen_t *s)
{
    /* CUP (CSI Ps;Pt H): Ghostty accepts 0, 1, or 2 params, else ignores. */
    if (!csi_param_count(st, 0, 2)) return;
    long row = (long)PARAM1(st, 1) - 1;
    long col = (long)PARAM2(st, 1) - 1;
    if (row < 0) row = 0;
    if (col < 0) col = 0;
    if (row >= s->rows_visible) row = s->rows_visible - 1;
    if (col >= s->cols) col = s->cols - 1;
    ghostcon_screen_cursor_set(s, (int16_t)col, (int16_t)row);
}

static void handle_csi_horizontal_vertical_position(ghostcon_stream_t *st, ghostcon_screen_t *s)
{
    handle_csi_cursor_position(st, s);
}

static void handle_csi_erase_display(ghostcon_stream_t *st, ghostcon_screen_t *s)
{
    if (!csi_param_count(st, 0, 1)) return;
    int mode = PARAM1(st, 0);
    bool dec = (st->intermediates_idx == 1 && st->intermediates[0] == '?');
    bool respect = dec || (s->protected_mode == GC_PROTECTED_ISO);
    if (respect)
        ghostcon_screen_erase_display_protected(s, mode);
    else
        ghostcon_screen_erase_display(s, mode);
}

static void handle_csi_erase_line(ghostcon_stream_t *st, ghostcon_screen_t *s)
{
    if (!csi_param_count(st, 0, 1)) return;
    int mode = PARAM1(st, 0);
    bool dec = (st->intermediates_idx == 1 && st->intermediates[0] == '?');
    bool respect = dec || (s->protected_mode == GC_PROTECTED_ISO);
    if (respect)
        ghostcon_screen_erase_line_protected(s, mode);
    else
        ghostcon_screen_erase_line(s, mode);
}

static void handle_csi_insert_lines(ghostcon_stream_t *st, ghostcon_screen_t *s)
{
    if (!csi_param_count(st, 0, 1)) return;
    int n = PARAM1(st, 1);
    ghostcon_screen_insert_lines(s, n);
}

static void handle_csi_delete_lines(ghostcon_stream_t *st, ghostcon_screen_t *s)
{
    if (!csi_param_count(st, 0, 1)) return;
    int n = PARAM1(st, 1);
    ghostcon_screen_delete_lines(s, n);
}

static void handle_csi_delete_chars(ghostcon_stream_t *st, ghostcon_screen_t *s)
{
    if (!csi_param_count(st, 0, 1)) return;
    int n = PARAM1(st, 1);
    ghostcon_screen_delete_chars(s, n);
}

static void handle_csi_erase_chars(ghostcon_stream_t *st, ghostcon_screen_t *s)
{
    if (!csi_param_count(st, 0, 1)) return;
    int n = PARAM1(st, 1);
    ghostcon_screen_erase_chars(s, n);
}

static void handle_csi_insert_chars(ghostcon_stream_t *st, ghostcon_screen_t *s)
{
    if (!csi_param_count(st, 0, 1)) return;
    int n = PARAM1(st, 1);
    if (n == 0) n = 1; /* explicit zero clamps to 1 (matches Ghostty) */
    ghostcon_screen_insert_chars(s, n);
}

static void handle_csi_scroll_up(ghostcon_stream_t *st, ghostcon_screen_t *s)
{
    if (!csi_param_count(st, 0, 1)) return;
    int n = PARAM1(st, 1);
    ghostcon_screen_scroll_up(s, n);
}

static void handle_csi_scroll_down(ghostcon_stream_t *st, ghostcon_screen_t *s)
{
    if (!csi_param_count(st, 0, 1)) return;
    int n = PARAM1(st, 1);
    ghostcon_screen_scroll_down(s, n);
}

static void handle_csi_set_scroll_region(ghostcon_stream_t *st, ghostcon_screen_t *s)
{
    /* DECSTBM: 0, 1, or 2 params (Ghostty), else ignored. */
    if (!csi_param_count(st, 0, 2)) return;
    int top = PARAM1(st, 1) - 1;
    int bottom = PARAM2(st, 0) - 1;
    if (bottom <= 0) bottom = (int16_t)(s->rows_visible - 1);
    ghostcon_screen_set_scroll_region(s, top, bottom);
}

static void handle_csi_set_margin_region(ghostcon_stream_t *st, ghostcon_screen_t *s)
{
    /* CSI s with no params is SCOSC (save cursor); with params it is DECSLRM (set
     * left/right margins)
     *
     * matches Ghostty's ambiguity split
     */
    if (st->params_idx == 0)
    {
        ghostcon_screen_cursor_save(s);
        return;
    }
    /* DECSLRM: 1 or 2 params, else ignored (Ghostty). */
    if (!csi_param_count(st, 1, 2)) return;
    /* DECSLRM requires DECLRMM (mode 69) to be enabled */
    if (!s->left_right_margin)
        return;
    int left = PARAM1(st, 1) - 1;
    int right = PARAM2(st, 0) - 1;
    if (right <= 0) right = (int16_t)(s->cols - 1);
    ghostcon_screen_set_margin_region(s, left, right);
}

static void handle_csi_restore_cursor(ghostcon_stream_t *st, ghostcon_screen_t *s)
{
    (void)st;
    ghostcon_screen_cursor_restore(s);
}

/* Kitty keyboard protocol --https://sw.kovidgoyal.net/kitty/keyboard-protocol/
 * Shares the plain 'u' final byte with DECRC (CSI u, no intermediate, handled by
 * handle_csi_restore_cursor above)
 *
 * distinguished the same way DEC private modes (?) are told apart from ANSI ones,
 * via the leading intermediate byte (see csi_dispatch()'s own doc comment on
 * this).
 *
 * Tracked state (screen->kitty) previously existed but was never actually written
 * to by anything in this parser --core/input.c's key encoder was permanently
 * hardcoded to legacy mode as a result, ignoring whatever an app actually
 * negotiated.
 */

static void handle_csi_kitty_push(ghostcon_stream_t *st, ghostcon_screen_t *s)
{
    /* CSI > flags u
     *
     * push `flags` as a new stack entry.
     *
     * Omitted flags defaults to 0 (fully disabled), matching this parser's
     * general "absent numeric param defaults to the safe/off value" convention
     * used throughout.
     */
    ghostcon_kitty_push(&s->kitty, (uint8_t)PARAM1(st, 0));
}

static void handle_csi_kitty_pop(ghostcon_stream_t *st, ghostcon_screen_t *s)
{
    /* CSI < n u
     *
     * pop n stack entries (default 1 per the protocol spec, unlike most other
     * sequences in this file that default an omitted count to 0).
     */
    ghostcon_kitty_pop(&s->kitty, (uint8_t)PARAM1(st, 1));
}

static void handle_csi_kitty_set(ghostcon_stream_t *st, ghostcon_screen_t *s)
{
    /* CSI = flags ; mode u
     *
     * apply `flags` to the CURRENT (top-of-stack) entry per `mode`: wire protocol
     * 1=set (default), 2=OR, 3=AND-NOT. ghostcon_kitty_set_mode_t is 0-indexed
     * (GC_KITTY_SET=0/OR=1/NOT=2), hence the -1; any other wire value is invalid
     * per the spec and ignored, same as this file's other handlers silently
     * ignore an out-of-range mode.
     */
    int wire_mode = PARAM2(st, 1);
    if (wire_mode < 1 || wire_mode > 3)
        return;
    ghostcon_kitty_set_mode(&s->kitty, (ghostcon_kitty_set_mode_t)(wire_mode - 1),
                             (uint8_t)PARAM1(st, 0));
}

static void handle_csi_kitty_query(ghostcon_stream_t *st, ghostcon_screen_t *s)
{
    /* CSI ? u -- report the current (top-of-stack) flags as CSI ? {flags} u. */
    char buf[16];
    snprintf(buf, sizeof(buf), "\x1b[?%uu", (unsigned)ghostcon_kitty_current(&s->kitty));
    stream_output(st, buf);
}

static void handle_csi_cursor_style(ghostcon_stream_t *st, ghostcon_screen_t *s)
{
    if (!csi_param_count(st, 0, 1)) return;
    int style = PARAM1(st, 0);
    /* DECSCUSR: 0=blink block, 1=blink block, 2=steady block, 3=blink
     * underline, 4=steady underline, 5=blink bar, 6=steady bar
     */
    switch (style)
    {
    case 0: case 1: s->cursor.cursor_style = GC_CURSOR_BLOCK_BLINK; break;
    case 2: s->cursor.cursor_style = GC_CURSOR_BLOCK; break;
    case 3: s->cursor.cursor_style = GC_CURSOR_UNDERLINE_BLINK; break;
    case 4: s->cursor.cursor_style = GC_CURSOR_UNDERLINE; break;
    case 5: s->cursor.cursor_style = GC_CURSOR_BAR_BLINK; break;
    case 6: s->cursor.cursor_style = GC_CURSOR_BAR; break;
    default: break; /* out-of-range DECSCUSR values are ignored */
    }
}

static void handle_csi_decsca(ghostcon_stream_t *st, ghostcon_screen_t *s)
{
    /* DECSCA
     *
     * CSI Ps " q
     *
     * select character protection attribute. Ghostty (stream.zig): 0 or 1 param;
     * Ps 0 or 2 → off, Ps 1 → DEC.
     *
     * Note Ghostty maps Ps=2 to `.off`, NOT ISO
     *
     * the ISO screen mode is only reachable from internal callers, never from
     * DECSCA.
     *
     * The screen's protected_mode is left unchanged on off (Ghostty comment:
     * "NEVER reset to .off because logic such as eraseChars depends on knowing
     * what the most recent mode was").
     */
    if (!csi_param_count(st, 0, 1)) return;
    int mode = PARAM1(st, 0);
    switch (mode)
    {
    case 0:
    case 2:
        s->cursor.protected = false;
        break;
    case 1:
        s->cursor.protected = true;
        s->protected_mode = GC_PROTECTED_DEC;
        break;
    default:
        /* Ghostty: invalid value -> ignore */
        break;
    }
}

static void handle_csi_save_mode(ghostcon_stream_t *st, ghostcon_screen_t *s)
{
    /* CSI ? Ps s — save the given DEC modes */
    for (uint8_t i = 0; i < st->params_idx; i++)
        ghostcon_screen_save_mode(s, st->params[i]);
}

static void handle_csi_restore_mode(ghostcon_stream_t *st, ghostcon_screen_t *s)
{
    /* CSI ? Ps r — restore the given DEC modes */
    for (uint8_t i = 0; i < st->params_idx; i++)
        ghostcon_screen_restore_mode(s, st->params[i]);
}

static void handle_csi_shift_escape(ghostcon_stream_t *st, ghostcon_screen_t *s)
{
    /* XTSHIFTESCAPE — CSI > Ps s — mouse-shift capture on/off */
    switch (st->params_idx)
    {
    case 0: s->mouse_shift_capture = false; break;
    case 1:
        switch (PARAM1(st, 0))
        {
        case 0: s->mouse_shift_capture = false; break;
        case 1: s->mouse_shift_capture = true; break;
        default: break; /* invalid — Ghostty warns and ignores */
        }
        break;
    default:
        /* invalid — Ghostty warns and ignores */
        break;
    }
}

static void handle_csi_request_mode(ghostcon_stream_t *st, ghostcon_screen_t *s)
{
    /* DECRQM: CSI Ps $ p (ansi) / CSI ? Ps $ p (DEC)
     *
     * respond with DECRPM: CSI[?] Ps;Pn $ y. Pn: 0 = not recognized, 1 = set, 2 =
     * reset.
     */
    if (st->params_idx != 1) return;
    int mode = st->params[0];
    bool ansi = (st->intermediates_idx == 1 && st->intermediates[0] == '$');
    int state = 0; /* not recognized */
    char buf[32];

    if (ansi)
    {
        switch (mode)
        {
        case 4:  state = s->insert_mode ? 1 : 2; break;
        case 2:  state = 2; break;  /* KAM — not tracked */
        case 20: state = 2; break;  /* LNM — not tracked */
        default: state = 0; break;
        }
        snprintf(buf, sizeof(buf), "\x1b[%d;%d$y", mode, state);
    }
    else
    {
        switch (mode)
        {
        case 1:    state = s->application_cursor ? 1 : 2; break;
        case 4:    state = s->insert_mode ? 1 : 2; break;
        case 5:    state = s->reverse_video ? 1 : 2; break;
        case 6:    state = s->origin_mode ? 1 : 2; break;
        case 7:    state = s->auto_wrap ? 1 : 2; break;
        case 69:   state = s->left_right_margin ? 1 : 2; break;
        case 1000: case 1002: case 1003:
                   state = s->mouse_tracking ? 1 : 2; break;
        case 2004: state = s->bracketed_paste ? 1 : 2; break;
        case 2026: state = s->synchronized_output ? 1 : 2; break;
        default:   state = 0; break;
        }
        snprintf(buf, sizeof(buf), "\x1b[?%d;%d$y", mode, state);
    }
    stream_output(st, buf);
}

static void handle_csi_tab_clear(ghostcon_stream_t *st, ghostcon_screen_t *s)
{
    int mode = PARAM1(st, 0);
    ghostcon_screen_tab_clear(s, mode);
}

static void handle_csi_device_attrib(ghostcon_stream_t *st, ghostcon_screen_t *s)
{
    (void)s;
    /* DA1/DA2/DA3 responses — mirrors Ghostty's device_attributes.zig defaults. */
    switch (st->intermediates_idx)
    {
    case 0:
        /* DA1: VT220 level-2 + ANSI color */
        stream_output(st, "\x1b[?62;22c");
        break;
    case 1:
        if (st->intermediates[0] == '>')
        {
            /* DA2: VT220, fw rev 0, ROM 0 */
            stream_output(st, "\x1b[>1;0;0c");
        }
        else if (st->intermediates[0] == '=')
        {
            /* DA3: DECRPTUI 0 */
            stream_output(st, "\x1bP!|00000000\x1b\\");
        }
        break;
    default: break;
    }
}

static void handle_csi_device_status_report(ghostcon_stream_t *st, ghostcon_screen_t *s)
{
    if (st->params_idx != 1) return;
    int mode = st->params[0];
    bool question = (st->intermediates_idx == 1 && st->intermediates[0] == '?');
    char buf[32];
    if (!question)
    {
        switch (mode)
        {
        case 5:
            stream_output(st, "\x1b[0n");
            break;
        case 6: {
            /* CPR
             *
             * cursor position report (1-based). With origin mode coordinates are
             * relative to the scroll region (Ghostty).
             */
            int x = s->cursor.x, y = s->cursor.y;
            if (s->origin_mode) {
                x -= s->margin_region.left;
                y -= s->scroll_region.top;
                if (x < 0) x = 0;
                if (y < 0) y = 0;
            }
            snprintf(buf, sizeof(buf), "\x1b[%d;%dR", y + 1, x + 1);
            stream_output(st, buf);
            break;
        }
        default: break;
        }
    }
    else
    {
        switch (mode)
        {
        case 996:
            stream_output(st, "\x1b[?997;2n");  /* dark */
            break;
        case 998:
            stream_output(st, "\x1b[?999;1n");  /* visible */
            break;
        default: break;
        }
    }
}

/* Stamps when a synchronized-output batch (mode 2026) started, so core/main.c's
 * render gate can bound how long it withholds a frame if the app never sends the
 * closing ?2026l
 *
 * see screen.h's own doc comment on synchronized_output_since.
 */
static void mark_sync_output_start(ghostcon_screen_t *s)
{
    s->synchronized_output = true;
    clock_gettime(CLOCK_MONOTONIC, &s->synchronized_output_since);
}

static void handle_csi_set_mode(ghostcon_stream_t *st, ghostcon_screen_t *s)
{
    /* SM — Ghostty iterates ALL params (multi-mode sequences). */
    for (uint8_t i = 0; i < st->params_idx; i++)
    {
        switch (st->params[i])
        {
        case 4:  s->insert_mode = true; break;                    /* IRM */
        case 2004: s->bracketed_paste = true; break;
        case 2026: mark_sync_output_start(s); break;
        default: break; /* unrecognized mode numbers are ignored */
        }
    }
}

static void handle_csi_reset_mode(ghostcon_stream_t *st, ghostcon_screen_t *s)
{
    /* RM — iterate all params. */
    for (uint8_t i = 0; i < st->params_idx; i++)
    {
        switch (st->params[i])
        {
        case 4:  s->insert_mode = false; break;
        case 2004: s->bracketed_paste = false; break;
        case 2026: s->synchronized_output = false; break;
        default: break; /* unrecognized mode numbers are ignored */
        }
    }
}

static void handle_csi_dec_set(ghostcon_stream_t *st, ghostcon_screen_t *s)
{
    /* DECSET
     *
     * iterate all params (Ghostty). DECOM also homes the cursor on both set and
     * reset (Ghostty: `.origin => setCursorPos(1, 1)`).
     */
    for (uint8_t i = 0; i < st->params_idx; i++)
    {
        switch (st->params[i])
        {
        case 1: s->application_cursor = true; break;              /* DECCKM */
        case 3: break; /* DECCOLM — 132 cols — ignore */
        case 5: s->reverse_video = true; break;                    /* DECSCNM */
        case 6: s->origin_mode = true; ghostcon_screen_cursor_set(s, 0, 0); break; /* DECOM */
        case 7: s->auto_wrap = true; break;                        /* DECAWM */
        case 23: /* DECSSD — end margin mode */ break;
        case 25: s->cursor_visible = true; break;                   /* DECTCEM */
        case 47: ghostcon_screen_alt_screen_enter(s); break;      /* alt screen (legacy) */
        case 69: s->left_right_margin = true; break;              /* DECLRMM */
        case 1000: s->mouse_tracking = true; s->mouse_protocol = 1000; break;
        case 1002: s->mouse_tracking = true; s->mouse_protocol = 1002; break;
        case 1003: s->mouse_tracking = true; s->mouse_protocol = 1003; break;
        case 1005: /* URXVT extension */ break;
        case 1006: s->mouse_sgr = true; break;                     /* SGR mouse mode */
        case 2026: mark_sync_output_start(s); break;                /* synchronized output */
        case 1047: ghostcon_screen_alt_screen_enter(s); break;     /* alt screen */
        case 1048: ghostcon_screen_cursor_save(s); break;         /* save cursor */
        case 1049: ghostcon_screen_cursor_save(s); ghostcon_screen_alt_screen_enter(s); break; /* alt screen + save cursor */
        default: break; /* unrecognized DEC private modes are ignored */
        }
    }
}

static void handle_csi_dec_reset(ghostcon_stream_t *st, ghostcon_screen_t *s)
{
    /* DECRST — iterate all params. */
    for (uint8_t i = 0; i < st->params_idx; i++)
    {
        switch (st->params[i])
        {
        case 1: s->application_cursor = false; break;
        case 3: break;
        case 5: s->reverse_video = false; break;
        case 6: s->origin_mode = false; ghostcon_screen_cursor_set(s, 0, 0); break;
        case 7: s->auto_wrap = false; break;                       /* DECAWM */
        case 25: s->cursor_visible = false; break;                  /* DECTCEM */
        case 47: ghostcon_screen_alt_screen_exit(s); break;        /* alt screen (legacy) */
        case 69:                                                    /* DECLRMM */
            s->left_right_margin = false;
            /* put_char()'s wrap boundary applies margin_region.right
             * unconditionally, without checking left_right_margin --so disabling
             * DECLRMM without also resetting the region left a stale DECSLRM
             * margin permanently in effect.
             *
             * Found live: this made every subsequent line wrap at whatever column
             * DECSLRM last set, indefinitely.
             */
            ghostcon_screen_set_margin_region(s, -1, -1);
            break;
        case 1000: case 1002: case 1003: s->mouse_tracking = false; s->mouse_protocol = 0; break;
        case 1006: s->mouse_sgr = false; break;
        case 2026: s->synchronized_output = false; break;          /* synchronized output */
        case 1047: ghostcon_screen_alt_screen_exit(s); break;      /* alt screen */
        case 1048: ghostcon_screen_cursor_restore(s); break;      /* restore cursor */
        case 1049: ghostcon_screen_alt_screen_exit(s); ghostcon_screen_cursor_restore(s); break; /* alt screen + restore cursor */
        default: break; /* unrecognized DEC private modes are ignored */
        }
    }
}

/* SGR handler (CSI m) */
static void handle_csi_sgr(ghostcon_stream_t *st, ghostcon_screen_t *s)
{
    GhosttySgrParser parser;
    if (ghostty_sgr_new(NULL, &parser) != GHOSTTY_SUCCESS)
        return;

    /* Feed params */
    ghostty_sgr_set_params(parser, st->params, NULL, st->params_idx);

    /* Process each attribute */
    ghostcon_style_t cur;
    if (s->cursor.style_id == GC_STYLE_DEFAULT_ID)
        cur = GHOSTCON_STYLE_DEFAULT;
    else
        cur = *ghostcon_style_set_get(s->styles, s->cursor.style_id);

    GhosttySgrAttribute attr;
    while (ghostty_sgr_next(parser, &attr))
    {
        switch (attr.tag)
        {
        case GHOSTTY_SGR_ATTR_UNSET:
            /* SGR 0 — reset all attributes */
            cur = GHOSTCON_STYLE_DEFAULT;
            break;
        case GHOSTTY_SGR_ATTR_BOLD:          cur.flags |= GC_STYLE_BOLD; break;
        case GHOSTTY_SGR_ATTR_RESET_BOLD:    cur.flags &= ~GC_STYLE_BOLD; break;
        case GHOSTTY_SGR_ATTR_FAINT:         cur.flags |= GC_STYLE_DIM; break;
        case GHOSTTY_SGR_ATTR_ITALIC:        cur.flags |= GC_STYLE_ITALIC; break;
        case GHOSTTY_SGR_ATTR_RESET_ITALIC:  cur.flags &= ~GC_STYLE_ITALIC; break;
        case GHOSTTY_SGR_ATTR_UNDERLINE:
            cur.flags |= GC_STYLE_UNDERLINE;
            cur.underline = (ghostcon_style_underline_t)attr.value.underline;
            break;
        case GHOSTTY_SGR_ATTR_BLINK:         cur.flags |= GC_STYLE_BLINK; break;
        case GHOSTTY_SGR_ATTR_RESET_BLINK:   cur.flags &= ~GC_STYLE_BLINK; break;
        case GHOSTTY_SGR_ATTR_INVERSE:       cur.flags |= GC_STYLE_INVERSE; break;
        case GHOSTTY_SGR_ATTR_RESET_INVERSE: cur.flags &= ~GC_STYLE_INVERSE; break;
        case GHOSTTY_SGR_ATTR_INVISIBLE:     cur.flags |= GC_STYLE_HIDDEN; break;
        case GHOSTTY_SGR_ATTR_RESET_INVISIBLE: cur.flags &= ~GC_STYLE_HIDDEN; break;
        case GHOSTTY_SGR_ATTR_STRIKETHROUGH: cur.flags |= GC_STYLE_STRIKETHROUGH; break;
        case GHOSTTY_SGR_ATTR_RESET_STRIKETHROUGH: cur.flags &= ~GC_STYLE_STRIKETHROUGH; break;
        case GHOSTTY_SGR_ATTR_OVERLINE:      cur.flags |= GC_STYLE_OVERLINE; break;
        case GHOSTTY_SGR_ATTR_RESET_OVERLINE: cur.flags &= ~GC_STYLE_OVERLINE; break;

        case GHOSTTY_SGR_ATTR_FG_8:
            cur.flags &= ~(GC_STYLE_FG_TRUECOLOR | GC_STYLE_FG_DEFAULT);
            cur.fg_palette = (uint8_t)attr.value.fg_8;
            break;
        case GHOSTTY_SGR_ATTR_BG_8:
            cur.flags &= ~(GC_STYLE_BG_TRUECOLOR | GC_STYLE_BG_DEFAULT);
            cur.bg_palette = (uint8_t)attr.value.bg_8;
            break;
        case GHOSTTY_SGR_ATTR_BRIGHT_FG_8:
            cur.flags &= ~(GC_STYLE_FG_TRUECOLOR | GC_STYLE_FG_DEFAULT);
            /* bright_fg_8 is already the absolute 0-15 palette index (e.g. 9 for
             * bright red / SGR 91, matching GHOSTTY_COLOR_NAMED_BRIGHT_RED)
             *
             * NOT a 0-7 offset that needs +8.
             *
             * Adding 8 here double-offset it (9+8=17), which isn't a named color
             * at all
             *
             * it lands inside the 216 color cube (indices 16-231) at a
             * essentially arbitrary color.
             *
             * Confirmed against libghostty-vt directly: see PLAN.md for the live
             * repro (bright red rendering as dark blue) and the standalone parser
             * test that caught this.
             */
            cur.fg_palette = (uint8_t)attr.value.bright_fg_8;
            break;
        case GHOSTTY_SGR_ATTR_BRIGHT_BG_8:
            cur.flags &= ~(GC_STYLE_BG_TRUECOLOR | GC_STYLE_BG_DEFAULT);
            cur.bg_palette = (uint8_t)attr.value.bright_bg_8;
            break;
        case GHOSTTY_SGR_ATTR_FG_256:
            cur.flags &= ~(GC_STYLE_FG_TRUECOLOR | GC_STYLE_FG_DEFAULT);
            cur.fg_palette = (uint8_t)attr.value.fg_256;
            break;
        case GHOSTTY_SGR_ATTR_BG_256:
            cur.flags &= ~(GC_STYLE_BG_TRUECOLOR | GC_STYLE_BG_DEFAULT);
            cur.bg_palette = (uint8_t)attr.value.bg_256;
            break;
        case GHOSTTY_SGR_ATTR_DIRECT_COLOR_FG:
            cur.flags |= GC_STYLE_FG_TRUECOLOR;
            cur.flags &= ~GC_STYLE_FG_DEFAULT;
            cur.fg_rgb.r = attr.value.direct_color_fg.r;
            cur.fg_rgb.g = attr.value.direct_color_fg.g;
            cur.fg_rgb.b = attr.value.direct_color_fg.b;
            break;
        case GHOSTTY_SGR_ATTR_DIRECT_COLOR_BG:
            cur.flags |= GC_STYLE_BG_TRUECOLOR;
            cur.flags &= ~GC_STYLE_BG_DEFAULT;
            cur.bg_rgb.r = attr.value.direct_color_bg.r;
            cur.bg_rgb.g = attr.value.direct_color_bg.g;
            cur.bg_rgb.b = attr.value.direct_color_bg.b;
            break;
        case GHOSTTY_SGR_ATTR_UNDERLINE_COLOR:
            cur.flags |= GC_STYLE_UNDERLINE_TRUECOLOR;
            cur.ul_rgb.r = attr.value.underline_color.r;
            cur.ul_rgb.g = attr.value.underline_color.g;
            cur.ul_rgb.b = attr.value.underline_color.b;
            break;
        case GHOSTTY_SGR_ATTR_UNDERLINE_COLOR_256:
            cur.flags &= ~GC_STYLE_UNDERLINE_TRUECOLOR;
            cur.underline = (uint8_t)attr.value.underline_color_256;
            break;
        case GHOSTTY_SGR_ATTR_RESET_UNDERLINE_COLOR:
            cur.flags &= ~GC_STYLE_UNDERLINE_TRUECOLOR;
            break;
        case GHOSTTY_SGR_ATTR_RESET_FG:
            cur.flags &= ~GC_STYLE_FG_TRUECOLOR;
            cur.flags |= GC_STYLE_FG_DEFAULT;
            break;
        case GHOSTTY_SGR_ATTR_RESET_BG:
            cur.flags &= ~GC_STYLE_BG_TRUECOLOR;
            cur.flags |= GC_STYLE_BG_DEFAULT;
            break;
        default: break;
        }
    }

    ghostty_sgr_free(parser);
    s->cursor.style_id = ghostcon_style_set_add(s->styles, &cur);
}

/* CSI dispatch table
 *
 * One row per (final byte, intermediates) pair.
 *
 * Intermediate discrimination mirrors Ghostty's `switch
 * (input.intermediates.len)` structure (stream.zig).
 *
 * The private markers < = > ? are collected into intermediates when
 * they lead a sequence, so DEC private, DA2/DA3, XTSHIFTESCAPE, DECSCA,
 * and DECSED/DECSEL are all told apart here.
 */
typedef struct
{
    uint8_t                final;     /* the final byte */
    uint8_t                inter_len; /* exact number of intermediates */
    uint8_t                inter0;    /* first intermediate, when inter_len >= 1 */
    uint8_t                inter1;    /* second intermediate, when inter_len >= 2 */
    gc_stream_csi_handler_t fn;
} csi_entry_t;

static const csi_entry_t csi_table[] = {
    /* Sorted by final byte, so csi_lookup() can binary-search for the
     * first row of a final and walk its variants from there. Keep it
     * sorted: an out-of-order row is dispatched wrongly, not rejected.
     *
     * 't' (window ops) has no handler.
     */
    { '@', 0, 0, 0, handle_csi_insert_chars },
    { 'A', 0, 0, 0, handle_csi_cursor_up },
    { 'B', 0, 0, 0, handle_csi_cursor_down },
    { 'C', 0, 0, 0, handle_csi_cursor_forward },
    { 'D', 0, 0, 0, handle_csi_cursor_back },
    { 'E', 0, 0, 0, handle_csi_cursor_next_line },
    { 'F', 0, 0, 0, handle_csi_cursor_prev_line },
    { 'G', 0, 0, 0, handle_csi_cursor_horizontal_abs },
    { 'H', 0, 0, 0, handle_csi_cursor_position },
    { 'I', 0, 0, 0, handle_csi_tab_forward },
    { 'J', 0, 0, 0, handle_csi_erase_display },
    { 'J', 1, '?', 0, handle_csi_erase_display },
    { 'K', 0, 0, 0, handle_csi_erase_line },
    { 'K', 1, '?', 0, handle_csi_erase_line },
    { 'L', 0, 0, 0, handle_csi_insert_lines },
    { 'M', 0, 0, 0, handle_csi_delete_lines },
    { 'P', 0, 0, 0, handle_csi_delete_chars },
    { 'S', 0, 0, 0, handle_csi_scroll_up },
    { 'T', 0, 0, 0, handle_csi_scroll_down },
    { 'X', 0, 0, 0, handle_csi_erase_chars },
    { 'Z', 0, 0, 0, handle_csi_tab_back },
    { '`', 0, 0, 0, handle_csi_cursor_horizontal_abs },
    { 'a', 0, 0, 0, handle_csi_cursor_forward },
    { 'b', 0, 0, 0, handle_csi_repeat },
    { 'c', 0, 0, 0, handle_csi_device_attrib },
    { 'c', 1, '>', 0, handle_csi_device_attrib },
    { 'c', 1, '=', 0, handle_csi_device_attrib },
    { 'd', 0, 0, 0, handle_csi_cursor_vertical_abs },
    { 'e', 0, 0, 0, handle_csi_cursor_down },
    { 'f', 0, 0, 0, handle_csi_horizontal_vertical_position },
    { 'g', 0, 0, 0, handle_csi_tab_clear },
    { 'h', 0, 0, 0, handle_csi_set_mode },
    { 'h', 1, '?', 0, handle_csi_dec_set },
    { 'l', 0, 0, 0, handle_csi_reset_mode },
    { 'l', 1, '?', 0, handle_csi_dec_reset },
    { 'm', 0, 0, 0, handle_csi_sgr },
    { 'n', 0, 0, 0, handle_csi_device_status_report },
    { 'p', 1, '$', 0, handle_csi_request_mode },
    { 'p', 2, '?', '$', handle_csi_request_mode },
    { 'q', 1, ' ', 0, handle_csi_cursor_style },
    { 'q', 1, '"', 0, handle_csi_decsca },
    { 'r', 0, 0, 0, handle_csi_set_scroll_region },
    { 'r', 1, '?', 0, handle_csi_restore_mode },
    { 's', 0, 0, 0, handle_csi_set_margin_region },
    { 's', 1, '?', 0, handle_csi_save_mode },
    { 's', 1, '>', 0, handle_csi_shift_escape },
    { 'u', 0, 0, 0, handle_csi_restore_cursor },
    { 'u', 1, '>', 0, handle_csi_kitty_push },
    { 'u', 1, '<', 0, handle_csi_kitty_pop },
    { 'u', 1, '=', 0, handle_csi_kitty_set },
    { 'u', 1, '?', 0, handle_csi_kitty_query },
};

bool gc_stream_csi_table_sorted(void)
{
    /* The table is searched by binary search, so it has to stay sorted by final
     * byte, and no two rows may be identical -- an out-of-order or duplicated row
     * is dispatched wrongly rather than rejected, and a CSI sequence the table
     * does not match is silently ignored.
     *
     * Exposed so a test can check the invariant instead of it living only in a
     * comment.
     */
    const size_t n = sizeof(csi_table) / sizeof(csi_table[0]);
    for (size_t i = 1; i < n; i++)
    {
        const csi_entry_t *a = &csi_table[i - 1];
        const csi_entry_t *b = &csi_table[i];
        if (a->final > b->final)
            return false;
        if (a->final == b->final &&
            a->inter_len == b->inter_len &&
            a->inter0 == b->inter0 &&
            a->inter1 == b->inter1)
            return false;
    }
    return true;
}

static gc_stream_csi_handler_t
csi_lookup(ghostcon_stream_t *st, uint8_t final_byte)
{
    /* Ghostty drops any CSI sequence whose parameter count reached MAX_PARAMS
     * (24)
     *
     * `params_idx >= Parser.MAX_PARAMS` returns before dispatch in
     * csiDispatchFinal.
     */
    if (st->params_idx >= GC_STREAM_MAX_PARAMS)
        return NULL;

    const uint8_t inter_len = st->intermediates_idx;
    const uint8_t inter0 = (inter_len > 0) ? st->intermediates[0] : 0;
    const uint8_t inter1 = (inter_len > 1) ? st->intermediates[1] : 0;

    const size_t n = sizeof(csi_table) / sizeof(csi_table[0]);
    size_t lo = 0;
    size_t hi = n;
    while (lo < hi)
    {
        size_t mid = lo + (hi - lo) / 2;
        if (csi_table[mid].final < final_byte)
            lo = mid + 1;
        else
            hi = mid;
    }

    for (size_t i = lo; i < n; i++)
    {
        const csi_entry_t *e = &csi_table[i];
        if (e->final != final_byte)
            break;  /* past the last row for this final */
        if (e->inter_len != inter_len)
            continue;
        if (e->inter_len >= 1 && e->inter0 != inter0)
            continue;
        if (e->inter_len >= 2 && e->inter1 != inter1)
            continue;
        return e->fn;
    }
    return NULL;
}

gc_stream_csi_handler_t
gc_stream_csi_dispatch(ghostcon_stream_t *st, uint8_t final_byte)
{
    gc_stream_csi_handler_t h = csi_lookup(st, final_byte);
    if (!h)
        GC_LOG(stream, "unhandled CSI final '%c' (%s params)",
                final_byte, st->params_idx ? "with" : "without");
    return h;
}
