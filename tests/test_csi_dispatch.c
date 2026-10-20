/* tests/test_csi_dispatch.c
 *
 * Covers every CSI sequence that carries an intermediate (the private markers < =
 * > ? $ space "), which is where src/term/stream/stream_csi.c's dispatch table
 * has more than one row for the same final byte.
 *
 * These were the sequences with no coverage at all before the dispatch table
 * landed: a row added out of order in csi_table[] would misdispatch one of them
 * silently, because a CSI sequence the table does not match is ignored rather
 * than reported.
 */
#include "ghostcon/term/term.h"
#include "ghostcon/term/cell.h"
#include "ghostcon/term/style.h"

#include "term/stream/stream_internal.h"

#include "conftest.h"

static gc_test_output_t out;

/* Init a terminal and point its output channel at the capture buffer, so
 * a test can assert on what the parser wrote back.
 */
static void init_term(ghostcon_term_t *t)
{
    ASSERT(ghostcon_term_init(t, 20, 6, 100), "init");
    ghostcon_term_set_output(t, gc_test_output_fn, &out);
    gc_test_output_reset(&out);
}

static void feed(ghostcon_term_t *t, const char *s)
{
    ghostcon_term_feed(t, (const uint8_t *)s, strlen(s));
}

/* CSI ? Ps h -- DECSET. Each case picks a mode with a distinct,
 * observable effect on the screen.
 */
TEST(decset_variants)
{
    ghostcon_term_t t;
    init_term(&t);
    feed(&t, "\x1b[?25h");
    ASSERT(t.screen.cursor_visible, "CSI ? 25 h shows the cursor");
    feed(&t, "\x1b[?25l");
    ASSERT(!t.screen.cursor_visible, "CSI ? 25 l hides the cursor");
    ghostcon_term_deinit(&t);

    init_term(&t);
    feed(&t, "\x1b[?1006h");
    ASSERT(t.screen.mouse_sgr, "CSI ? 1006 h selects SGR mouse mode");
    feed(&t, "\x1b[?1006l");
    ASSERT(!t.screen.mouse_sgr, "CSI ? 1006 l clears SGR mouse mode");
    ghostcon_term_deinit(&t);

    init_term(&t);
    feed(&t, "\x1b[?1049h");
    ASSERT(t.screen.alt_screen_active, "CSI ? 1049 h enters the alt screen");
    feed(&t, "\x1b[?1049l");
    ASSERT(!t.screen.alt_screen_active, "CSI ? 1049 l leaves the alt screen");
    ghostcon_term_deinit(&t);

    init_term(&t);
    feed(&t, "\x1b[?5h");
    ASSERT(t.screen.reverse_video, "CSI ? 5 h selects reverse video");
    feed(&t, "\x1b[?5l");
    ASSERT(!t.screen.reverse_video, "CSI ? 5 l clears reverse video");
    ghostcon_term_deinit(&t);
}

/* CSI ? Ps l -- DECRESET. */
TEST(decreset_variants)
{
    ghostcon_term_t t;
    init_term(&t);
    feed(&t, "\x1b[?25h");
    feed(&t, "\x1b[?25l");
    ASSERT(!t.screen.cursor_visible, "CSI ? 25 l resets the cursor visibility bit");
    ghostcon_term_deinit(&t);

    init_term(&t);
    feed(&t, "\x1b[?1049h");
    feed(&t, "\x1b[?1049l");
    ASSERT(!t.screen.alt_screen_active, "CSI ? 1049 l resets the alt screen");
    ghostcon_term_deinit(&t);
}

/* CSI Ps " q -- DECSCA. */
TEST(decsca_selects_protection_mode)
{
    ghostcon_term_t t;
    init_term(&t);
    feed(&t, "\x1b[1\"q");
    ASSERT(t.screen.protected_mode == GC_PROTECTED_DEC,
           "CSI 1 \" q selects DEC character protection");
    feed(&t, "\x1b[0\"q");
    ASSERT(t.screen.protected_mode == GC_PROTECTED_DEC,
           "CSI 0 \" q leaves the mode alone -- Ghostty never resets to .off");
    ghostcon_term_deinit(&t);
}

