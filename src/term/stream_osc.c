#define _DEFAULT_SOURCE
#include "stream_internal.h"
#include "ghostcon/core/log.h"
#include <ghostty/vt/osc.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* OSC dispatch (see stream_internal.h)
 *
 * Split out of stream.c per PLAN.md "Phase 2": OSC 4/10/11/12/104, 7, 8,
 * 9/777, 52, 133/633 live in the manual number tier below; everything else
 * falls through to the libghostty-vt classification loop at the bottom of
 * this file.
 */

/* OSC dispatch — manual (number-level) tier
 *
 * The installed libghostty-vt's OSC C API only exposes coarse command
 * classification plus window-title text extraction (verified against both
 * the distro package and a from-source master build with -Demit-lib-vt=true
 * — see color.h's own note). Everything else is parsed by us directly from
 * st->buf, the raw NUL-terminated "N;p1;p2;..." OSC payload we already own
 * from accumulating OSC bytes ourselves in the state machine below. This
 * sidesteps the API's classification ambiguity too: e.g. OSC 4/10/11/12/104
 * (all color operations) collapse into one
 * GHOSTTY_OSC_COMMAND_COLOR_OPERATION type with no way to tell them apart
 * via the API — we just read the leading number ourselves instead.
 */

/* Splits `buf` (length `len`, NUL-terminated) on ';' in place, writing up
 * to `max_fields` pointers into `fields`. fields[0] is always the leading
 * OSC number. Returns the field count.
 */
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
 * storing (and later echoing back through a query) arbitrary garbage a
 * misbehaving program sent. Doesn't validate padding placement/length is a
 * multiple of 4 -- xterm itself is lenient about this, and being stricter
 * here would just make a legitimate-but-slightly-malformed payload from
 * some other terminal's app silently vanish instead of being stored as-is,
 * which is more surprising than useful.
 */
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
 * common case, emitted by bash/zsh/fish's OSC 7 hooks) a file://[host]/path
 * URI. The host component is discarded -- this terminal has no notion of
 * "which host a path is valid on" beyond "whichever one is currently
 * attached", and xterm's own recommendation is to accept it without
 * validating against the local hostname. Percent-encoded bytes are decoded;
 * a lone/invalid '%' escape is passed through literally rather than
 * rejecting the whole sequence, matching OSC 7's "best effort, advisory"
 * spirit.
 */
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
 * recognized (whether or not the specific field contents were valid — a
 * malformed field is silently ignored per the terminal's usual "never crash
 * on bad input" rule, not treated as "unrecognized OSC"). Returns false for
 * anything this tier doesn't own, so the caller can fall through to the
 * ghostty_osc-based path (window title, and the discard-and-log fallback
 * for truly unrecognized sequences).
 */
static bool
osc_dispatch_manual(ghostcon_stream_t *st, ghostcon_screen_t *s)
{
    /* Work on a local copy, not st->buf directly: osc_split_fields()
     * mutates in place (';' -> '\0'), and if this OSC number turns out not
     * to be one we own, the caller falls through to the existing
     * ghostty_osc_next() loop over st->buf -- which needs the original
     * semicolons intact (e.g. window title "0;My Title" would parse as
     * corrupted "0\0My Title" otherwise). Payloads too long for this are
     * definitely not one of our small numeric OSC types anyway.
     */
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
                /* 133;A/B/C/D cover FinalTerm; 633 additionally sends E
                 * (command line text) and others we don't yet store
                 * anywhere -- silently ignored, matches OSC 8's stub tier
                 * philosophy of "don't crash on the unsupported remainder
                 * of a spec".
                 */
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

/* OSC dispatch */

void
gc_stream_osc_dispatch(ghostcon_stream_t *st, ghostcon_screen_t *s) {
    if (osc_dispatch_manual(st, s))
        return;

    GhosttyOscParser parser;
    if (ghostty_osc_new(NULL, &parser) != GHOSTTY_SUCCESS)
        return;

    for (uint32_t i = 0; i < st->osc_len; i++)
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
        /* OSC 133 — handled above in osc_dispatch_manual() (OSC 633, the
         * VSCode superset, isn't classified by the library at all since it
         * isn't in GhosttyOscCommandType — it's caught purely by number in
         * the manual tier).
         */
        break;
    case GHOSTTY_OSC_COMMAND_CLIPBOARD_CONTENTS: {
        /* OSC 52 — stub, needs ghostcon-ipc */
        break;
    }
    case GHOSTTY_OSC_COMMAND_REPORT_PWD: {
        /* OSC 7 — handled above in osc_dispatch_manual(); the library
         * classifies the command but exposes none of its URI data, same
         * limitation as the color-operation tier.
         */
        break;
    }
    case GHOSTTY_OSC_COMMAND_MOUSE_SHAPE: {
        /* OSC 22 — store cursor shape request */
        break;
    }
    case GHOSTTY_OSC_COMMAND_COLOR_OPERATION: {
        /* OSC 4/10/11/12/104 — palette operations Stub: handled in Phase 1
         * with proper palette integration
         */
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
        GC_LOG(stream, "OSC discarded (%s)", st->buf);
        break;
    }

    ghostty_osc_free(parser);
}

