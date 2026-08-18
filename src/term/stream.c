#define _DEFAULT_SOURCE /* CLOCK_MONOTONIC under -std=c11 without this */
#include "ghostcon/term/stream.h"
#include "ghostcon/term/cell.h"
#include <ghostty/vt/sgr.h>
#include <ghostty/vt/osc.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

bool
ghostcon_stream_init(ghostcon_stream_t *st) {
    memset(st, 0, sizeof(*st));
    st->state = GC_STREAM_GROUND;
    st->buf = st->local_buf;
    st->buf_cap = sizeof(st->local_buf);
    return true;
}

void
ghostcon_stream_deinit(ghostcon_stream_t *st) {
    if (st->buf && st->buf != st->local_buf)
        free(st->buf);
    memset(st, 0, sizeof(*st));
}

void
ghostcon_stream_set_output(ghostcon_stream_t *st,
                           ghostcon_output_fn fn, void *userdata) {
    st->output_fn = fn;
    st->output_userdata = userdata;
}

void
ghostcon_stream_set_title(ghostcon_stream_t *st,
                          ghostcon_title_fn fn, void *userdata) {
    st->title_fn = fn;
    st->title_userdata = userdata;
}

void
ghostcon_stream_set_notify(ghostcon_stream_t *st,
                           ghostcon_notify_fn fn, void *userdata) {
    st->notify_fn = fn;
    st->notify_userdata = userdata;
}

/* ------------------------------------------------------------------ */
/* UTF-8 decoder                                                       */
/*                                                                     */
/* Bjoern Hoehrmann's DFA-based, error-replacing UTF-8 decoder,        */
/* ported directly from Ghostty's UTF8Decoder.zig (which is itself     */
/* based on Hoehrmann's work).                                         */
/*                                                                     */
/* Copyright (c) 2008-2009 Bjoern Hoehrmann <bjoern@hoehrmann.de>      */
/* See http://bjoern.hoehrmann.de/utf-8/decoder/dfa/ for details.      */
/*                                                                     */
/* Permission is hereby granted, free of charge, to any person         */
/* obtaining a copy of this software and associated documentation      */
/* files (the "Software"), to deal in the Software without             */
/* restriction, including without limitation the rights to use, copy,  */
/* modify, merge, publish, distribute, sublicense, and/or sell copies  */
/* of the Software, and to permit persons to whom the Software is      */
/* furnished to do so, subject to the following conditions:            */
/*                                                                     */
/* The above copyright notice and this permission notice shall be      */
/* included in all copies or substantial portions of the Software.     */
/*                                                                     */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,     */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF  */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND               */
/* NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT         */
/* HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,        */
/* WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,  */
/* OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER       */
/* DEALINGS IN THE SOFTWARE.                                           */
/*                                                                     */
/* MIT is GPL-compatible; the combined work remains GPL-2.0-or-later.  */
/* ------------------------------------------------------------------ */

#define UTF8_ACCEPT 0
#define UTF8_REJECT 12

static const uint8_t utf8_char_classes[256] = {
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,  9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,
    7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,  7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,
    8,8,2,2,2,2,2,2,2,2,2,2,2,2,2,2,  2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,
   10,3,3,3,3,3,3,3,3,3,3,3,3,4,3,3, 11,6,6,6,5,8,8,8,8,8,8,8,8,8,8,8,
};

static const uint8_t utf8_transitions[108] = {
    0,12,24,36,60,96,84,12,12,12,48,72, 12,12,12,12,12,12,12,12,12,12,12,12,
   12, 0,12,12,12,12,12, 0,12, 0,12,12, 12,24,12,12,12,12,12,24,12,24,12,12,
   12,12,12,12,12,12,12,24,12,12,12,12, 12,24,12,12,12,12,12,12,12,24,12,12,
   12,12,12,12,12,12,12,36,12,36,12,12, 12,36,12,12,12,12,12,36,12,36,12,12,
   12,36,12,12,12,12,12,12,12,12,12,12,
};

/* Emit *cp (0xFFFFFFFF = no codepoint emitted this byte). Returns true
   if the byte was consumed (false only on mid-sequence reject, in which
   case the caller must retry the same byte). */
static inline bool
utf8_next(ghostcon_stream_t *st, uint8_t byte, uint32_t *cp) {
    const uint8_t cls = utf8_char_classes[byte];
    const uint8_t initial_state = st->utf8_state;

    if (st->utf8_state != UTF8_ACCEPT) {
        st->utf8_acc = (st->utf8_acc << 6) | (byte & 0x3F);
    } else {
        st->utf8_acc = ((uint32_t)0xFF >> cls) & byte;
    }

    st->utf8_state = utf8_transitions[st->utf8_state + cls];

    if (st->utf8_state == UTF8_ACCEPT) {
        *cp = st->utf8_acc;
        st->utf8_acc = 0;
        return true;
    } else if (st->utf8_state == UTF8_REJECT) {
        st->utf8_acc = 0;
        st->utf8_state = UTF8_ACCEPT;
        *cp = 0xFFFD; /* replacement character */
        return initial_state == UTF8_ACCEPT;
    } else {
        *cp = 0xFFFFFFFF; /* none — mid-sequence */
        return true;
    }
}

/* Execute a C0 control character (mirrors Ghostty's stream.execute). */
static void
stream_execute_c0(ghostcon_stream_t *st, ghostcon_screen_t *s, uint8_t c) {
    (void)st;
    switch (c) {
    case 0x07: /* BEL */ break;
    case 0x08: /* BS */ ghostcon_screen_cursor_left(s, 1); break;
    case 0x09: /* HT */ ghostcon_screen_tab(s); break;
    case 0x0A: /* LF */ ghostcon_screen_linefeed(s); break;
    case 0x0B: /* VT */ ghostcon_screen_linefeed(s); break;
    case 0x0C: /* FF */ ghostcon_screen_linefeed(s); break;
    case 0x0D: /* CR */ ghostcon_screen_carriage_return(s); break;
    case 0x0E: /* SO */ break;
    case 0x0F: /* SI */ break;
    default:   /* other C0 ignored */ break;
    }
}

/* Route a decoded codepoint: ESC to escape state, C0 to execute,
   DEL ignored, everything else printed. */
static void
stream_handle_codepoint(ghostcon_stream_t *st, ghostcon_screen_t *s, uint32_t cp) {
    if (cp == 0x1B) {
        st->state = GC_STREAM_ESC;
        return;
    }
    if (cp < 0x20) {
        stream_execute_c0(st, s, (uint8_t)cp);
        return;
    }
    if (cp == 0x7F) return; /* DEL */
    ghostcon_screen_put_char(s, cp);
}

void
ghostcon_stream_reset(ghostcon_stream_t *st) {
    st->state = GC_STREAM_GROUND;
    st->intermediates_idx = 0;
    st->params_idx = 0;
    st->param_acc = 0;
    st->param_acc_idx = 0;
    st->utf8_acc = 0;
    st->utf8_state = UTF8_ACCEPT;
    st->osc_len = 0;
    st->osc_pending = false;
    st->osc_terminated_by_bel = false;
    st->dcs_len = 0;
}

/* ------------------------------------------------------------------ */
/* CSI parameter parsing                                               */
/* ------------------------------------------------------------------ */

/* Parse a collected CSI parameter. Returns default if empty. */
static int
param(ghostcon_stream_t *st, int idx, int def) {
    if (idx < st->params_idx && st->params[idx] != 0)
        return st->params[idx];
    return def;
}

#define PARAM(st, idx, def) param(st, idx, def)
#define PARAM1(st, def) param(st, 0, def)
#define PARAM2(st, def) param(st, 1, def)

/* Ghostty drops CSI sequences whose parameter count doesn't match the
   handler's expected range (stream.zig `switch (input.params.len)` with
   an `else => return`). mirror that here. */
static inline bool
csi_param_count(ghostcon_stream_t *st, uint8_t minc, uint8_t maxc) {
    return st->params_idx >= minc && st->params_idx <= maxc;
}

/* ------------------------------------------------------------------ */
/* Output channel helpers                                              */
/* ------------------------------------------------------------------ */

static void
stream_output(ghostcon_stream_t *st, const char *s) {
    if (st->output_fn)
        st->output_fn(st->output_userdata, (const uint8_t *)s, strlen(s));
}