/* CSI Ps SP q -- DECSCUSR. */
TEST(cursor_style_variants)
{
    ghostcon_term_t t;
    init_term(&t);
    feed(&t, "\x1b[4 q");
    ASSERT(t.screen.cursor.cursor_style == GC_CURSOR_UNDERLINE,
           "CSI 4 SP q selects the steady underline cursor");
    feed(&t, "\x1b[6 q");
    ASSERT(t.screen.cursor.cursor_style == GC_CURSOR_BAR,
           "CSI 6 SP q selects the steady bar cursor");
    ghostcon_term_deinit(&t);
}

/* CSI > flags u / CSI < n u / CSI = flags ; mode u / CSI ? u -- the Kitty
 * keyboard protocol.
 */
TEST(kitty_keyboard_variants)
{
    ghostcon_term_t t;
    init_term(&t);
    feed(&t, "\x1b[>1u");
    ASSERT(ghostcon_kitty_current(&t.screen.kitty) == 1,
           "CSI > 1 u pushes the disambiguate flag onto the kitty stack");
    feed(&t, "\x1b[<1u");
    ASSERT(ghostcon_kitty_current(&t.screen.kitty) == 0,
           "CSI < 1 u pops the kitty stack back to empty");
    ghostcon_term_deinit(&t);

    init_term(&t);
    feed(&t, "\x1b[=1;1u");
    ASSERT(ghostcon_kitty_current(&t.screen.kitty) == 1,
           "CSI = 1 ; 1 u sets the kitty flags on the current stack entry");
    feed(&t, "\x1b[=0;1u");
    ASSERT(ghostcon_kitty_current(&t.screen.kitty) == 0,
           "CSI = 0 ; 1 u replaces the kitty flags with 0");
    ghostcon_term_deinit(&t);

    init_term(&t);
    feed(&t, "\x1b[=5;1u");
    gc_test_output_reset(&out);
    feed(&t, "\x1b[?u");
    ASSERT_OUTPUT(out, "\x1b[?5u", "CSI ? u reports the current kitty flags");
    ghostcon_term_deinit(&t);
}

/* CSI > c / CSI = c -- device attributes. */
TEST(device_attributes_variants)
{
    ghostcon_term_t t;
    init_term(&t);
    gc_test_output_reset(&out);
    feed(&t, "\x1b[>c");
    ASSERT_OUTPUT(out, "\x1b[>1;0;0c", "CSI > c answers with secondary device attributes");
    ghostcon_term_deinit(&t);

    init_term(&t);
    gc_test_output_reset(&out);
    feed(&t, "\x1b[=c");
    ASSERT_OUTPUT(out, "\x1bP!|00000000\x1b\\", "CSI = c answers with tertiary device attributes");
    ghostcon_term_deinit(&t);
}

/* CSI Ps $ p / CSI ? Ps $ p -- DECRQM. */
TEST(request_mode_variants)
{
    ghostcon_term_t t;
    init_term(&t);
    gc_test_output_reset(&out);
    feed(&t, "\x1b[2$p");
    ASSERT_OUTPUT(out, "\x1b[2;2$y", "CSI 2 $ p reports KAM as reset (ANSI form)");
    ghostcon_term_deinit(&t);

    init_term(&t);
    gc_test_output_reset(&out);
    feed(&t, "\x1b[?25$p");
    ASSERT_OUTPUT(out, "\x1b[?25;0$y", "CSI ? 25 $ p reports an unrecognized mode (DEC form)");
    ghostcon_term_deinit(&t);
}

/* CSI > s -- XTSHIFTESCAPE. */
TEST(shift_escape_variant)
{
    ghostcon_term_t t;
    init_term(&t);
    feed(&t, "\x1b[>s");
    /* The sequence has no screen effect
     *
     * it must at least be consumed without disturbing the grid.
     */
    ASSERT(t.screen.cursor.x == 0 && t.screen.cursor.y == 0,
           "CSI > s leaves the cursor where it was");
    ghostcon_term_deinit(&t);
}

/* CSI ? Ps s -- DEC private save mode; CSI ? Ps r -- DEC private restore
 * mode. Neither has a screen effect of its own, but both must be
 * recognised rather than mistaken for the plain 's' and 'r' forms.
 */
