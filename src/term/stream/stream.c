#define _DEFAULT_SOURCE /* CLOCK_MONOTONIC under -std=c11 without this */
#include "stream_internal.h"
#include "ghostcon/term/cell.h"
#include <stdlib.h>
#include <string.h>

/* Lifecycle */

bool ghostcon_stream_init(ghostcon_stream_t *st)
{
    memset(st, 0, sizeof(*st));
    st->state = GC_STREAM_GROUND;
    st->buf = st->local_buf;
    st->buf_cap = sizeof(st->local_buf);
    return true;
}

void ghostcon_stream_deinit(ghostcon_stream_t *st)
{
    if (st->buf && st->buf != st->local_buf)
        free(st->buf);
    memset(st, 0, sizeof(*st));
}

void ghostcon_stream_set_output(ghostcon_stream_t *st, ghostcon_output_fn fn, void *userdata)
{
    st->output_fn = fn;
    st->output_userdata = userdata;
}

void ghostcon_stream_set_title(ghostcon_stream_t *st, ghostcon_title_fn fn, void *userdata)
{
    st->title_fn = fn;
    st->title_userdata = userdata;
}

void ghostcon_stream_set_notify(ghostcon_stream_t *st, ghostcon_notify_fn fn, void *userdata)
{
    st->notify_fn = fn;
    st->notify_userdata = userdata;
}