/* ------------------------------------------------------------------ */
/* CSI dispatch handlers                                               */
/* ------------------------------------------------------------------ */

/* A raw CSI param can be up to UINT16_MAX (65535). Handlers that apply
   it via an O(n) per-step loop (rather than O(1) arithmetic or a single
   memmove) must not loop that many times just to hang synchronously in
   the single-threaded render/input loop -- clamp to the largest count
   that can still change anything observable. */
static int
clamp_loop_count(int n, int max_useful) {
    if (n > max_useful) return max_useful;
    if (n < 0) return 0;
    return n;
}

static void
handle_csi_cursor_up(ghostcon_stream_t *st, ghostcon_screen_t *s) {
    if (!csi_param_count(st, 0, 1)) return;
    int n = PARAM1(st, 1);
    ghostcon_screen_cursor_up(s, n);
}

static void
handle_csi_cursor_down(ghostcon_stream_t *st, ghostcon_screen_t *s) {
    if (!csi_param_count(st, 0, 1)) return;
    int n = PARAM1(st, 1);
    ghostcon_screen_cursor_down(s, n);
}

static void
handle_csi_cursor_forward(ghostcon_stream_t *st, ghostcon_screen_t *s) {
    if (!csi_param_count(st, 0, 1)) return;
    int n = PARAM1(st, 1);
    ghostcon_screen_cursor_right(s, n);
}

static void
handle_csi_cursor_back(ghostcon_stream_t *st, ghostcon_screen_t *s) {
    if (!csi_param_count(st, 0, 1)) return;
    int n = PARAM1(st, 1);
    ghostcon_screen_cursor_left(s, n);
}

static void
handle_csi_cursor_next_line(ghostcon_stream_t *st, ghostcon_screen_t *s) {
    if (!csi_param_count(st, 0, 1)) return;
    int n = clamp_loop_count(PARAM1(st, 1), s->rows_visible);
    for (int i = 0; i < n; i++)
        ghostcon_screen_cursor_next_line(s);
}

static void
handle_csi_cursor_prev_line(ghostcon_stream_t *st, ghostcon_screen_t *s) {
    if (!csi_param_count(st, 0, 1)) return;
    int n = clamp_loop_count(PARAM1(st, 1), s->rows_visible);
    for (int i = 0; i < n; i++)
        ghostcon_screen_cursor_prev_line(s);
}

static void
handle_csi_cursor_horizontal_abs(ghostcon_stream_t *st, ghostcon_screen_t *s) {
    if (!csi_param_count(st, 0, 1)) return;
    int x = PARAM1(st, 1) - 1;
    ghostcon_screen_cursor_horizontal_abs(s, x);
}

static void
handle_csi_cursor_vertical_abs(ghostcon_stream_t *st, ghostcon_screen_t *s) {
    /* VPA (CSI Ps d): set row, column unchanged. */
    if (!csi_param_count(st, 0, 1)) return;
    int y = PARAM1(st, 1) - 1;
    ghostcon_screen_cursor_vertical_abs(s, y);
}

static void
handle_csi_tab_forward(ghostcon_stream_t *st, ghostcon_screen_t *s) {
    /* CHT (CSI Ps I): move forward Ps tab stops. */
    if (!csi_param_count(st, 0, 1)) return;
    int n = clamp_loop_count(PARAM1(st, 1), s->cols);
    for (int i = 0; i < n; i++)
        ghostcon_screen_tab(s);
}

static void
handle_csi_tab_back(ghostcon_stream_t *st, ghostcon_screen_t *s) {
    /* CBT (CSI Ps Z): move backward Ps tab stops. */
    if (!csi_param_count(st, 0, 1)) return;
    int n = clamp_loop_count(PARAM1(st, 1), s->cols);
    for (int i = 0; i < n; i++)
        ghostcon_screen_tab_back(s);
}

static void
handle_csi_repeat(ghostcon_stream_t *st, ghostcon_screen_t *s) {
    /* REP (CSI Ps b): repeat preceding graphic character Ps times.
       Ghostty: @max(count, 1). No last-char → no-op. */
    if (!csi_param_count(st, 0, 1)) return;
    if (s->last_codepoint == 0) return;
    int n = PARAM1(st, 1);
    if (n == 0) n = 1;
    /* Beyond one full screen, further repeats just keep scrolling the
       same character -- clamp instead of hanging synchronously. */
    n = clamp_loop_count(n, (int)s->cols * (int)s->rows_visible);
    for (int i = 0; i < n; i++)
        ghostcon_screen_put_char(s, s->last_codepoint);
}