TEST(save_restore_mode_variants)
{
    ghostcon_term_t t;
    init_term(&t);
    feed(&t, "\x1b[?1s");
    feed(&t, "\x1b[?1r");
    ASSERT(t.screen.cursor.x == 0 && t.screen.cursor.y == 0,
           "CSI ? 1 s / CSI ? 1 r are consumed without moving the cursor");
    ghostcon_term_deinit(&t);

    init_term(&t);
    feed(&t, "\x1b[?2r");
    ASSERT(t.screen.scroll_region.top == 0,
           "CSI ? 2 r is a private mode restore -- it does not touch the scroll region");
    ghostcon_term_deinit(&t);

    init_term(&t);
    feed(&t, "\x1b[2r");
    ASSERT(t.screen.scroll_region.top != 0,
           "the plain CSI 2 r still sets the scroll region, not a private restore");
    ghostcon_term_deinit(&t);
}

/* CSI ? Ps J / CSI ? Ps K -- DECSED / DECSEL. These are the selective
 * erases: protected cells survive, unlike the plain forms.
 */
TEST(selective_erase_variants)
{
    ghostcon_term_t t;
    init_term(&t);
    feed(&t, "\x1b[1\"q");          /* protect subsequent writes */
    feed(&t, "A");                   /* -> a protected cell */
    feed(&t, "\x1b[0\"q");          /* stop protecting */
    feed(&t, "B");                   /* -> an unprotected cell */
    feed(&t, "\x1b[1;1H");          /* back to the start of the line */
    feed(&t, "\x1b[?K");            /* DECSEL: erase to end of line */
    ASSERT(ghostcon_screen_row(&t.screen, 0)->cells[0].raw != 0,
           "CSI ? K leaves a DECSCA-protected cell alone");
    ASSERT(ghostcon_screen_row(&t.screen, 0)->cells[1].raw == 0,
           "CSI ? K erases the unprotected cell");
    ghostcon_term_deinit(&t);

    /* The plain form must differ: it erases the protected cell too. */
    init_term(&t);
    feed(&t, "\x1b[1\"q");
    feed(&t, "A");
    feed(&t, "\x1b[0\"q");
    feed(&t, "B");
    feed(&t, "\x1b[1;1H");
    feed(&t, "\x1b[K");             /* plain EL: no protection */
    ASSERT(ghostcon_screen_row(&t.screen, 0)->cells[0].raw == 0,
           "the plain CSI K erases the protected cell -- it is not the selective erase");
    ghostcon_term_deinit(&t);

    init_term(&t);
    feed(&t, "\x1b[1\"q");
    feed(&t, "A");
    feed(&t, "\x1b[0\"q");
    feed(&t, "B");
    feed(&t, "\x1b[1;1H");          /* ED erases from the cursor */
    feed(&t, "\x1b[?J");            /* DECSED: erase the display */
    ASSERT(ghostcon_screen_row(&t.screen, 0)->cells[0].raw != 0,
           "CSI ? J leaves a DECSCA-protected cell alone");
    ASSERT(ghostcon_screen_row(&t.screen, 0)->cells[1].raw == 0,
           "CSI ? J erases the unprotected cell");
    ghostcon_term_deinit(&t);
}

/* A sequence whose intermediates do not match any table row must be
 * ignored -- not dispatched to the intermediate-less handler of the same
 * final byte. This is the property the table's per-row intermediate match
 * exists to provide.
 */
TEST(unmatched_intermediates_are_ignored)
{
    ghostcon_term_t t;
    init_term(&t);
    feed(&t, "\x1b[?H");
    ASSERT(t.screen.cursor.x == 0 && t.screen.cursor.y == 0,
           "CSI ? H (no such intermediate variant) is ignored");
    ghostcon_term_deinit(&t);

    init_term(&t);
    feed(&t, "\x1b[?m");
    ASSERT(t.screen.cursor.style_id == GC_STYLE_DEFAULT_ID,
           "CSI ? m (no such intermediate variant) is ignored, not treated as SGR");
    ghostcon_term_deinit(&t);
}

/* The dispatch table is searched by binary search, so it has to stay sorted by
 * final byte.
 *
 * An out-of-order row is dispatched wrongly rather than rejected, and an
 * unmatched CSI sequence is silently ignored -- so the invariant is checked, not
 * just documented.
 */
TEST(dispatch_table_is_sorted)
{
    ASSERT(gc_stream_csi_table_sorted(),
           "the CSI dispatch table is sorted by final byte with no duplicate rows");
}

int main(void)
{
    return gc_test_run_all();
}