void ghostcon_stream_set_cell_size(ghostcon_stream_t *st, int32_t cell_w, int32_t cell_h)
{
    st->cell_w = cell_w;
    st->cell_h = cell_h;
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

/* Emit *cp (0xFFFFFFFF = no codepoint emitted this byte). Returns true if
 * the byte was consumed (false only on mid-sequence reject, in which case
 * the caller must retry the same byte).
 */
static inline bool utf8_next(ghostcon_stream_t *st, uint8_t byte, uint32_t *cp)
{
    const uint8_t cls = utf8_char_classes[byte];
    const uint8_t initial_state = st->utf8_state;

    if (st->utf8_state != UTF8_ACCEPT)
    {
        st->utf8_acc = (st->utf8_acc << 6) | (byte & 0x3F);
    }
    else
    {
        st->utf8_acc = ((uint32_t)0xFF >> cls) & byte;
    }

    st->utf8_state = utf8_transitions[st->utf8_state + cls];

    if (st->utf8_state == UTF8_ACCEPT)
    {
        *cp = st->utf8_acc;
        st->utf8_acc = 0;
        return true;
    }
    else if (st->utf8_state == UTF8_REJECT)
    {
        st->utf8_acc = 0;
        st->utf8_state = UTF8_ACCEPT;
        *cp = 0xFFFD; /* replacement character */
        return initial_state == UTF8_ACCEPT;
    }
    else
    {
        *cp = 0xFFFFFFFF; /* none — mid-sequence */
        return true;
    }
}

/* Execute a C0 control character (mirrors Ghostty's stream.execute). */
static void stream_execute_c0(ghostcon_stream_t *st, ghostcon_screen_t *s, uint8_t c)
{
    (void)st;
    switch (c)
    {
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

/* Route a decoded codepoint: ESC to escape state, C0 to execute, DEL
 * ignored, everything else printed.
 */
static void stream_handle_codepoint(ghostcon_stream_t *st, ghostcon_screen_t *s, uint32_t cp)
{
    if (cp == 0x1B)
    {
        st->state = GC_STREAM_ESC;
        return;
    }
    if (cp < 0x20)
    {
        stream_execute_c0(st, s, (uint8_t)cp);
        return;
    }
    if (cp == 0x7F) return; /* DEL */
    ghostcon_screen_put_char(s, cp);
}

void ghostcon_stream_reset(ghostcon_stream_t *st)
{
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

/* CSI parameter parsing */

/* Main state machine — per-byte processing */

/* Ordinary UTF-8 text, plus a pending OSC/APC ESC-\ terminator. */
static void stream_state_ground(ghostcon_stream_t *st, uint8_t c, ghostcon_screen_t *s)
{
    /* Check for pending ST (ESC \) terminator for OSC */
    if (st->osc_pending)
    {
        if (c == '\\')
        {
            st->osc_terminated_by_bel = false;
            gc_stream_osc_dispatch(st, s);
            st->osc_len = 0;
        }
        st->osc_pending = false;
        if (c == '\\') return;
    }
    if (st->apc_pending)
    {
        if (c == '\\' && !st->apc_overflow)
            gc_stream_apc_dispatch(st, s);
        st->apc_pending = false;
        st->apc_len = 0;
        st->apc_overflow = false;
        if (c == '\\') return;
    }

    /* Every byte in ground is UTF-8. The decoder emits codepoints;
     * control characters and ESC are routed out via
     * stream_handle_codepoint (mirrors Ghostty's nextUtf8).
     */
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
}

/* Byte after a lone ESC: pick the escape-string/CSI/ESC-intermediate branch. */
static void stream_state_esc(ghostcon_stream_t *st, uint8_t c, ghostcon_screen_t *s)
{
    st->state = GC_STREAM_GROUND;

    if (c == '[')
    {
        st->state = GC_STREAM_CSI_ENTRY;
    }
    else if (c == ']')
    {
        st->state = GC_STREAM_OSC_STRING;
        st->osc_len = 0;
    }
    else if (c == 'P')
    {
        st->state = GC_STREAM_DCS_ENTRY;
        st->dcs_len = 0;
    }
    else if (c == 'X' || c == '^' || c == '_')
    {
        st->state = GC_STREAM_SOS_PM_APC_STRING;
        st->apc_introducer = (char)c;
        st->apc_len = 0;
        st->apc_overflow = false;
    }
    else if (c >= 0x20 && c <= 0x2F)
    {
        /* Intermediate byte */
        st->intermediates[0] = c;
        st->intermediates_idx = 1;
        st->state = GC_STREAM_ESC_INTERMEDIATE;
    }
    else if (c >= 0x30 && c <= 0x7E)
    {
        /* Final byte — dispatch */
        switch (c)
        {
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
    }
    else if (c == '[')
    {
        st->state = GC_STREAM_CSI_ENTRY;
    }
    else if (c == ']')
    {
        st->state = GC_STREAM_OSC_STRING;
        st->osc_len = 0;
    }
    else if (c == 'P')
    {
        st->state = GC_STREAM_DCS_ENTRY;
        st->dcs_len = 0;
    }
    else if (c == 'X' || c == '^' || c == '_')
    {
        /* SOS, PM, APC */
        st->state = GC_STREAM_SOS_PM_APC_STRING;
        st->apc_introducer = (char)c;
        st->apc_len = 0;
        st->apc_overflow = false;
    }
}

/* Collect ESC intermediates; a final byte ends the sequence. */
static void stream_state_esc_intermediate(ghostcon_stream_t *st, uint8_t c)
{
    if (c >= 0x20 && c <= 0x2F)
    {
        if (st->intermediates_idx < GC_STREAM_MAX_INTERMEDIATES)
            st->intermediates[st->intermediates_idx++] = c;
    }
    else if (c >= 0x30 && c <= 0x7E)
    {
        st->state = GC_STREAM_GROUND;
        /* Dispatch with intermediates */
    }
    else
    {
        st->state = GC_STREAM_GROUND;
    }
}

/* First CSI byte: parameter, intermediate, or final dispatch. */
static void stream_state_csi_entry(ghostcon_stream_t *st, uint8_t c, ghostcon_screen_t *s)
{
    st->intermediates_idx = 0;
    st->params_idx = 0;
    st->param_acc = 0;
    st->param_acc_idx = 0;

    if (c >= 0x30 && c <= 0x3F)
    {
        /* Parameter byte */
        st->state = GC_STREAM_CSI_PARAM;
        if (c >= 0x3C && c <= 0x3F)
        {
            /* Private marker (< = > ?): Ghostty's parser collects these
             * into the intermediate array when they lead the sequence
             * (csi_entry .collect -> csi_param). They are NOT params.
             */
            if (st->intermediates_idx < GC_STREAM_MAX_INTERMEDIATES)
                st->intermediates[st->intermediates_idx++] = c;
        }
        else if (c >= '0' && c <= '9')
        {
            st->param_acc = c - '0';
            st->param_acc_idx = 1;
        }
        else if (c == ';')
        {
            st->params[0] = 0;
            st->params_idx = 1;
        }
    }
    else if (c >= 0x20 && c <= 0x2F)
    {
        st->state = GC_STREAM_CSI_INTERMEDIATE;
        if (st->intermediates_idx < GC_STREAM_MAX_INTERMEDIATES)
            st->intermediates[st->intermediates_idx++] = c;
    }
    else if (c >= 0x40 && c <= 0x7E)
    {
        /* Final byte */
        st->state = GC_STREAM_GROUND;
        gc_stream_csi_handler_t handler = gc_stream_csi_dispatch(st, c);
        if (handler) handler(st, s);
    }
    else
    {
        st->state = GC_STREAM_GROUND;
    }
}

/* Accumulate CSI parameters, then flush on intermediate/final. */
static void stream_state_csi_param(ghostcon_stream_t *st, uint8_t c, ghostcon_screen_t *s)
{
    if (c >= '0' && c <= '9')
    {
        /* Wrapping accumulation matching Ghostty's `*|`/`+|` (u16
         * wrap). Use u32 intermediate so a huge digit run can't
         * overflow int.
         */
        st->param_acc = (uint16_t)((uint32_t)st->param_acc * 10 + (c - '0'));
        st->param_acc_idx++;
    }
    else if (c == ';')
    {
        if (st->params_idx < GC_STREAM_MAX_PARAMS)
        {
            st->params[st->params_idx++] = st->param_acc;
        }
        st->param_acc = 0;
        st->param_acc_idx = 0;
    }
    else if (c >= 0x20 && c <= 0x2F)
    {
        /* Flush current param */
        if (st->params_idx < GC_STREAM_MAX_PARAMS)
            st->params[st->params_idx++] = st->param_acc;
        st->state = GC_STREAM_CSI_INTERMEDIATE;
        if (st->intermediates_idx < GC_STREAM_MAX_INTERMEDIATES)
            st->intermediates[st->intermediates_idx++] = c;
    }
    else if (c >= 0x40 && c <= 0x7E)
    {
        /* Flush current param */
        if (st->params_idx < GC_STREAM_MAX_PARAMS)
            st->params[st->params_idx++] = st->param_acc;
        /* Final byte */
        st->state = GC_STREAM_GROUND;
        gc_stream_csi_handler_t handler = gc_stream_csi_dispatch(st, c);
        if (handler) handler(st, s);
    }
    else if (c >= 0x3C && c <= 0x3F)
    {
        /* Private marker mid-params: Ghostty ignores the whole sequence
         * (csi_param 0x3C-0x3F -> csi_ignore). Private markers are only
         * valid as the first byte, handled in CSI_ENTRY.
         */
        st->state = GC_STREAM_CSI_IGNORE;
    }
    else
    {
        st->state = GC_STREAM_CSI_IGNORE;
    }
}

/* Collect CSI intermediates; a final byte dispatches. */
static void stream_state_csi_intermediate(ghostcon_stream_t *st, uint8_t c, ghostcon_screen_t *s)
{
    if (c >= 0x20 && c <= 0x2F)
    {
        if (st->intermediates_idx < GC_STREAM_MAX_INTERMEDIATES)
            st->intermediates[st->intermediates_idx++] = c;
    }
    else if (c >= 0x40 && c <= 0x7E)
    {
        st->state = GC_STREAM_GROUND;
        gc_stream_csi_handler_t handler = gc_stream_csi_dispatch(st, c);
        if (handler) handler(st, s);
    }
    else
    {
        st->state = GC_STREAM_CSI_IGNORE;
    }
}

/* Swallow a malformed CSI until its final byte. */
static void stream_state_csi_ignore(ghostcon_stream_t *st, uint8_t c)
{
    if (c >= 0x40 && c <= 0x7E)
        st->state = GC_STREAM_GROUND;
}

/* Accumulate an OSC payload until BEL, ESC \, or a C0 control. */
static void stream_state_osc_string(ghostcon_stream_t *st, uint8_t c, ghostcon_screen_t *s)
{
    if (c == 0x07)
    {
        /* BEL terminated — dispatch immediately */
        st->osc_terminated_by_bel = true;
        gc_stream_osc_dispatch(st, s);
        st->osc_len = 0;
        st->state = GC_STREAM_GROUND;
    }
    else if (c == 0x1B)
    {
        /* Potentially ST (ESC \) — flag and wait for next byte */
        st->osc_pending = true;
        st->state = GC_STREAM_GROUND;
    }
    else if (c < 0x20 && c != 0x09 && c != 0x0A)
    {
        /* C0 controls (except TAB/LF) terminate OSC */
        st->state = GC_STREAM_GROUND;
    }
    else
    {
        if (st->osc_len < st->buf_cap - 1)
        {
            st->buf[st->osc_len++] = (char)c;
            st->buf[st->osc_len] = '\0';
        }
    }
}

/* First DCS byte: parameter, intermediate, or final. */
static void stream_state_dcs_entry(ghostcon_stream_t *st, uint8_t c)
{
    st->intermediates_idx = 0;
    st->params_idx = 0;
    st->param_acc = 0;

    if (c >= 0x30 && c <= 0x3F)
    {
        st->state = GC_STREAM_DCS_PARAM;
    }
    else if (c >= 0x20 && c <= 0x2F)
    {
        st->state = GC_STREAM_DCS_INTERMEDIATE;
        if (st->intermediates_idx < GC_STREAM_MAX_INTERMEDIATES)
            st->intermediates[st->intermediates_idx++] = c;
    }
    else if (c >= 0x40 && c <= 0x7E)
    {
        st->state = GC_STREAM_DCS_PASSTHROUGH;
        st->dcs_len = 0;
        st->dcs_final = (char)c;
    }
    else
    {
        st->state = GC_STREAM_GROUND;
    }
}

/* Accumulate DCS parameters, then flush on intermediate/final. */
static void stream_state_dcs_param(ghostcon_stream_t *st, uint8_t c)
{
    if (c >= '0' && c <= '9')
    {
        st->param_acc = (uint16_t)((uint32_t)st->param_acc * 10 + (c - '0'));
    }
    else if (c == ';')
    {
        if (st->params_idx < GC_STREAM_MAX_PARAMS)
            st->params[st->params_idx++] = st->param_acc;
        st->param_acc = 0;
    }
    else if (c >= 0x20 && c <= 0x2F)
    {
        /* Flush the pending param
         *
         * matches CSI_PARAM's own final-byte handling (see that state's own
         * comment); without this, a DCS param string with no trailing ';' before
         * the final byte (e.g. sixel's "P1;P2 q" with no ';' after P2) silently
         * loses that last value.
         */
        if (st->params_idx < GC_STREAM_MAX_PARAMS)
            st->params[st->params_idx++] = st->param_acc;
        st->state = GC_STREAM_DCS_INTERMEDIATE;
        if (st->intermediates_idx < GC_STREAM_MAX_INTERMEDIATES)
            st->intermediates[st->intermediates_idx++] = c;
    }
    else if (c >= 0x40 && c <= 0x7E)
    {
        if (st->params_idx < GC_STREAM_MAX_PARAMS)
            st->params[st->params_idx++] = st->param_acc;
        st->state = GC_STREAM_DCS_PASSTHROUGH;
        st->dcs_len = 0;
        st->dcs_final = (char)c;
    }
    else
    {
        st->state = GC_STREAM_GROUND;
    }
}

/* Collect DCS intermediates; a final byte enters passthrough. */
static void stream_state_dcs_intermediate(ghostcon_stream_t *st, uint8_t c)
{
    if (c >= 0x20 && c <= 0x2F)
    {
        if (st->intermediates_idx < GC_STREAM_MAX_INTERMEDIATES)
            st->intermediates[st->intermediates_idx++] = c;
    }
    else if (c >= 0x40 && c <= 0x7E)
    {
        st->state = GC_STREAM_DCS_PASSTHROUGH;
        st->dcs_len = 0;
        st->dcs_final = (char)c;
    }
    else
    {
        st->state = GC_STREAM_GROUND;
    }
}

/* Accumulate a DCS payload until ESC \ terminates it. */
static void stream_state_dcs_passthrough(ghostcon_stream_t *st, uint8_t c)
{
    /* Only the 7-bit ESC-backslash form of ST terminates
     *
     * NOT a raw 0x9C byte, which is also a valid UTF-8 continuation byte (see
     * this function's own doc comment on the top-level C1 guard for the real bug
     * this caused with an emoji mid-OSC-payload; DCS payloads can equally contain
     * UTF-8 text, so the same fix applies here).
     */
    if (c == 0x1B)
    {
        st->state = GC_STREAM_DCS_PASSTHROUGH_ESC;
    }
    else
    {
        if (st->dcs_len + 1 >= st->buf_cap && !gc_stream_dcs_buf_grow(st))
        {
            /* At GHOSTCON_STREAM_DCS_MAX_BUF or allocation failed
             * --reject rather than silently truncate-and-accept a
             * corrupt/incomplete image (same posture as apc_overflow
             * elsewhere in this file).
             */
            st->state = GC_STREAM_DCS_IGNORE;
            return;
        }
        st->buf[st->dcs_len++] = (char)c;
        st->buf[st->dcs_len] = '\0';
    }
}

/* Confirm the DCS ESC-\ terminator, else resume passthrough. */
static void stream_state_dcs_passthrough_esc(ghostcon_stream_t *st, uint8_t c, ghostcon_screen_t *s)
{
    if (c == '\\')
    {
        /* ST (ESC \) terminates */
        st->state = GC_STREAM_GROUND;
        gc_stream_dcs_dispatch(st, s);
    }
    else if (c == 0x1B)
    {
        /* Double ESC — stay in this state */
    }
    else
    {
        /* Not ST — return to passthrough */
        st->state = GC_STREAM_DCS_PASSTHROUGH;
    }
}

/* Swallow an oversized/invalid DCS until its terminator. */
static void stream_state_dcs_ignore(ghostcon_stream_t *st, uint8_t c)
{
    if ((c >= 0x40 && c <= 0x7E) || c == 0x9C || c == 0x1B)
        st->state = GC_STREAM_GROUND;
}

/* Accumulate an APC payload (Kitty graphics); SOS/PM are dropped. */
static void stream_state_sos_pm_apc_string(ghostcon_stream_t *st, uint8_t c)
{
    /* Only the 7-bit ESC-backslash form of ST terminates
     *
     * same reasoning as DCS_PASSTHROUGH above: a raw 0x9C is also a valid UTF-8
     * continuation byte, and APC payloads (Kitty graphics base64 data) can
     * contain byte values that overlap it.
     *
     * Sets apc_pending and returns to GROUND on ESC, mirroring OSC_STRING's own
     * handling below it
     *
     * GROUND's pending-check block (above) is what actually confirms the '\' and
     * dispatches; without that flag, a real APC payload's terminator would leak
     * its trailing '\' onto the screen as literal text (SOS/PM, which nothing
     * here acts on, still just accumulate-then-drop like before).
     */
    if (c == 0x1B)
    {
        st->apc_pending = true;
        st->state = GC_STREAM_GROUND;
    }
    else if (st->apc_introducer == '_')
    {
        if (st->apc_len < st->buf_cap - 1)
        {
            st->buf[st->apc_len++] = (char)c;
            st->buf[st->apc_len] = '\0';
        }
        else
        {
            st->apc_overflow = true;
        }
    }
}

/* Handle an 8-bit C1 control (0x80-0x9F) when the current state is one that
 * terminates on C1, returning true if the byte was consumed.
 *
 * GROUND is excluded: there a C1-range byte is UTF-8 continuation/lead data
 * handled by the decoder (matching Ghostty, a stray C1 in ground decodes to
 * U+FFFD).
 *
 * The opaque string-payload states (OSC, DCS-passthrough, SOS/PM/APC) are
 * excluded too -- they are BEL/ESC-backslash-terminated on their own and must
 * treat C1-range bytes as ordinary payload.
 *
 * Real bug found live: 0x9C is both the C1 code for ST and a valid UTF-8
 * continuation byte, so a window title containing e.g. U+2733 "✳" (E2 9C B3)
 * arriving mid-OSC-payload used to be truncated here, after which the rest of the
 * title leaked onto the screen as literal text.
 */
static bool stream_handle_c1(ghostcon_stream_t *st, uint8_t c, ghostcon_screen_t *s)
{
    if (c < 0x80 || c > 0x9F)
        return false;
    if (st->state == GC_STREAM_GROUND || st->state == GC_STREAM_OSC_STRING ||
        st->state == GC_STREAM_DCS_PASSTHROUGH ||
        st->state == GC_STREAM_DCS_PASSTHROUGH_ESC ||
        st->state == GC_STREAM_SOS_PM_APC_STRING)
        return false;

    switch (c)
    {
    case 0x84: /* IND */ ghostcon_screen_linefeed(s); return true;
    case 0x85: /* NEL */ ghostcon_screen_carriage_return(s); ghostcon_screen_linefeed(s); return true;
    case 0x88: /* HTS */ ghostcon_screen_tab_set(s); return true;
    case 0x8D: /* RI */ ghostcon_screen_reverse_index(s); return true;
    case 0x8E: /* SS2 */ return true;
    case 0x8F: /* SS3 */ return true;
    case 0x9B: /* CSI */ st->state = GC_STREAM_CSI_ENTRY; return true;
    case 0x9C: /* ST */ st->state = GC_STREAM_GROUND; return true;
    case 0x9D: /* OSC */ st->state = GC_STREAM_OSC_STRING; return true;
    case 0x90: /* DCS */ st->state = GC_STREAM_DCS_ENTRY; return true;
    case 0x98: /* SOS */ st->state = GC_STREAM_SOS_PM_APC_STRING; st->apc_introducer = 'X'; st->apc_len = 0; st->apc_overflow = false; return true;
    case 0x9E: /* PM */ st->state = GC_STREAM_SOS_PM_APC_STRING; st->apc_introducer = '^'; st->apc_len = 0; st->apc_overflow = false; return true;
    case 0x9F: /* APC */ st->state = GC_STREAM_SOS_PM_APC_STRING; st->apc_introducer = '_'; st->apc_len = 0; st->apc_overflow = false; return true;
    default: return false; /* unmapped C1 byte: fall through to the state machine */
    }
}

void ghostcon_stream_process_byte(ghostcon_stream_t *st, uint8_t c, ghostcon_screen_t *s)
{
    if (stream_handle_c1(st, c, s))
        return;

    switch (st->state)
    {
    case GC_STREAM_GROUND:
        stream_state_ground(st, c, s);
        break;
    case GC_STREAM_ESC:
        stream_state_esc(st, c, s);
        break;
    case GC_STREAM_ESC_INTERMEDIATE:
        stream_state_esc_intermediate(st, c);
        break;
    case GC_STREAM_CSI_ENTRY:
        stream_state_csi_entry(st, c, s);
        break;
    case GC_STREAM_CSI_PARAM:
        stream_state_csi_param(st, c, s);
        break;
    case GC_STREAM_CSI_INTERMEDIATE:
        stream_state_csi_intermediate(st, c, s);
        break;
    case GC_STREAM_CSI_IGNORE:
        stream_state_csi_ignore(st, c);
        break;
    case GC_STREAM_OSC_STRING:
        stream_state_osc_string(st, c, s);
        break;
    case GC_STREAM_DCS_ENTRY:
        stream_state_dcs_entry(st, c);
        break;
    case GC_STREAM_DCS_PARAM:
        stream_state_dcs_param(st, c);
        break;
    case GC_STREAM_DCS_INTERMEDIATE:
        stream_state_dcs_intermediate(st, c);
        break;
    case GC_STREAM_DCS_PASSTHROUGH:
        stream_state_dcs_passthrough(st, c);
        break;
    case GC_STREAM_DCS_PASSTHROUGH_ESC:
        stream_state_dcs_passthrough_esc(st, c, s);
        break;
    case GC_STREAM_DCS_IGNORE:
        stream_state_dcs_ignore(st, c);
        break;
    case GC_STREAM_SOS_PM_APC_STRING:
        stream_state_sos_pm_apc_string(st, c);
        break;
    }
}

void ghostcon_stream_process(ghostcon_stream_t *st, const uint8_t *data, size_t len, ghostcon_screen_t *s)
{
    for (size_t i = 0; i < len; i++)
        ghostcon_stream_process_byte(st, data[i], s);
}