static void
handle_csi_cursor_position(ghostcon_stream_t *st, ghostcon_screen_t *s) {
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

static void
handle_csi_horizontal_vertical_position(ghostcon_stream_t *st, ghostcon_screen_t *s) {
    handle_csi_cursor_position(st, s);
}

static void
handle_csi_erase_display(ghostcon_stream_t *st, ghostcon_screen_t *s) {
    if (!csi_param_count(st, 0, 1)) return;
    int mode = PARAM1(st, 0);
    bool dec = (st->intermediates_idx == 1 && st->intermediates[0] == '?');
    bool respect = dec || (s->protected_mode == GC_PROTECTED_ISO);
    if (respect)
        ghostcon_screen_erase_display_protected(s, mode);
    else
        ghostcon_screen_erase_display(s, mode);
}

static void
handle_csi_erase_line(ghostcon_stream_t *st, ghostcon_screen_t *s) {
    if (!csi_param_count(st, 0, 1)) return;
    int mode = PARAM1(st, 0);
    bool dec = (st->intermediates_idx == 1 && st->intermediates[0] == '?');
    bool respect = dec || (s->protected_mode == GC_PROTECTED_ISO);
    if (respect)
        ghostcon_screen_erase_line_protected(s, mode);
    else
        ghostcon_screen_erase_line(s, mode);
}

static void
handle_csi_insert_lines(ghostcon_stream_t *st, ghostcon_screen_t *s) {
    if (!csi_param_count(st, 0, 1)) return;
    int n = PARAM1(st, 1);
    ghostcon_screen_insert_lines(s, n);
}

static void
handle_csi_delete_lines(ghostcon_stream_t *st, ghostcon_screen_t *s) {
    if (!csi_param_count(st, 0, 1)) return;
    int n = PARAM1(st, 1);
    ghostcon_screen_delete_lines(s, n);
}

static void
handle_csi_delete_chars(ghostcon_stream_t *st, ghostcon_screen_t *s) {
    if (!csi_param_count(st, 0, 1)) return;
    int n = PARAM1(st, 1);
    ghostcon_screen_delete_chars(s, n);
}

static void
handle_csi_erase_chars(ghostcon_stream_t *st, ghostcon_screen_t *s) {
    if (!csi_param_count(st, 0, 1)) return;
    int n = PARAM1(st, 1);
    ghostcon_screen_erase_chars(s, n);
}

static void
handle_csi_insert_chars(ghostcon_stream_t *st, ghostcon_screen_t *s) {
    if (!csi_param_count(st, 0, 1)) return;
    int n = PARAM1(st, 1);
    if (n == 0) n = 1; /* explicit zero clamps to 1 (matches Ghostty) */
    ghostcon_screen_insert_chars(s, n);
}

static void
handle_csi_scroll_up(ghostcon_stream_t *st, ghostcon_screen_t *s) {
    if (!csi_param_count(st, 0, 1)) return;
    int n = PARAM1(st, 1);
    ghostcon_screen_scroll_up(s, n);
}

static void
handle_csi_scroll_down(ghostcon_stream_t *st, ghostcon_screen_t *s) {
    if (!csi_param_count(st, 0, 1)) return;
    int n = PARAM1(st, 1);
    ghostcon_screen_scroll_down(s, n);
}

static void
handle_csi_set_scroll_region(ghostcon_stream_t *st, ghostcon_screen_t *s) {
    /* DECSTBM: 0, 1, or 2 params (Ghostty), else ignored. */
    if (!csi_param_count(st, 0, 2)) return;
    int top = PARAM1(st, 1) - 1;
    int bottom = PARAM2(st, 0) - 1;
    if (bottom <= 0) bottom = (int16_t)(s->rows_visible - 1);
    ghostcon_screen_set_scroll_region(s, top, bottom);
}

static void
handle_csi_set_margin_region(ghostcon_stream_t *st, ghostcon_screen_t *s) {
    /* CSI s with no params is SCOSC (save cursor); with params it is
       DECSLRM (set left/right margins) — matches Ghostty's ambiguity split */
    if (st->params_idx == 0) {
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

static void
handle_csi_restore_cursor(ghostcon_stream_t *st, ghostcon_screen_t *s) {
    (void)st;
    ghostcon_screen_cursor_restore(s);
}

/* Kitty keyboard protocol -- https://sw.kovidgoyal.net/kitty/keyboard-protocol/
   Shares the plain 'u' final byte with DECRC (CSI u, no intermediate,
   handled by handle_csi_restore_cursor above) -- distinguished the
   same way DEC private modes (?) are told apart from ANSI ones,
   via the leading intermediate byte (see csi_dispatch()'s own doc
   comment on this). Tracked state (screen->kitty) previously existed
   but was never actually written to by anything in this parser --
   core/input.c's key encoder was permanently hardcoded to legacy mode
   as a result, ignoring whatever an app actually negotiated. */

static void
handle_csi_kitty_push(ghostcon_stream_t *st, ghostcon_screen_t *s) {
    /* CSI > flags u -- push `flags` as a new stack entry. Omitted
       flags defaults to 0 (fully disabled), matching this parser's
       general "absent numeric param defaults to the safe/off value"
       convention used throughout. */
    ghostcon_kitty_push(&s->kitty, (uint8_t)PARAM1(st, 0));
}

static void
handle_csi_kitty_pop(ghostcon_stream_t *st, ghostcon_screen_t *s) {
    /* CSI < n u -- pop n stack entries (default 1 per the protocol
       spec, unlike most other sequences in this file that default
       an omitted count to 0). */
    ghostcon_kitty_pop(&s->kitty, (uint8_t)PARAM1(st, 1));
}

static void
handle_csi_kitty_set(ghostcon_stream_t *st, ghostcon_screen_t *s) {
    /* CSI = flags ; mode u -- apply `flags` to the CURRENT (top-of-
       stack) entry per `mode`: wire protocol 1=set (default), 2=OR,
       3=AND-NOT. ghostcon_kitty_set_mode_t is 0-indexed
       (GC_KITTY_SET=0/OR=1/NOT=2), hence the -1; any other wire value
       is invalid per the spec and ignored, same as this file's other
       handlers silently ignore an out-of-range mode. */
    int wire_mode = PARAM2(st, 1);
    if (wire_mode < 1 || wire_mode > 3)
        return;
    ghostcon_kitty_set_mode(&s->kitty, (ghostcon_kitty_set_mode_t)(wire_mode - 1),
                             (uint8_t)PARAM1(st, 0));
}

static void
handle_csi_kitty_query(ghostcon_stream_t *st, ghostcon_screen_t *s) {
    /* CSI ? u -- report the current (top-of-stack) flags as CSI ? {flags} u. */
    char buf[16];
    snprintf(buf, sizeof(buf), "\x1b[?%uu", (unsigned)ghostcon_kitty_current(&s->kitty));
    stream_output(st, buf);
}

static void
handle_csi_cursor_style(ghostcon_stream_t *st, ghostcon_screen_t *s) {
    if (!csi_param_count(st, 0, 1)) return;
    int style = PARAM1(st, 0);
    /* DECSCUSR: 0=blink block, 1=blink block, 2=steady block,
       3=blink underline, 4=steady underline, 5=blink bar, 6=steady bar */
    switch (style) {
    case 0: case 1: s->cursor.cursor_style = GC_CURSOR_BLOCK_BLINK; break;
    case 2: s->cursor.cursor_style = GC_CURSOR_BLOCK; break;
    case 3: s->cursor.cursor_style = GC_CURSOR_UNDERLINE_BLINK; break;
    case 4: s->cursor.cursor_style = GC_CURSOR_UNDERLINE; break;
    case 5: s->cursor.cursor_style = GC_CURSOR_BAR_BLINK; break;
    case 6: s->cursor.cursor_style = GC_CURSOR_BAR; break;
    }
}

static void
handle_csi_decsca(ghostcon_stream_t *st, ghostcon_screen_t *s) {
    /* DECSCA — CSI Ps " q — select character protection attribute.
       Ghostty (stream.zig): 0 or 1 param; Ps 0 or 2 → off, Ps 1 → DEC.
       Note Ghostty maps Ps=2 to `.off`, NOT ISO — the ISO screen mode
       is only reachable from internal callers, never from DECSCA. The
       screen's protected_mode is left unchanged on off (Ghostty comment:
       "NEVER reset to .off because logic such as eraseChars depends on
       knowing what the most recent mode was"). */
    if (!csi_param_count(st, 0, 1)) return;
    int mode = PARAM1(st, 0);
    switch (mode) {
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

static void
handle_csi_save_mode(ghostcon_stream_t *st, ghostcon_screen_t *s) {
    /* CSI ? Ps s — save the given DEC modes */
    for (uint8_t i = 0; i < st->params_idx; i++)
        ghostcon_screen_save_mode(s, st->params[i]);
}

static void
handle_csi_restore_mode(ghostcon_stream_t *st, ghostcon_screen_t *s) {
    /* CSI ? Ps r — restore the given DEC modes */
    for (uint8_t i = 0; i < st->params_idx; i++)
        ghostcon_screen_restore_mode(s, st->params[i]);
}

static void
handle_csi_shift_escape(ghostcon_stream_t *st, ghostcon_screen_t *s) {
    /* XTSHIFTESCAPE — CSI > Ps s — mouse-shift capture on/off */
    switch (st->params_idx) {
    case 0: s->mouse_shift_capture = false; break;
    case 1:
        switch (PARAM1(st, 0)) {
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

static void
handle_csi_request_mode(ghostcon_stream_t *st, ghostcon_screen_t *s) {
    /* DECRQM: CSI Ps $ p (ansi) / CSI ? Ps $ p (DEC) —
       respond with DECRPM: CSI[?] Ps;Pn $ y.
       Pn: 0 = not recognized, 1 = set, 2 = reset. */
    if (st->params_idx != 1) return;
    int mode = st->params[0];
    bool ansi = (st->intermediates_idx == 1 && st->intermediates[0] == '$');
    int state = 0; /* not recognized */
    char buf[32];

    if (ansi) {
        switch (mode) {
        case 4:  state = s->insert_mode ? 1 : 2; break;
        case 2:  state = 2; break;  /* KAM — not tracked */
        case 20: state = 2; break;  /* LNM — not tracked */
        default: state = 0; break;
        }
        snprintf(buf, sizeof(buf), "\x1b[%d;%d$y", mode, state);
    } else {
        switch (mode) {
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

static void
handle_csi_tab_clear(ghostcon_stream_t *st, ghostcon_screen_t *s) {
    int mode = PARAM1(st, 0);
    ghostcon_screen_tab_clear(s, mode);
}

static void
handle_csi_device_attrib(ghostcon_stream_t *st, ghostcon_screen_t *s) {
    (void)s;
    /* DA1/DA2/DA3 responses — mirrors Ghostty's device_attributes.zig defaults. */
    switch (st->intermediates_idx) {
    case 0:
        /* DA1: VT220 level-2 + ANSI color */
        stream_output(st, "\x1b[?62;22c");
        break;
    case 1:
        if (st->intermediates[0] == '>') {
            /* DA2: VT220, fw rev 0, ROM 0 */
            stream_output(st, "\x1b[>1;0;0c");
        } else if (st->intermediates[0] == '=') {
            /* DA3: DECRPTUI 0 */
            stream_output(st, "\x1bP!|00000000\x1b\\");
        }
        break;
    default: break;
    }
}

static void
handle_csi_device_status_report(ghostcon_stream_t *st, ghostcon_screen_t *s) {
    if (st->params_idx != 1) return;
    int mode = st->params[0];
    bool question = (st->intermediates_idx == 1 && st->intermediates[0] == '?');
    char buf[32];
    if (!question) {
        switch (mode) {
        case 5:
            stream_output(st, "\x1b[0n");
            break;
        case 6: {
            /* CPR — cursor position report (1-based). With origin mode
               coordinates are relative to the scroll region (Ghostty). */
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
    } else {
        switch (mode) {
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

/* Stamps when a synchronized-output batch (mode 2026) started, so
   core/main.c's render gate can bound how long it withholds a frame if
   the app never sends the closing ?2026l -- see screen.h's own doc
   comment on synchronized_output_since. */
static void
mark_sync_output_start(ghostcon_screen_t *s) {
    s->synchronized_output = true;
    clock_gettime(CLOCK_MONOTONIC, &s->synchronized_output_since);
}

static void
handle_csi_set_mode(ghostcon_stream_t *st, ghostcon_screen_t *s) {
    /* SM — Ghostty iterates ALL params (multi-mode sequences). */
    for (uint8_t i = 0; i < st->params_idx; i++) {
        switch (st->params[i]) {
        case 4:  s->insert_mode = true; break;                    /* IRM */
        case 2004: s->bracketed_paste = true; break;
        case 2026: mark_sync_output_start(s); break;
        }
    }
}

static void
handle_csi_reset_mode(ghostcon_stream_t *st, ghostcon_screen_t *s) {
    /* RM — iterate all params. */
    for (uint8_t i = 0; i < st->params_idx; i++) {
        switch (st->params[i]) {
        case 4:  s->insert_mode = false; break;
        case 2004: s->bracketed_paste = false; break;
        case 2026: s->synchronized_output = false; break;
        }
    }
}

static void
handle_csi_dec_set(ghostcon_stream_t *st, ghostcon_screen_t *s) {
    /* DECSET — iterate all params (Ghostty). DECOM also homes the cursor
       on both set and reset (Ghostty: `.origin => setCursorPos(1, 1)`). */
    for (uint8_t i = 0; i < st->params_idx; i++) {
        switch (st->params[i]) {
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
        }
    }
}

static void
handle_csi_dec_reset(ghostcon_stream_t *st, ghostcon_screen_t *s) {
    /* DECRST — iterate all params. */
    for (uint8_t i = 0; i < st->params_idx; i++) {
        switch (st->params[i]) {
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
               unconditionally, without checking left_right_margin --
               so disabling DECLRMM without also resetting the region
               left a stale DECSLRM margin permanently in effect.
               Found live: this made every subsequent line wrap at
               whatever column DECSLRM last set, indefinitely. */
            ghostcon_screen_set_margin_region(s, -1, -1);
            break;
        case 1000: case 1002: case 1003: s->mouse_tracking = false; s->mouse_protocol = 0; break;
        case 1006: s->mouse_sgr = false; break;
        case 2026: s->synchronized_output = false; break;          /* synchronized output */
        case 1047: ghostcon_screen_alt_screen_exit(s); break;      /* alt screen */
        case 1048: ghostcon_screen_cursor_restore(s); break;      /* restore cursor */
        case 1049: ghostcon_screen_alt_screen_exit(s); ghostcon_screen_cursor_restore(s); break; /* alt screen + restore cursor */
        }
    }
}

/* SGR handler (CSI m) */
static void
handle_csi_sgr(ghostcon_stream_t *st, ghostcon_screen_t *s) {
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
    while (ghostty_sgr_next(parser, &attr)) {
        switch (attr.tag) {
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
            /* bright_fg_8 is already the absolute 0-15 palette index
               (e.g. 9 for bright red / SGR 91, matching
               GHOSTTY_COLOR_NAMED_BRIGHT_RED) -- NOT a 0-7 offset that
               needs +8. Adding 8 here double-offset it (9+8=17), which
               isn't a named color at all -- it lands inside the 216
               color cube (indices 16-231) at a essentially arbitrary
               color. Confirmed against libghostty-vt directly: see
               PLAN.md for the live repro (bright red rendering as dark
               blue) and the standalone parser test that caught this. */
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

/* ------------------------------------------------------------------ */
/* CSI dispatch table                                                  */
/* ------------------------------------------------------------------ */

typedef void (*csi_handler_t)(ghostcon_stream_t *, ghostcon_screen_t *);

static csi_handler_t
csi_dispatch(ghostcon_stream_t *st, uint8_t final_byte) {
    /* Intermediate discrimination mirrors Ghostty's
       `switch (input.intermediates.len)` structure (stream.zig). The
       private markers < = > ? are collected into intermediates when they
       lead a sequence, so DEC private, DA2/DA3, XTSHIFTESCAPE, DECSCA,
       and DECSED/DECSEL are all told apart here. */
    const uint8_t inter_len = st->intermediates_idx;
    const uint8_t inter0 = (inter_len > 0) ? st->intermediates[0] : 0;
    const bool has_inter = (inter_len > 0);

    /* Ghostty drops any CSI sequence whose parameter count reached
       MAX_PARAMS (24) — `params_idx >= Parser.MAX_PARAMS` returns before
       dispatch in csiDispatchFinal. */
    if (st->params_idx >= GC_STREAM_MAX_PARAMS)
        return NULL;

    /* Finals that have intermediate variants (DEC private and friends) */
    switch (final_byte) {
    case 'J':
        if (inter_len == 0) return handle_csi_erase_display;
        if (inter_len == 1 && inter0 == '?') return handle_csi_erase_display;
        return NULL;
    case 'K':
        if (inter_len == 0) return handle_csi_erase_line;
        if (inter_len == 1 && inter0 == '?') return handle_csi_erase_line;
        return NULL;
    case 'h':
        if (inter_len == 0) return handle_csi_set_mode;
        if (inter_len == 1 && inter0 == '?') return handle_csi_dec_set;
        return NULL;
    case 'l':
        if (inter_len == 0) return handle_csi_reset_mode;
        if (inter_len == 1 && inter0 == '?') return handle_csi_dec_reset;
        return NULL;
    case 'r':
        if (inter_len == 0) return handle_csi_set_scroll_region;
        if (inter_len == 1 && inter0 == '?') return handle_csi_restore_mode;
        return NULL;
    case 's':
        if (inter_len == 0) return handle_csi_set_margin_region;
        if (inter_len == 1 && inter0 == '?') return handle_csi_save_mode;
        if (inter_len == 1 && inter0 == '>') return handle_csi_shift_escape;
        return NULL;
    case 'q':
        if (inter_len == 1 && inter0 == ' ') return handle_csi_cursor_style;
        if (inter_len == 1 && inter0 == '"') return handle_csi_decsca;
        return NULL;
    case 'c':
        if (inter_len == 0) return handle_csi_device_attrib;
        if (inter_len == 1 && inter0 == '>') return handle_csi_device_attrib;
        if (inter_len == 1 && inter0 == '=') return handle_csi_device_attrib;
        return NULL;
    case 'p':
        if (inter_len == 1 && inter0 == '$') return handle_csi_request_mode;
        if (inter_len == 2 && inter0 == '?' && st->intermediates[1] == '$')
            return handle_csi_request_mode;
        return NULL;
    case 'u':
        if (inter_len == 0) return handle_csi_restore_cursor;
        if (inter_len == 1 && inter0 == '>') return handle_csi_kitty_push;
        if (inter_len == 1 && inter0 == '<') return handle_csi_kitty_pop;
        if (inter_len == 1 && inter0 == '=') return handle_csi_kitty_set;
        if (inter_len == 1 && inter0 == '?') return handle_csi_kitty_query;
        return NULL;
    default:
        break;
    }

    /* Everything else must be a plain (intermediate-less) sequence */
    if (has_inter) return NULL;

    switch (final_byte) {
    case 'A': return handle_csi_cursor_up;
    case 'B': return handle_csi_cursor_down;
    case 'C': return handle_csi_cursor_forward;
    case 'D': return handle_csi_cursor_back;
    case 'E': return handle_csi_cursor_next_line;
    case 'F': return handle_csi_cursor_prev_line;
    case 'G': return handle_csi_cursor_horizontal_abs;
    case 'H': return handle_csi_cursor_position;
    case 'f': return handle_csi_horizontal_vertical_position;
    case 'L': return handle_csi_insert_lines;
    case 'M': return handle_csi_delete_lines;
    case 'P': return handle_csi_delete_chars;
    case 'X': return handle_csi_erase_chars;
    case '@': return handle_csi_insert_chars;
    case 'S': return handle_csi_scroll_up;
    case 'T': return handle_csi_scroll_down;
    case 'm': return handle_csi_sgr;
    case 'g': return handle_csi_tab_clear;
    case 'n': return handle_csi_device_status_report;
    case 'I': return handle_csi_tab_forward;       /* CHT */
    case 'Z': return handle_csi_tab_back;           /* CBT */
    case 'a': return handle_csi_cursor_forward;     /* HPR */
    case 'b': return handle_csi_repeat;             /* REP */
    case 'd': return handle_csi_cursor_vertical_abs; /* VPA */
    case 'e': return handle_csi_cursor_down;        /* VPR */
    case 't': break; /* window ops */
    case '`': return handle_csi_cursor_horizontal_abs; /* HPA */
    default: break;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* OSC dispatch — manual (number-level) tier                          */
/*                                                                     */
/* The installed libghostty-vt's OSC C API only exposes coarse command */
/* classification plus window-title text extraction (verified against */
/* both the distro package and a from-source master build with        */
/* -Demit-lib-vt=true — see color.h's own note). Everything else is    */
/* parsed by us directly from st->buf, the raw NUL-terminated          */
/* "N;p1;p2;..." OSC payload we already own from accumulating OSC      */
/* bytes ourselves in the state machine below. This sidesteps the API's*/
/* classification ambiguity too: e.g. OSC 4/10/11/12/104 (all color    */
/* operations) collapse into one GHOSTTY_OSC_COMMAND_COLOR_OPERATION   */
/* type with no way to tell them apart via the API — we just read the  */
/* leading number ourselves instead.                                   */
/* ------------------------------------------------------------------ */

/* Splits `buf` (length `len`, NUL-terminated) on ';' in place, writing
   up to `max_fields` pointers into `fields`. fields[0] is always the
   leading OSC number. Returns the field count. */
static int
osc_split_fields(char *buf, uint16_t len, char **fields, int max_fields)
{
    int n = 0;
    fields[n++] = buf;
    for (uint16_t i = 0; i < len && n < max_fields; i++) {
        if (buf[i] == ';') {
            buf[i] = '\0';
            fields[n++] = buf + i + 1;
        }
    }
    return n;
}

/* OSC 52's payload is base64 -- rejects anything that isn't, rather than
   storing (and later echoing back through a query) arbitrary garbage a
   misbehaving program sent. Doesn't validate padding placement/length is
   a multiple of 4 -- xterm itself is lenient about this, and being
   stricter here would just make a legitimate-but-slightly-malformed
   payload from some other terminal's app silently vanish instead of
   being stored as-is, which is more surprising than useful. */
static bool
osc52_is_valid_base64(const char *s)
{
    if (s[0] == '\0')
        return false;
    for (const char *p = s; *p; p++) {
        char c = *p;
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') || c == '+' || c == '/' || c == '=';
        if (!ok)
            return false;
    }
    return true;
}

static int
uri_hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Decodes an OSC 7 "report PWD" argument: either a bare path, or (the
   common case, emitted by bash/zsh/fish's OSC 7 hooks) a
   file://[host]/path URI. The host component is discarded -- this
   terminal has no notion of "which host a path is valid on" beyond
   "whichever one is currently attached", and xterm's own
   recommendation is to accept it without validating against the
   local hostname. Percent-encoded bytes are decoded; a lone/invalid
   '%' escape is passed through literally rather than rejecting the
   whole sequence, matching OSC 7's "best effort, advisory" spirit. */
static void
osc7_decode_pwd(const char *arg, char *out, size_t out_len)
{
    const char *path = arg;
    if (strncmp(arg, "file://", 7) == 0) {
        const char *rest = arg + 7;
        const char *slash = strchr(rest, '/');
        path = slash ? slash : rest;
    }
    size_t oi = 0;
    for (size_t i = 0; path[i] != '\0' && oi + 1 < out_len; i++) {
        if (path[i] == '%') {
            int hi = uri_hex_nibble(path[i + 1]);
            int lo = hi >= 0 ? uri_hex_nibble(path[i + 2]) : -1;
            if (hi >= 0 && lo >= 0) {
                out[oi++] = (char)((hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        out[oi++] = path[i];
    }
    out[oi] = '\0';
}

static void
osc_write_response(ghostcon_stream_t *st, const char *body)
{
    if (!st->output_fn)
        return;
    char seq[128];
    int n = snprintf(seq, sizeof(seq), "\x1b]%s\x1b\\", body);
    if (n > 0)
        st->output_fn(st->output_userdata, (const uint8_t *)seq, (size_t)n);
}

/* Handles OSC numbers the manual tier owns. Returns true if `num` was
   recognized (whether or not the specific field contents were valid —
   a malformed field is silently ignored per the terminal's usual
   "never crash on bad input" rule, not treated as "unrecognized OSC").
   Returns false for anything this tier doesn't own, so the caller can
   fall through to the ghostty_osc-based path (window title, and the
   discard-and-log fallback for truly unrecognized sequences). */
static bool
osc_dispatch_manual(ghostcon_stream_t *st, ghostcon_screen_t *s)
{
    /* Work on a local copy, not st->buf directly: osc_split_fields()
       mutates in place (';' -> '\0'), and if this OSC number turns out
       not to be one we own, the caller falls through to the existing
       ghostty_osc_next() loop over st->buf -- which needs the original
       semicolons intact (e.g. window title "0;My Title" would parse as
       corrupted "0\0My Title" otherwise). Payloads too long for this
       are definitely not one of our small numeric OSC types anyway. */
    char copy[1024];
    if (st->osc_len >= sizeof(copy))
        return false;
    memcpy(copy, st->buf, st->osc_len);
    copy[st->osc_len] = '\0';

    char *fields[16];
    int nfields = osc_split_fields(copy, st->osc_len, fields, 16);
    if (nfields < 1)
        return false;
    long num = strtol(fields[0], NULL, 10);

    switch (num) {
    case 4: /* set/query palette entry(ies): 4;idx;spec[;idx;spec...] */
        for (int i = 1; i + 1 < nfields; i += 2) {
            long idx = strtol(fields[i], NULL, 10);
            if (idx < 0 || idx > 255)
                continue;
            const char *spec = fields[i + 1];
            if (strcmp(spec, "?") == 0) {
                GhosttyColorRgb c = ghostcon_palette_resolve(&s->palette, (uint8_t)idx);
                char colorspec[24], body[48];
                ghostcon_color_format_spec(c, colorspec, sizeof(colorspec));
                snprintf(body, sizeof(body), "4;%ld;%s", idx, colorspec);
                osc_write_response(st, body);
            } else {
                GhosttyColorRgb c;
                if (ghostcon_color_parse_spec(spec, &c))
                    ghostcon_palette_set(&s->palette, (int)idx, c);
            }
        }
        return true;

    case 10: /* set/query default foreground */
    case 11: /* set/query default background */
    case 12: /* set/query cursor color */
        if (nfields >= 2) {
            const char *spec = fields[1];
            if (strcmp(spec, "?") == 0) {
                GhosttyColorRgb c = num == 10 ? s->palette.fg_default
                                   : num == 11 ? s->palette.bg_default
                                   : s->palette.cursor_color;
                char colorspec[24], body[48];
                ghostcon_color_format_spec(c, colorspec, sizeof(colorspec));
                snprintf(body, sizeof(body), "%ld;%s", num, colorspec);
                osc_write_response(st, body);
            } else {
                GhosttyColorRgb c;
                if (ghostcon_color_parse_spec(spec, &c)) {
                    if (num == 10)
                        ghostcon_palette_set_default_fg(&s->palette, c);
                    else if (num == 11)
                        ghostcon_palette_set_default_bg(&s->palette, c);
                    else
                        ghostcon_palette_set_cursor(&s->palette, c);
                }
            }
        }
        return true;

    case 104: /* reset palette entry(ies): 104[;idx[;idx...]] -- no args = reset all */
        if (nfields == 1) {
            for (int i = 0; i < 256; i++)
                ghostcon_palette_reset(&s->palette, (uint8_t)i);
        } else {
            for (int i = 1; i < nfields; i++) {
                long idx = strtol(fields[i], NULL, 10);
                if (idx >= 0 && idx <= 255)
                    ghostcon_palette_reset(&s->palette, (uint8_t)idx);
            }
        }
        return true;

    case 7: /* report/set PWD: 7;file://[host]/path (or a bare path) */
        if (nfields >= 2)
            osc7_decode_pwd(fields[1], s->cwd, sizeof(s->cwd));
        return true;

    case 133: /* FinalTerm semantic prompt: 133;<letter>[;args...] */
    case 633: /* VSCode shell integration: superset of 133's letters,
                 plus its own property-report sub-commands */
        if (nfields >= 2) {
            switch (fields[1][0]) {
            case 'A': /* prompt start */
                s->semantic_current = GHOSTCON_CELL_SEMANTIC_PROMPT;
                break;
            case 'B': /* prompt end / command input start */
                s->semantic_current = GHOSTCON_CELL_SEMANTIC_INPUT;
                break;
            case 'C': /* command output start */
                s->semantic_current = GHOSTCON_CELL_SEMANTIC_OUTPUT;
                break;
            case 'D': /* command finished: 133;D[;exit_code] */
                s->semantic_current = GHOSTCON_CELL_SEMANTIC_PROMPT;
                if (nfields >= 3)
                    s->semantic_last_exit_code = (int)strtol(fields[2], NULL, 10);
                break;
            case 'P': /* 633;P;Cwd=<path> -- VSCode property report.
                         Only Cwd is handled; reuses OSC 7's decoder
                         since VSCode sends the same file://-or-bare-path
                         form. Other properties (IsWindows, etc.) are
                         intentionally ignored -- not applicable here. */
                if (num == 633 && nfields >= 3 && strncmp(fields[2], "Cwd=", 4) == 0)
                    osc7_decode_pwd(fields[2] + 4, s->cwd, sizeof(s->cwd));
                break;
            default:
                /* 133;A/B/C/D cover FinalTerm; 633 additionally sends
                   E (command line text) and others we don't yet store
                   anywhere -- silently ignored, matches OSC 8's stub
                   tier philosophy of "don't crash on the unsupported
                   remainder of a spec". */
                break;
            }
        }
        return true;

    case 8: /* hyperlink: 8;params;uri (uri empty = end current link) */
        if (nfields >= 3) {
            const char *uri = fields[2];
            if (uri[0] == '\0') {
                s->cursor.hyperlink_id = GC_HYPERLINK_ID_NONE;
            } else {
                s->cursor.hyperlink_id = ghostcon_hyperlink_set_add(s->hyperlinks, uri);
            }
        }
        return true;

    case 9: case 777: /* desktop notification -- stub tier, no ghostcon-ipc
                          broker exists yet (PLAN.md's own deferred item),
                          so the message is handed up via a callback
                          rather than discarded -- ghostcon-core logs it
                          through core/diag.c, which term/ has no
                          dependency on (see stream.h's notify_fn doc). */
        if (nfields >= 2 && st->notify_fn)
            st->notify_fn(st->notify_userdata, fields[nfields - 1]);
        return true;

    case 52: /* clipboard -- stub tier, single-instance only (no
                cross-VT sharing without ghostcon-ipc). 52;c;<base64> sets,
                52;c;? queries. Stored/returned as the raw base64 payload
                verbatim -- OSC 52's own wire format already is base64, so
                there's nothing to decode. A non-base64 payload is rejected
                outright rather than stored as garbage. */
        if (nfields >= 3) {
            const char *payload = fields[2];
            if (strcmp(payload, "?") == 0) {
                char resp[sizeof(s->clipboard) + 8];
                snprintf(resp, sizeof(resp), "52;c;%s", s->clipboard);
                osc_write_response(st, resp);
            } else if (osc52_is_valid_base64(payload)) {
                snprintf(s->clipboard, sizeof(s->clipboard), "%s", payload);
            }
        }
        return true;

    default:
        return false;
    }
}

/* ------------------------------------------------------------------ */
/* OSC dispatch                                                       */
/* ------------------------------------------------------------------ */

static void
osc_dispatch(ghostcon_stream_t *st, ghostcon_screen_t *s) {
    if (osc_dispatch_manual(st, s))
        return;

    GhosttyOscParser parser;
    if (ghostty_osc_new(NULL, &parser) != GHOSTTY_SUCCESS)
        return;

    for (uint16_t i = 0; i < st->osc_len; i++)
        ghostty_osc_next(parser, (uint8_t)st->buf[i]);

    /* Terminator byte: BEL (0x07) or ST (0x5C for ESC \) */
    uint8_t terminator = st->osc_terminated_by_bel ? 0x07 : 0x5C;
    GhosttyOscCommand cmd = ghostty_osc_end(parser, terminator);
    GhosttyOscCommandType type = ghostty_osc_command_type(cmd);

    switch (type) {
    case GHOSTTY_OSC_COMMAND_CHANGE_WINDOW_TITLE:
    case GHOSTTY_OSC_COMMAND_CHANGE_WINDOW_ICON: {
        const char *str = NULL;
        ghostty_osc_command_data(cmd, GHOSTTY_OSC_DATA_CHANGE_WINDOW_TITLE_STR, &str);
        if (str && st->title_fn)
            st->title_fn(st->title_userdata, str);
        (void)s;
        break;
    }
    case GHOSTTY_OSC_COMMAND_SEMANTIC_PROMPT:
        /* OSC 133 — handled above in osc_dispatch_manual() (OSC 633,
           the VSCode superset, isn't classified by the library at all
           since it isn't in GhosttyOscCommandType — it's caught purely
           by number in the manual tier). */
        break;
    case GHOSTTY_OSC_COMMAND_CLIPBOARD_CONTENTS: {
        /* OSC 52 — stub, needs ghostcon-ipc */
        break;
    }
    case GHOSTTY_OSC_COMMAND_REPORT_PWD: {
        /* OSC 7 — handled above in osc_dispatch_manual(); the library
           classifies the command but exposes none of its URI data,
           same limitation as the color-operation tier. */
        break;
    }
    case GHOSTTY_OSC_COMMAND_MOUSE_SHAPE: {
        /* OSC 22 — store cursor shape request */
        break;
    }
    case GHOSTTY_OSC_COMMAND_COLOR_OPERATION: {
        /* OSC 4/10/11/12/104 — palette operations */
        /* Stub: handled in Phase 1 with proper palette integration */
        break;
    }
    case GHOSTTY_OSC_COMMAND_KITTY_COLOR_PROTOCOL:
        break;
    case GHOSTTY_OSC_COMMAND_SHOW_DESKTOP_NOTIFICATION: {
        /* OSC 9 — stub, needs ghostcon-ipc */
        break;
    }
    case GHOSTTY_OSC_COMMAND_HYPERLINK_START:
    case GHOSTTY_OSC_COMMAND_HYPERLINK_END:
        /* OSC 8 — hyperlinks. Store on cursor/cell */
        break;
    case GHOSTTY_OSC_COMMAND_CONEMU_SLEEP:
    case GHOSTTY_OSC_COMMAND_CONEMU_SHOW_MESSAGE_BOX:
    case GHOSTTY_OSC_COMMAND_CONEMU_CHANGE_TAB_TITLE:
    case GHOSTTY_OSC_COMMAND_CONEMU_PROGRESS_REPORT:
    case GHOSTTY_OSC_COMMAND_CONEMU_WAIT_INPUT:
    case GHOSTTY_OSC_COMMAND_CONEMU_GUIMACRO:
    case GHOSTTY_OSC_COMMAND_CONEMU_RUN_PROCESS:
    case GHOSTTY_OSC_COMMAND_CONEMU_OUTPUT_ENVIRONMENT_VARIABLE:
    case GHOSTTY_OSC_COMMAND_CONEMU_XTERM_EMULATION:
    case GHOSTTY_OSC_COMMAND_CONEMU_COMMENT:
        /* ConEmu — discard, application-specific */
        break;
    case GHOSTTY_OSC_COMMAND_KITTY_TEXT_SIZING:
        break;
    case GHOSTTY_OSC_COMMAND_INVALID:
    default:
        /* Per PLAN.md: log to journald (future), discard, continue */
        break;
    }

    ghostty_osc_free(parser);
}

/* ------------------------------------------------------------------ */
/* Main state machine — per-byte processing                           */
/* ------------------------------------------------------------------ */

void
ghostcon_stream_process_byte(ghostcon_stream_t *st,
                             uint8_t c,
                             ghostcon_screen_t *s)
{
    /* 8-bit C1 controls (0x80-0x9F). In ground state these bytes are
       UTF-8 continuation/lead bytes and are handled by the UTF-8 decoder
       (matching Ghostty: a stray C1 in ground decodes to U+FFFD). Only
       escape-string and CSI/DCS states terminate on C1 -- NOT the opaque
       string-payload-accumulation states (OSC/DCS-passthrough/SOS-PM-APC).
       Real bug found live: 0x9C is simultaneously the C1 code for ST
       (String Terminator) and a valid UTF-8 continuation byte -- a UTF-8
       character whose encoding happens to contain 0x9C as its second or
       third byte (e.g. U+2733 "✳", encoded E2 9C B3) arriving mid-OSC-
       payload (a window title containing that character, sent by a real
       app) got its 0x9C byte mistaken for ST here, silently truncating
       the OSC before osc_dispatch() ever ran and discarding the title --
       then the OSC's remaining bytes got reprocessed from GROUND state as
       if newly typed, printing "leaked" title text directly onto the
       screen (a stray orphaned continuation byte first decoding to
       U+FFFD, then the rest as literal ASCII). These states are already
       correctly BEL/ESC-backslash-terminated on their own (see
       GC_STREAM_OSC_STRING's own case below) and must treat C1-range
       bytes as ordinary opaque payload, not a termination shortcut. */
    if (c >= 0x80 && c <= 0x9F && st->state != GC_STREAM_GROUND &&
        st->state != GC_STREAM_OSC_STRING &&
        st->state != GC_STREAM_DCS_PASSTHROUGH &&
        st->state != GC_STREAM_DCS_PASSTHROUGH_ESC &&
        st->state != GC_STREAM_SOS_PM_APC_STRING) {
        switch (c) {
        case 0x84: /* IND */ ghostcon_screen_linefeed(s); return;
        case 0x85: /* NEL */ ghostcon_screen_carriage_return(s); ghostcon_screen_linefeed(s); return;
        case 0x88: /* HTS */ ghostcon_screen_tab_set(s); return;
        case 0x8D: /* RI */ ghostcon_screen_reverse_index(s); return;
        case 0x8E: /* SS2 */ return;
        case 0x8F: /* SS3 */ return;
        case 0x9B: /* CSI */ st->state = GC_STREAM_CSI_ENTRY; return;
        case 0x9C: /* ST */ st->state = GC_STREAM_GROUND; return;
        case 0x9D: /* OSC */ st->state = GC_STREAM_OSC_STRING; return;
        case 0x90: /* DCS */ st->state = GC_STREAM_DCS_ENTRY; return;
        case 0x98: /* SOS */ st->state = GC_STREAM_SOS_PM_APC_STRING; return;
        case 0x9E: /* PM */ st->state = GC_STREAM_SOS_PM_APC_STRING; return;
        case 0x9F: /* APC */ st->state = GC_STREAM_SOS_PM_APC_STRING; return;
        }
    }

    switch (st->state) {

    /* ============================================================== */
    case GC_STREAM_GROUND:
    /* ============================================================== */
        /* Check for pending ST (ESC \) terminator for OSC */
        if (st->osc_pending) {
            if (c == '\\') {
                st->osc_terminated_by_bel = false;
                osc_dispatch(st, s);
                st->osc_len = 0;
            }
            st->osc_pending = false;
            if (c == '\\') break;
        }

        /* Every byte in ground is UTF-8. The decoder emits codepoints;
           control characters and ESC are routed out via
           stream_handle_codepoint (mirrors Ghostty's nextUtf8). */
        {
            uint32_t cp;
            bool consumed = utf8_next(st, c, &cp);
            if (cp != 0xFFFFFFFF)
                stream_handle_codepoint(st, s, cp);
            if (!consumed) {
                utf8_next(st, c, &cp);
                if (cp != 0xFFFFFFFF)
                    stream_handle_codepoint(st, s, cp);
            }
        }
        break;

    /* ============================================================== */
    case GC_STREAM_ESC:
    /* ============================================================== */
        st->state = GC_STREAM_GROUND;

        if (c == '[') {
            st->state = GC_STREAM_CSI_ENTRY;
        } else if (c == ']') {
            st->state = GC_STREAM_OSC_STRING;
            st->osc_len = 0;
        } else if (c == 'P') {
            st->state = GC_STREAM_DCS_ENTRY;
            st->dcs_len = 0;
        } else if (c == 'X' || c == '^' || c == '_') {
            st->state = GC_STREAM_SOS_PM_APC_STRING;
        } else if (c >= 0x20 && c <= 0x2F) {
            /* Intermediate byte */
            st->intermediates[0] = c;
            st->intermediates_idx = 1;
            st->state = GC_STREAM_ESC_INTERMEDIATE;
        } else if (c >= 0x30 && c <= 0x7E) {
            /* Final byte — dispatch */
            switch (c) {
            case '7': ghostcon_screen_cursor_save(s); break;
            case '8': ghostcon_screen_cursor_restore(s); break;
            case 'D': ghostcon_screen_linefeed(s); break;
            case 'E': ghostcon_screen_carriage_return(s); ghostcon_screen_linefeed(s); break;
            case 'M': ghostcon_screen_reverse_index(s); break;
            case 'H': ghostcon_screen_tab_set(s); break; /* HTS -- the 7-bit
                two-character form every real program actually sends; only
                the rare 1-byte C1 form (0x88) was wired up before. Found
                via direct comparison against real libghostty-vt: it sets
                the stop where ghostcon silently did nothing. */
            case 'c': ghostcon_screen_reset(s); break; /* RIS — full reset */
            /* DEC private */
            case '=': break; /* DECKPAM — application keypad */
            case '>': break; /* DECPNM — numeric keypad */
            case '(': case ')': case '*': case '+':
                /* Designate character set — defer */
                st->intermediates[0] = c;
                st->intermediates_idx = 1;
                st->state = GC_STREAM_ESC_INTERMEDIATE;
                break;
            default:
                break;
            }
        } else if (c == '[') {
            st->state = GC_STREAM_CSI_ENTRY;
        } else if (c == ']') {
            st->state = GC_STREAM_OSC_STRING;
            st->osc_len = 0;
        } else if (c == 'P') {
            st->state = GC_STREAM_DCS_ENTRY;
            st->dcs_len = 0;
        } else if (c == 'X' || c == '^' || c == '_') {
            /* SOS, PM, APC */
            st->state = GC_STREAM_SOS_PM_APC_STRING;
        }
        break;

    /* ============================================================== */
    case GC_STREAM_ESC_INTERMEDIATE:
    /* ============================================================== */
        if (c >= 0x20 && c <= 0x2F) {
            if (st->intermediates_idx < GC_STREAM_MAX_INTERMEDIATES)
                st->intermediates[st->intermediates_idx++] = c;
        } else if (c >= 0x30 && c <= 0x7E) {
            st->state = GC_STREAM_GROUND;
            /* Dispatch with intermediates */
        } else {
            st->state = GC_STREAM_GROUND;
        }
        break;

    /* ============================================================== */
    case GC_STREAM_CSI_ENTRY:
    /* ============================================================== */
        st->intermediates_idx = 0;
        st->params_idx = 0;
        st->param_acc = 0;
        st->param_acc_idx = 0;

        if (c >= 0x30 && c <= 0x3F) {
            /* Parameter byte */
            st->state = GC_STREAM_CSI_PARAM;
            if (c >= 0x3C && c <= 0x3F) {
                /* Private marker (< = > ?): Ghostty's parser collects these
                   into the intermediate array when they lead the sequence
                   (csi_entry .collect -> csi_param). They are NOT params. */
                if (st->intermediates_idx < GC_STREAM_MAX_INTERMEDIATES)
                    st->intermediates[st->intermediates_idx++] = c;
            } else if (c >= '0' && c <= '9') {
                st->param_acc = c - '0';
                st->param_acc_idx = 1;
            } else if (c == ';') {
                st->params[0] = 0;
                st->params_idx = 1;
            }
        } else if (c >= 0x20 && c <= 0x2F) {
            st->state = GC_STREAM_CSI_INTERMEDIATE;
            if (st->intermediates_idx < GC_STREAM_MAX_INTERMEDIATES)
                st->intermediates[st->intermediates_idx++] = c;
        } else if (c >= 0x40 && c <= 0x7E) {
            /* Final byte */
            st->state = GC_STREAM_GROUND;
            csi_handler_t handler = csi_dispatch(st, c);
            if (handler) handler(st, s);
        } else {
            st->state = GC_STREAM_GROUND;
        }
        break;

    /* ============================================================== */
    case GC_STREAM_CSI_PARAM:
    /* ============================================================== */
        if (c >= '0' && c <= '9') {
            /* Wrapping accumulation matching Ghostty's `*|`/`+|` (u16 wrap).
               Use u32 intermediate so a huge digit run can't overflow int. */
            st->param_acc = (uint16_t)((uint32_t)st->param_acc * 10 + (c - '0'));
            st->param_acc_idx++;
        } else if (c == ';') {
            if (st->params_idx < GC_STREAM_MAX_PARAMS) {
                st->params[st->params_idx++] = st->param_acc;
            }
            st->param_acc = 0;
            st->param_acc_idx = 0;
        } else if (c >= 0x20 && c <= 0x2F) {
            /* Flush current param */
            if (st->params_idx < GC_STREAM_MAX_PARAMS)
                st->params[st->params_idx++] = st->param_acc;
            st->state = GC_STREAM_CSI_INTERMEDIATE;
            if (st->intermediates_idx < GC_STREAM_MAX_INTERMEDIATES)
                st->intermediates[st->intermediates_idx++] = c;
        } else if (c >= 0x40 && c <= 0x7E) {
            /* Flush current param */
            if (st->params_idx < GC_STREAM_MAX_PARAMS)
                st->params[st->params_idx++] = st->param_acc;
            /* Final byte */
            st->state = GC_STREAM_GROUND;
            csi_handler_t handler = csi_dispatch(st, c);
            if (handler) handler(st, s);
        } else if (c >= 0x3C && c <= 0x3F) {
            /* Private marker mid-params: Ghostty ignores the whole sequence
               (csi_param 0x3C-0x3F -> csi_ignore). Private markers are only
               valid as the first byte, handled in CSI_ENTRY. */
            st->state = GC_STREAM_CSI_IGNORE;
        } else {
            st->state = GC_STREAM_CSI_IGNORE;
        }
        break;

    /* ============================================================== */
    case GC_STREAM_CSI_INTERMEDIATE:
    /* ============================================================== */
        if (c >= 0x20 && c <= 0x2F) {
            if (st->intermediates_idx < GC_STREAM_MAX_INTERMEDIATES)
                st->intermediates[st->intermediates_idx++] = c;
        } else if (c >= 0x40 && c <= 0x7E) {
            st->state = GC_STREAM_GROUND;
            csi_handler_t handler = csi_dispatch(st, c);
            if (handler) handler(st, s);
        } else {
            st->state = GC_STREAM_CSI_IGNORE;
        }
        break;

    /* ============================================================== */
    case GC_STREAM_CSI_IGNORE:
    /* ============================================================== */
        if (c >= 0x40 && c <= 0x7E)
            st->state = GC_STREAM_GROUND;
        break;

    /* ============================================================== */
    case GC_STREAM_OSC_STRING:
    /* ============================================================== */
        if (c == 0x07) {
            /* BEL terminated — dispatch immediately */
            st->osc_terminated_by_bel = true;
            osc_dispatch(st, s);
            st->osc_len = 0;
            st->state = GC_STREAM_GROUND;
        } else if (c == 0x1B) {
            /* Potentially ST (ESC \) — flag and wait for next byte */
            st->osc_pending = true;
            st->state = GC_STREAM_GROUND;
        } else if (c < 0x20 && c != 0x09 && c != 0x0A) {
            /* C0 controls (except TAB/LF) terminate OSC */
            st->state = GC_STREAM_GROUND;
        } else {
            if (st->osc_len < st->buf_cap - 1) {
                st->buf[st->osc_len++] = (char)c;
                st->buf[st->osc_len] = '\0';
            }
        }
        break;

    /* ============================================================== */
    case GC_STREAM_DCS_ENTRY:
    /* ============================================================== */
        st->intermediates_idx = 0;
        st->params_idx = 0;
        st->param_acc = 0;

        if (c >= 0x30 && c <= 0x3F) {
            st->state = GC_STREAM_DCS_PARAM;
        } else if (c >= 0x20 && c <= 0x2F) {
            st->state = GC_STREAM_DCS_INTERMEDIATE;
            if (st->intermediates_idx < GC_STREAM_MAX_INTERMEDIATES)
                st->intermediates[st->intermediates_idx++] = c;
        } else if (c >= 0x40 && c <= 0x7E) {
            st->state = GC_STREAM_DCS_PASSTHROUGH;
            st->dcs_len = 0;
        } else {
            st->state = GC_STREAM_GROUND;
        }
        break;

    /* ============================================================== */
    case GC_STREAM_DCS_PARAM:
    /* ============================================================== */
        if (c >= '0' && c <= '9') {
            st->param_acc = (uint16_t)((uint32_t)st->param_acc * 10 + (c - '0'));
        } else if (c == ';') {
            if (st->params_idx < GC_STREAM_MAX_PARAMS)
                st->params[st->params_idx++] = st->param_acc;
            st->param_acc = 0;
        } else if (c >= 0x20 && c <= 0x2F) {
            st->state = GC_STREAM_DCS_INTERMEDIATE;
            if (st->intermediates_idx < GC_STREAM_MAX_INTERMEDIATES)
                st->intermediates[st->intermediates_idx++] = c;
        } else if (c >= 0x40 && c <= 0x7E) {
            st->state = GC_STREAM_DCS_PASSTHROUGH;
            st->dcs_len = 0;
        } else {
            st->state = GC_STREAM_GROUND;
        }
        break;

    /* ============================================================== */
    case GC_STREAM_DCS_INTERMEDIATE:
    /* ============================================================== */
        if (c >= 0x20 && c <= 0x2F) {
            if (st->intermediates_idx < GC_STREAM_MAX_INTERMEDIATES)
                st->intermediates[st->intermediates_idx++] = c;
        } else if (c >= 0x40 && c <= 0x7E) {
            st->state = GC_STREAM_DCS_PASSTHROUGH;
            st->dcs_len = 0;
        } else {
            st->state = GC_STREAM_GROUND;
        }
        break;

    /* ============================================================== */
    case GC_STREAM_DCS_PASSTHROUGH:
    /* ============================================================== */
        /* Only the 7-bit ESC-backslash form of ST terminates -- NOT a
           raw 0x9C byte, which is also a valid UTF-8 continuation byte
           (see this function's own doc comment on the top-level C1
           guard for the real bug this caused with an emoji mid-OSC-
           payload; DCS payloads can equally contain UTF-8 text, so the
           same fix applies here). */
        if (c == 0x1B) {
            st->state = GC_STREAM_DCS_PASSTHROUGH_ESC;
        } else {
            if (st->dcs_len < st->buf_cap - 1) {
                st->buf[st->dcs_len++] = (char)c;
                st->buf[st->dcs_len] = '\0';
            }
        }
        break;

    /* ============================================================== */
    case GC_STREAM_DCS_PASSTHROUGH_ESC:
    /* ============================================================== */
        if (c == '\\') {
            /* ST (ESC \) terminates */
            st->state = GC_STREAM_GROUND;
        } else if (c == 0x1B) {
            /* Double ESC — stay in this state */
        } else {
            /* Not ST — return to passthrough */
            st->state = GC_STREAM_DCS_PASSTHROUGH;
        }
        break;

    /* ============================================================== */
    case GC_STREAM_DCS_IGNORE:
    /* ============================================================== */
        if ((c >= 0x40 && c <= 0x7E) || c == 0x9C || c == 0x1B)
            st->state = GC_STREAM_GROUND;
        break;

    /* ============================================================== */
    case GC_STREAM_SOS_PM_APC_STRING:
    /* ============================================================== */
        /* Only the 7-bit ESC-backslash form of ST terminates -- same
           reasoning as DCS_PASSTHROUGH above: a raw 0x9C is also a
           valid UTF-8 continuation byte, and these payloads can
           contain UTF-8 text too. This doesn't wait for the full
           2-byte form (unlike DCS_PASSTHROUGH_ESC's dedicated
           sub-state) since this content is discarded either way --
           ESC alone is enough to end the (ignored) payload. */
        if (c == 0x1B) {
            st->state = GC_STREAM_GROUND;
        }
        break;
    }
}

void
ghostcon_stream_process(ghostcon_stream_t *st,
                        const uint8_t *data, size_t len,
                        ghostcon_screen_t *s)
{
    for (size_t i = 0; i < len; i++)
        ghostcon_stream_process_byte(st, data[i], s);
}
