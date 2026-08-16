#include "ghostcon/term/term.h"
#include "conftest.h"

/* Shortcut: feed a string using strlen */
static void feed(ghostcon_term_t *t, const char *s) {
    ghostcon_term_feed(t, (const uint8_t *)s, strlen(s));
}

/* ================================================================== */
/* Basic text                                                          */
/* ================================================================== */

TEST(basic_text) {
    ghostcon_term_t t;
    ASSERT(ghostcon_term_init(&t, 80, 24, 500), "init");
    feed(&t, "Hello!");
    ASSERT_CELL(t, 0, 0, 'H', "first char");
    ASSERT_CELL(t, 4, 0, 'o', "fifth char");
    ASSERT_CELL(t, 5, 0, '!', "sixth char");
    ASSERT_CELL(t, 6, 0, 0,   "beyond text is empty");
    ASSERT_CURSOR(t, 6, 0, "cursor after text");
    ghostcon_term_deinit(&t);
}

TEST(text_multi_line) {
    ghostcon_term_t t;
    ASSERT(ghostcon_term_init(&t, 80, 24, 500), "init");
    /* Use \r\n — LF alone doesn't CR in a real terminal */
    feed(&t, "Line1\r\nLine2\r\nLine3");
    ASSERT_CELL(t, 0, 0, 'L', "line1 start");
    ASSERT_CELL(t, 4, 0, '1', "line1 end");
    ASSERT_CELL(t, 0, 1, 'L', "line2 start");
    ASSERT_CELL(t, 4, 1, '2', "line2 end");
    ASSERT_CELL(t, 0, 2, 'L', "line3 start");
    ASSERT_CURSOR(t, 5, 2, "cursor after line3");
    ghostcon_term_deinit(&t);
}

TEST(text_fill_line) {
    ghostcon_term_t t;
    ASSERT(ghostcon_term_init(&t, 80, 24, 500), "init");
    char buf[81];
    memset(buf, 'A', 80);
    buf[0] = 'B';  /* mark start */
    ghostcon_term_feed(&t, (const uint8_t *)buf, 80);
    ASSERT_CELL(t, 0, 0, 'B', "col 0 char");
    ASSERT_CELL(t, 79, 0, 'A', "col 79 char");
    ASSERT_CURSOR(t, 79, 0, "cursor at right margin (pending wrap)");
    ghostcon_term_deinit(&t);
}

TEST(text_full_screen) {
    ghostcon_term_t t;
    ASSERT(ghostcon_term_init(&t, 80, 24, 500), "init");
    for (int i = 0; i < 24 * 80; i++) {
        uint8_t c = (uint8_t)(' ' + (i % 95));
        ghostcon_term_feed(&t, &c, 1);
    }
    ASSERT_CELL(t, 0, 23, (uint32_t)(' ' + ((23*80) % 95)), "last cell");
    ghostcon_term_deinit(&t);
}

/* ================================================================== */
/* Cursor movement                                                     */
/* ================================================================== */

TEST(cursor_up) {
    ghostcon_term_t t;
    ASSERT(ghostcon_term_init(&t, 80, 24, 500), "init");
    feed(&t, "\x1b[5;10H");  /* cursor to 5,10 (1-based) */
    ASSERT_CURSOR(t, 9, 4, "cursor set");
    feed(&t, "\x1b[3A");     /* up 3 */
    ASSERT_CURSOR(t, 9, 1, "cursor up 3");
    feed(&t, "\x1b[A");      /* up 1 (default) */
    ASSERT_CURSOR(t, 9, 0, "cursor up 1");
    feed(&t, "\x1b[A");      /* up 1 at top — clamped */
    ASSERT_CURSOR(t, 9, 0, "cursor up at top");
    ghostcon_term_deinit(&t);
}

TEST(cursor_down) {
    ghostcon_term_t t;
    ASSERT(ghostcon_term_init(&t, 80, 24, 500), "init");
    feed(&t, "\x1b[5;5H");
    ASSERT_CURSOR(t, 4, 4, "cursor set");
    feed(&t, "\x1b[10B");    /* down 10 */
    ASSERT_CURSOR(t, 4, 14, "cursor down 10");
    feed(&t, "\x1b[B");      /* down 1 (default) */
    ASSERT_CURSOR(t, 4, 15, "cursor down 1");
    feed(&t, "\x1b[100B");   /* clamped at bottom */
    ASSERT_CURSOR(t, 4, 23, "cursor down clamped");
    ghostcon_term_deinit(&t);
}

TEST(cursor_left_right) {
    ghostcon_term_t t;
    ASSERT(ghostcon_term_init(&t, 80, 24, 500), "init");
    /* CUP format: ESC [ row ; col H (ECMA-48) */
    feed(&t, "\x1b[12;40H");
    ASSERT_CURSOR(t, 39, 11, "cursor set");
    feed(&t, "\x1b[5D");     /* left 5 */
    ASSERT_CURSOR(t, 34, 11, "left 5");
    feed(&t, "\x1b[3C");     /* right 3 */
    ASSERT_CURSOR(t, 37, 11, "right 3");
    feed(&t, "\x1b[D");      /* left 1 (default) */
    ASSERT_CURSOR(t, 36, 11, "left 1");
    feed(&t, "\x1b[C");      /* right 1 (default) */
    ASSERT_CURSOR(t, 37, 11, "right 1");
    feed(&t, "\x1b[100D");   /* left 100 — clamped */
    ASSERT_CURSOR(t, 0, 11, "left clamped");
    feed(&t, "\x1b[100C");   /* right 100 — clamped */
    ASSERT_CURSOR(t, 79, 11, "right clamped");
    ghostcon_term_deinit(&t);
}

TEST(cursor_position) {
    ghostcon_term_t t;
    ASSERT(ghostcon_term_init(&t, 80, 24, 500), "init");
    feed(&t, "\x1b[H");      /* home */
    ASSERT_CURSOR(t, 0, 0, "home");
    feed(&t, "\x1b[5;10H");  /* row 5, col 10 (1-based) */
    ASSERT_CURSOR(t, 9, 4, "set 5;10");
    feed(&t, "\x1b[3H");     /* row 3, col 1 (omitted default) */
    ASSERT_CURSOR(t, 0, 2, "set row 3");
    feed(&t, "\x1b[;5H");    /* row default, col 5 */
    ASSERT_CURSOR(t, 4, 0, "set col 5");
    feed(&t, "\x1b[;H");     /* both default = home */
    ASSERT_CURSOR(t, 0, 0, "home default");
    ghostcon_term_deinit(&t);
}

TEST(cursor_save_restore) {
    ghostcon_term_t t;
    ASSERT(ghostcon_term_init(&t, 80, 24, 500), "init");
    feed(&t, "ABC");
    ghostcon_term_feed(&t, (const uint8_t *)"\x1b" "7", 2);       /* DECSC save */
    feed(&t, "\x1b[10;20H");
    feed(&t, "XY");
    ASSERT_CELL(t, 19, 9, 'X', "moved and wrote");
    ghostcon_term_feed(&t, (const uint8_t *)"\x1b" "8", 2);       /* DECRC restore */
    ASSERT_CURSOR(t, 3, 0, "restored cursor");
    ghostcon_term_deinit(&t);
}

TEST(cursor_next_prev_line) {
    ghostcon_term_t t;
    ASSERT(ghostcon_term_init(&t, 80, 24, 500), "init");
    feed(&t, "\x1b[5;20H");
    ghostcon_term_feed(&t, (const uint8_t *)"\x1b" "E", 2);       /* NEL — next line */
    ASSERT_CURSOR(t, 0, 5, "next line (NEL)");
    feed(&t, "\x1bM");       /* RI — reverse index */
    ASSERT_CURSOR(t, 0, 4, "reverse index (RI)");
    ghostcon_term_deinit(&t);
}

TEST(cursor_horizontal_abs) {
    ghostcon_term_t t;
    ASSERT(ghostcon_term_init(&t, 80, 24, 500), "init");
    feed(&t, "\x1b[5;30HAAAAA");
    feed(&t, "\x1b[G");      /* HPA default = col 1 */
    ASSERT_CURSOR(t, 0, 4, "HPA default");
    feed(&t, "\x1b[10G");    /* HPA col 10 */
    ASSERT_CURSOR(t, 9, 4, "HPA col 10");
    ghostcon_term_deinit(&t);
}

/* ================================================================== */
/* Erase                                                               */
/* ================================================================== */

TEST(erase_display_from_cursor) {
    ghostcon_term_t t;
    ASSERT(ghostcon_term_init(&t, 80, 24, 500), "init");
    /* Write content then 0J from a specific position */
    feed(&t, "AAAAA");
    feed(&t, "\x1b[2;1H");    /* cursor to row 2 col 1 (0-based: 0,1) */
    feed(&t, "BBBBB");
    feed(&t, "\x1b[3;1H");    /* cursor to row 3 col 1 (0-based: 0,2) */
    feed(&t, "CCCCC");
    /* Now: row 0 = AAAAA, row 1 = BBBBB, row 2 = CCCCC, cursor at (5,2) */
    feed(&t, "\x1b[2;1H");    /* cursor to (0,1) */
    feed(&t, "\x1b[J");       /* erase 0J (cursor to end) */
    ASSERT_CELL(t, 0, 0, 'A', "row 0 preserved");
    ASSERT_CELL(t, 0, 1, 0,  "row 1 erased (cursor row)");
    ASSERT_CELL(t, 0, 2, 0,  "row 2 erased");
    ghostcon_term_deinit(&t);
}

TEST(erase_display_to_cursor) {
    ghostcon_term_t t;
    ASSERT(ghostcon_term_init(&t, 80, 24, 500), "init");
    feed(&t, "AAAAA");
    feed(&t, "\x1b[2;1H");    /* cursor to (0,1) */
    feed(&t, "BBBBB");
    feed(&t, "\x1b[3;1H");    /* cursor to (0,2) */
    feed(&t, "CCCCC");
    /* Now: row 0 = AAAAA, row 1 = BBBBB, row 2 = CCCCC, cursor at (5,2) */
    feed(&t, "\x1b[3;1H");    /* cursor to (0,2) */
    feed(&t, "\x1b[1J");      /* erase 1J (start to cursor) */
    ASSERT_CELL(t, 0, 0, 0,  "row 0 erased");
    ASSERT_CELL(t, 0, 1, 0,  "row 1 erased");
    ASSERT_CELL(t, 0, 2, 0,  "row 2 erased (cursor row)");
    ghostcon_term_deinit(&t);
}

TEST(erase_display_all) {
    ghostcon_term_t t;
    ASSERT(ghostcon_term_init(&t, 80, 24, 500), "init");
    feed(&t, "AAAAABBBBBCCCCC");
    feed(&t, "\x1b[2J");      /* erase 2J (entire display) */
    for (int y = 0; y < 3; y++)
        ASSERT_CELL(t, 0, y, 0, "row should be empty");
    ghostcon_term_deinit(&t);
}

TEST(erase_display_saved) {
    ghostcon_term_t t;
    ASSERT(ghostcon_term_init(&t, 80, 24, 500), "init");
    feed(&t, "AAAAABBBBBCCCCC");
    feed(&t, "\x1b[3J");      /* erase 3J (saved lines) */
    ASSERT_CELL(t, 0, 0, 'A', "display content preserved");
    ghostcon_term_deinit(&t);
}

TEST(erase_line_from_cursor) {
    ghostcon_term_t t;
    ASSERT(ghostcon_term_init(&t, 80, 24, 500), "init");
    feed(&t, "AAAAABBBBB");
    feed(&t, "\x1b[6G");      /* HPA col 6 (0-based: 5) */
    feed(&t, "\x1b[K");       /* erase 0K (cursor to end) */
    ASSERT_CELL(t, 0, 0, 'A', "col 0 preserved");
    ASSERT_CELL(t, 4, 0, 'A', "col 4 preserved (before cursor)");
    ASSERT_CELL(t, 5, 0, 0,  "col 5 erased (cursor position)");
    ASSERT_CELL(t, 9, 0, 0,  "col 9 erased");
    ghostcon_term_deinit(&t);
}

TEST(erase_line_to_cursor) {
    ghostcon_term_t t;
    ASSERT(ghostcon_term_init(&t, 80, 24, 500), "init");
    feed(&t, "AAAAABBBBB");
    feed(&t, "\x1b[5G");      /* HPA col 5 */
    feed(&t, "\x1b[1K");      /* erase 1K (start to cursor) */
    ASSERT_CELL(t, 0, 0, 0,  "start erased");
    ASSERT_CELL(t, 4, 0, 0,  "before cursor erased");
    ASSERT_CELL(t, 5, 0, 'B', "at cursor preserved");
    ghostcon_term_deinit(&t);
}

TEST(erase_line_all) {
    ghostcon_term_t t;
    ASSERT(ghostcon_term_init(&t, 80, 24, 500), "init");
    feed(&t, "AAAAABBBBB");
    feed(&t, "\x1b[2K");      /* erase 2K (entire line) */
    for (int x = 0; x < 10; x++)
        ASSERT_CELL(t, x, 0, 0, "cell should be empty");
    ghostcon_term_deinit(&t);
}

/* ================================================================== */
/* Line wrapping                                                       */
/* ================================================================== */

TEST(wrapping_basic) {
    ghostcon_term_t t;
    ASSERT(ghostcon_term_init(&t, 80, 24, 500), "init");
    char buf[82];
    memset(buf, 'A', 81);
    buf[81] = '\0';
    ghostcon_term_feed(&t, (const uint8_t *)buf, 81);
    ASSERT_CELL(t, 79, 0, 'A',  "col 79 on first line");
    ASSERT_CELL(t, 0, 1, 'A',   "col 0 on second line (wrapped)");
    ASSERT_CURSOR(t, 1, 1, "cursor after wrap");
    ghostcon_term_deinit(&t);
}

TEST(wrapping_disabled) {
    ghostcon_term_t t;
    ASSERT(ghostcon_term_init(&t, 80, 24, 500), "init");
    feed(&t, "\x1b[?7l");      /* reset auto-wrap (DECAWM) */
    char buf[82];
    memset(buf, 'A', 81);
    buf[81] = '\0';
    ghostcon_term_feed(&t, (const uint8_t *)buf, 81);
    ASSERT_CELL(t, 0, 1, 0, "second line should be empty (no-wrap)");
    ASSERT_CURSOR(t, 79, 0, "cursor stays at right margin");
    ghostcon_term_deinit(&t);
}

TEST(wrapping_reenable) {
    ghostcon_term_t t;
    ASSERT(ghostcon_term_init(&t, 80, 24, 500), "init");
    feed(&t, "\x1b[?7l");      /* disable wrap */
    char buf[82];
    memset(buf, 'A', 81);
    buf[81] = '\0';
    ghostcon_term_feed(&t, (const uint8_t *)buf, 50);            /* write part */
    feed(&t, "\x1b[?7h");      /* enable wrap */
    ghostcon_term_feed(&t, (const uint8_t *)(buf+50), 31);       /* write rest */
    ASSERT_CURSOR(t, 1, 1, "wrapping re-enabled");
    ghostcon_term_deinit(&t);
}

TEST(wrap_flag_scroll_clears) {
    ghostcon_term_t t;
    ASSERT(ghostcon_term_init(&t, 80, 24, 500), "init");
    feed(&t, "\x1b[24;1H");     /* bottom row */
    char buf[82];
    memset(buf, 'A', 81);
    buf[81] = '\0';
    ghostcon_term_feed(&t, (const uint8_t *)buf, 81);  /* fills + wraps, scrolls */
    /* the wrapped row scrolls off; the fresh bottom row must not be wrapped */
    ASSERT(!t.screen.rows[23].wrap, "scrolled blank row not wrapped");
    ASSERT(t.screen.rows[22].wrap, "wrapped row (now 22) marked");
    ghostcon_term_deinit(&t);
}

TEST(el_right_resets_wrap) {
    ghostcon_term_t t;
    ASSERT(ghostcon_term_init(&t, 80, 24, 500), "init");
    char buf[82];
    memset(buf, 'A', 80);
    buf[80] = 'B';               /* 81 chars: row 0 wraps to row 1 */
    buf[81] = '\0';
    ghostcon_term_feed(&t, (const uint8_t *)buf, 81);
    ASSERT(t.screen.rows[0].wrap, "row 0 wrapped");
    feed(&t, "\x1b[1;1H\x1b[0K"); /* EL right at row 0 col 0 */
    ASSERT(!t.screen.rows[0].wrap, "EL-right clears row wrap (Ghostty)");
    ghostcon_term_deinit(&t);
}

TEST(ed_complete_clears_wrap) {
    ghostcon_term_t t;
    ASSERT(ghostcon_term_init(&t, 80, 24, 500), "init");
    char buf[82];
    memset(buf, 'A', 80);
    buf[80] = 'B';
    buf[81] = '\0';
    ghostcon_term_feed(&t, (const uint8_t *)buf, 81);
    ASSERT(t.screen.rows[0].wrap, "row 0 wrapped");
    feed(&t, "\x1b[2J");         /* ED complete */
    ASSERT(!t.screen.rows[0].wrap, "ED complete clears wrap");
    ghostcon_term_deinit(&t);
}

/* ================================================================== */
/* Scroll regions                                                      */
/* ================================================================== */

TEST(scroll_region_basic) {
    ghostcon_term_t t;
    ASSERT(ghostcon_term_init(&t, 80, 24, 500), "init");
    feed(&t, "\x1b[5;15r");    /* scroll region rows 5-15 (1-based) = 4-14 (0-based) */
    ASSERT_CURSOR(t, 0, 0, "DECSTBM moves cursor to home");
    /* Marker text above the region must be untouched by scrolling */
    feed(&t, "\x1b[4;1H");     /* row 4 (1-based) = row 3 (0-based), above region */
    feed(&t, "ABOVE");
    /* Put content at the bottom of the region */
    feed(&t, "\x1b[15;1H");    /* bottom of region */
    feed(&t, "X");
    feed(&t, "\r\n");          /* LF at region bottom → scroll region up */
    feed(&t, "Y");             /* new content at bottom after scroll */
    ASSERT_CELL(t, 0, 3, 'A', "content above region untouched");
    ASSERT_CELL(t, 0, 13, 'X', "row above bottom scrolled up");
    ASSERT_CELL(t, 0, 14, 'Y', "new content at bottom");
    ghostcon_term_deinit(&t);
}

TEST(scroll_region_reset) {
    ghostcon_term_t t;
    ASSERT(ghostcon_term_init(&t, 80, 24, 500), "init");
    feed(&t, "\x1b[5;15r");
    feed(&t, "\x1b[r");       /* reset scroll region */
    /* Now scrolling should use full screen — use \r\n */
    for (int i = 0; i < 30; i++)
        feed(&t, "\r\n");
    ASSERT_CURSOR(t, 0, 23, "full screen scroll");
    ghostcon_term_deinit(&t);
}

/* ================================================================== */
/* Alt screen                                                          */
/* ================================================================== */

TEST(alt_screen_basic) {
    ghostcon_term_t t;
    ASSERT(ghostcon_term_init(&t, 80, 24, 500), "init");
    feed(&t, "NormalScreen");
    feed(&t, "\x1b[?1049h");  /* alt screen */
    ASSERT_CELL(t, 0, 0, 0, "alt screen should be empty");
    feed(&t, "AltScreen");
    ASSERT_CELL(t, 0, 0, 'A', "alt screen text");
    feed(&t, "\x1b[?1049l");  /* back to normal */
    ASSERT_CELL(t, 0, 0, 'N', "normal screen restored");
    ASSERT_CURSOR(t, 12, 0, "cursor restored to saved position");
    ghostcon_term_deinit(&t);
}

/* ================================================================== */
/* Tab stops                                                           */
/* ================================================================== */

TEST(tab_stops_basic) {
    ghostcon_term_t t;
    ASSERT(ghostcon_term_init(&t, 80, 24, 500), "init");
    feed(&t, "A\tB");
    ASSERT_CELL(t, 0, 0, 'A', "before tab");
    ASSERT_CELL(t, 8, 0, 'B', "after tab (col 8)");
    ASSERT_CURSOR(t, 9, 0, "cursor after tab");
    ghostcon_term_deinit(&t);
}

TEST(tab_stops_clear_set) {
    ghostcon_term_t t;
    ASSERT(ghostcon_term_init(&t, 80, 24, 500), "init");
    feed(&t, "\x1b[3g");       /* clear all tabs (3g) */
    feed(&t, "A\tB");
    ASSERT_CELL(t, 79, 0, 'B', "tab with no stops moves to right margin");
    ghostcon_term_deinit(&t);
}

/* ================================================================== */
/* SGR attributes                                                      */
/* ================================================================== */

TEST(sgr_bold) {
    ghostcon_term_t t;
    ASSERT(ghostcon_term_init(&t, 80, 24, 500), "init");
    feed(&t, "\x1b[1mBold\x1b[m");
    ASSERT_STYLE_FLAG(t, 0, 0, GC_STYLE_BOLD, "bold");
    ASSERT_CELL(t, 0, 0, 'B', "bold char");
    ghostcon_term_deinit(&t);
}

TEST(sgr_italic) {
    ghostcon_term_t t;
    ASSERT(ghostcon_term_init(&t, 80, 24, 500), "init");
    feed(&t, "\x1b[3mItalic\x1b[23m");
    ASSERT_STYLE_FLAG(t, 0, 0, GC_STYLE_ITALIC, "italic");
    /* After reset, cursor style should not have italic */
    ghostcon_style_id_t sid = t.screen.cursor.style_id;
    const ghostcon_style_t *s = ghostcon_style_set_get(t.screen.styles, sid);
    ASSERT(!(s->flags & GC_STYLE_ITALIC), "italic reset after CSI 23 m");
    ghostcon_term_deinit(&t);
}

TEST(sgr_underline) {
    ghostcon_term_t t;
    ASSERT(ghostcon_term_init(&t, 80, 24, 500), "init");
    feed(&t, "\x1b[4mUnder\x1b[24m");
    ASSERT_STYLE_FLAG(t, 0, 0, GC_STYLE_UNDERLINE, "underline");
    ghostcon_term_deinit(&t);
}

TEST(sgr_reverse) {
    ghostcon_term_t t;
    ASSERT(ghostcon_term_init(&t, 80, 24, 500), "init");
    feed(&t, "\x1b[7mInv\x1b[27m");
    ASSERT_STYLE_FLAG(t, 0, 0, GC_STYLE_INVERSE, "inverse");
    ghostcon_term_deinit(&t);
}

TEST(sgr_strikethrough) {
    ghostcon_term_t t;
    ASSERT(ghostcon_term_init(&t, 80, 24, 500), "init");
    feed(&t, "\x1b[9mStrike\x1b[29m");
    ASSERT_STYLE_FLAG(t, 0, 0, GC_STYLE_STRIKETHROUGH, "strikethrough");
    ghostcon_term_deinit(&t);
}

TEST(sgr_reset_all) {
    ghostcon_term_t t;
    ASSERT(ghostcon_term_init(&t, 80, 24, 500), "init");
    feed(&t, "\x1b[1;3;4;7mTest");
    ASSERT_STYLE_FLAG(t, 0, 0, GC_STYLE_BOLD, "bold before reset");
    feed(&t, "\x1b[mMore");    /* SGR 0 = reset all */
    /* Check cursor style after reset — should have no flags */
    ghostcon_style_id_t sid = t.screen.cursor.style_id;
    if (sid != GC_STYLE_DEFAULT_ID) {
        const ghostcon_style_t *s = ghostcon_style_set_get(t.screen.styles, sid);
        if (s->flags != 0) FAIL("flags not cleared after SGR 0");
    }
    ghostcon_term_deinit(&t);
}

TEST(sgr_256_colors) {
    ghostcon_term_t t;
    ASSERT(ghostcon_term_init(&t, 80, 24, 500), "init");
    feed(&t, "\x1b[38;5;196mRed"); /* 256-color fg = 196 (red) */
    /* 256-color should NOT set the truecolor flag */
    ghostcon_style_id_t sid = t.screen.cursor.style_id;
    const ghostcon_style_t *s = ghostcon_style_set_get(t.screen.styles, sid);
    ASSERT(!(s->flags & GC_STYLE_FG_TRUECOLOR), "256-color should not set truecolor flag");
    ghostcon_term_deinit(&t);
}

TEST(sgr_truecolor) {
    ghostcon_term_t t;
    ASSERT(ghostcon_term_init(&t, 80, 24, 500), "init");
    feed(&t, "\x1b[38;2;255;128;64mRGB");
    ASSERT_STYLE_FLAG(t, 0, 0, GC_STYLE_FG_TRUECOLOR, "truecolor flag");
    ghostcon_term_deinit(&t);
}

/* ================================================================== */
/* OSC sequences                                                       */
/* ================================================================== */

TEST(osc_window_title) {
    ghostcon_term_t t;
    ASSERT(ghostcon_term_init(&t, 80, 24, 500), "init");
    /* OSC 0 = set window title (and icon name) */
    feed(&t, "\x1b]0;test title\x07");
    /* Should parse without crashing — title stored in screen state */
    ghostcon_term_deinit(&t);
}

TEST(osc_bel_terminated) {
    ghostcon_term_t t;
    ASSERT(ghostcon_term_init(&t, 80, 24, 500), "init");
    feed(&t, "\x1b]2;mytitle\x07"); /* OSC 2 with BEL */
    feed(&t, "Hello");               /* should print after */
    ASSERT_CELL(t, 0, 0, 'H', "print after OSC BEL");
    ghostcon_term_deinit(&t);
}

TEST(osc_st_terminated) {
    ghostcon_term_t t;
    ASSERT(ghostcon_term_init(&t, 80, 24, 500), "init");
    /* OSC 2 terminated by ST (ESC \) */
    feed(&t, "\x1b]2;mytitle\x1b\\");
    feed(&t, "Hi");
    ASSERT_CELL(t, 0, 0, 'H', "print after OSC ST");
    ghostcon_term_deinit(&t);
}

TEST(osc_invalid_ignored) {
    ghostcon_term_t t;
    ASSERT(ghostcon_term_init(&t, 80, 24, 500), "init");
    /* Random bytes in OSC — should not crash */
    feed(&t, "\x1b]9999;some_garbage_data\x07Hello");
    ASSERT_CELL(t, 0, 0, 'H', "print after invalid OSC");
    ghostcon_term_deinit(&t);
}

/* ================================================================== */
/* Modes                                                               */
/* ================================================================== */

TEST(dec_private_modes) {
    ghostcon_term_t t;
    ASSERT(ghostcon_term_init(&t, 80, 24, 500), "init");
    /* Save cursor, set DEC origin mode, move cursor — should affect coordinate system */
    feed(&t, "\x1b[?6h");      /* DECOM set */
    feed(&t, "\x1b[5;10H");    /* cursor to row 5 col 10 */
    /* With DECOM set, cursor position is relative to scroll region */
    ghostcon_term_deinit(&t);
}

TEST(dec_private_modes_reset) {
    ghostcon_term_t t;
    ASSERT(ghostcon_term_init(&t, 80, 24, 500), "init");
    feed(&t, "\x1b[?6h");      /* DECOM set */
    feed(&t, "\x1b[?6l");      /* DECOM reset */
    ghostcon_term_deinit(&t);
}

TEST(ansi_mode_irm) {
    ghostcon_term_t t;
    ASSERT(ghostcon_term_init(&t, 80, 24, 500), "init");
    feed(&t, "ABC");
    ASSERT_CURSOR(t, 3, 0, "cursor after ABC");
    feed(&t, "\x1b[4h");       /* IRM set (insert mode) */
    feed(&t, "XY");
    ASSERT_CURSOR(t, 5, 0, "cursor in insert mode");
    ghostcon_term_deinit(&t);
}

/* ================================================================== */
/* Synchronized output                                                 */
/* ================================================================== */

TEST(synchronized_output) {
    ghostcon_term_t t;
    ASSERT(ghostcon_term_init(&t, 80, 24, 500), "init");
    feed(&t, "\x1b[?2026h");   /* begin sync */
    ASSERT(t.screen.synchronized_output, "sync active");
    feed(&t, "SyncText");
    feed(&t, "\x1b[?2026l");   /* end sync */
    ASSERT(!t.screen.synchronized_output, "sync inactive");
    ghostcon_term_deinit(&t);
}

/* ================================================================== */
/* DCS passthrough                                                     */
/* ================================================================== */

TEST(dcs_passthrough) {
    ghostcon_term_t t;
    ASSERT(ghostcon_term_init(&t, 80, 24, 500), "init");
    feed(&t, "\x1bP0;1;2q\x1b\\Hello");
    /* DCS data should be consumed, following text should print */
    ASSERT_CELL(t, 0, 0, 'H', "text after DCS");
    ghostcon_term_deinit(&t);
}

/* ================================================================== */
/* Scrollback                                                          */
/* ================================================================== */

TEST(scrollback_basic) {
    ghostcon_term_t t;
    ASSERT(ghostcon_term_init(&t, 80, 24, 500), "init");
    /* Write more than visible rows — use \r\n */
    for (int i = 0; i < 30; i++) {
        uint8_t buf[32];
        int n = snprintf((char *)buf, sizeof(buf), "Line%d\r\n", i);
        ghostcon_term_feed(&t, buf, n);
    }
    /* First line should have scrolled into scrollback */
    ASSERT(t.screen.history_count > 0, "scrollback has data");
    ghostcon_term_deinit(&t);
}

TEST(scrollback_size) {
    ghostcon_term_t t;
    ASSERT(ghostcon_term_init(&t, 80, 24, 100), "init small sb");
    for (int i = 0; i < 150; i++) {
        uint8_t buf[32];
        int n = snprintf((char *)buf, sizeof(buf), "Line%d\r\n", i);
        ghostcon_term_feed(&t, buf, n);
    }
    ASSERT(t.screen.history_count <= 100, "scrollback capped");
    ASSERT(t.screen.history_count > 0, "scrollback has data");
    ghostcon_term_deinit(&t);
}

/* ================================================================== */
/* Edge cases                                                          */
/* ================================================================== */

TEST(empty_input) {
    ghostcon_term_t t;
    ASSERT(ghostcon_term_init(&t, 80, 24, 500), "init");
    feed(&t, "");
    ASSERT_CURSOR(t, 0, 0, "no movement");
    ghostcon_term_deinit(&t);
}

TEST(null_bytes) {
    ghostcon_term_t t;
    ASSERT(ghostcon_term_init(&t, 80, 24, 500), "init");
    ghostcon_term_feed(&t, (const uint8_t *)"\x00\x00Hello\x00\x00", 9);
    ASSERT_CELL(t, 0, 0, 'H', "null bytes skipped");
    ghostcon_term_deinit(&t);
}

TEST(ctrl_characters) {
    ghostcon_term_t t;
    ASSERT(ghostcon_term_init(&t, 80, 24, 500), "init");
    /* Control characters below 0x20 should not produce visible output */
    ghostcon_term_feed(&t, (const uint8_t *)"\x01\x02\x03\x04\x05\x06\x0E\x0F\x10\x11\x12\x13\x14\x15\x16\x17\x18\x19\x1A\x1C\x1D\x1E\x1FX", 24);
    ASSERT_CELL(t, 0, 0, 'X', "controls ignored, last char is X");
    ghostcon_term_deinit(&t);
}

TEST(invalid_escape_discard) {
    ghostcon_term_t t;
    ASSERT(ghostcon_term_init(&t, 80, 24, 500), "init");
    /* ESC followed by invalid final byte. feed() can't be used because of \0 in string. */
    ghostcon_term_feed(&t, (const uint8_t *)"\x1b\x1f\x1b\x00\x1b\x7fHello", 12);
    ASSERT_CELL(t, 0, 0, 'H', "invalid ESC consumed");
    ghostcon_term_deinit(&t);
}

TEST(csi_missing_params) {
    ghostcon_term_t t;
    ASSERT(ghostcon_term_init(&t, 80, 24, 500), "init");
    /* CSI with empty params */
    feed(&t, "\x1b[H");        /* home */
    ASSERT_CURSOR(t, 0, 0, "home works");
    ghostcon_term_deinit(&t);
}

TEST(long_input_stream) {
    ghostcon_term_t t;
    ASSERT(ghostcon_term_init(&t, 80, 24, 500), "init");
    /* Stream of 10KB — should not crash */
    uint8_t buf[10240];
    for (size_t i = 0; i < sizeof(buf); i++)
        buf[i] = (uint8_t)(' ' + (i % 95));
    ghostcon_term_feed(&t, buf, sizeof(buf));
    ASSERT(t.screen.history_count > 0, "scrollback after long input");
    ghostcon_term_deinit(&t);
}

TEST(repeated_resize) {
    ghostcon_term_t t;
    ASSERT(ghostcon_term_init(&t, 80, 24, 500), "init");
    for (int i = 0; i < 50; i++) {
        int w = 40 + (i % 40);
        int h = 10 + (i % 15);
        ASSERT(ghostcon_term_resize(&t, w, h), "resize");
    }
    ghostcon_term_deinit(&t);
}

/* ================================================================== */
/* Insert/Delete/Erase chars                                           */
/* ================================================================== */

TEST(insert_chars) {
    ghostcon_term_t t;
    ASSERT(ghostcon_term_init(&t, 80, 24, 500), "init");
    feed(&t, "ABCDE");         /* cursor at (5,0) */
    feed(&t, "\x1b[2G");       /* HPA col 2 -> cursor (1,0) */
    feed(&t, "\x1b[2@");       /* ICH insert 2 blanks at col 1 */
    ASSERT_CELL(t, 0, 0, 'A', "col 0 unchanged");
    ASSERT_CELL(t, 1, 0, 0,  "col 1 blank (inserted)");
    ASSERT_CELL(t, 2, 0, 0,  "col 2 blank (inserted)");
    ASSERT_CELL(t, 3, 0, 'B', "col 3 shifted");
    ASSERT_CELL(t, 4, 0, 'C', "col 4 shifted");
    ASSERT_CELL(t, 5, 0, 'D', "col 5 shifted");
    ASSERT_CELL(t, 6, 0, 'E', "col 6 shifted");
    ASSERT_CURSOR(t, 1, 0, "cursor unmoved by ICH");
    ghostcon_term_deinit(&t);
}

TEST(insert_chars_zero_clamp) {
    ghostcon_term_t t;
    ASSERT(ghostcon_term_init(&t, 80, 24, 500), "init");
    feed(&t, "ABC");
    feed(&t, "\x1b[1G");       /* cursor (0,0) */
    feed(&t, "\x1b[0@");       /* ICH explicit 0 clamps to 1 (Ghostty) */
    ASSERT_CELL(t, 0, 0, 0,  "col 0 blank (inserted)");
    ASSERT_CELL(t, 1, 0, 'A', "col 1 shifted");
    ASSERT_CELL(t, 2, 0, 'B', "col 2 shifted");
    ghostcon_term_deinit(&t);
}

TEST(delete_chars) {
    ghostcon_term_t t;
    ASSERT(ghostcon_term_init(&t, 80, 24, 500), "init");
    feed(&t, "ABCDE");
    feed(&t, "\x1b[2G");       /* cursor (1,0) */
    feed(&t, "\x1b[2P");       /* DCH delete 2 at col 1 */
    ASSERT_CELL(t, 0, 0, 'A', "col 0 unchanged");
    ASSERT_CELL(t, 1, 0, 'D', "col 1 = D");
    ASSERT_CELL(t, 2, 0, 'E', "col 2 = E");
    ASSERT_CURSOR(t, 1, 0, "cursor unmoved by DCH");
    ghostcon_term_deinit(&t);
}

TEST(erase_chars) {
    ghostcon_term_t t;
    ASSERT(ghostcon_term_init(&t, 80, 24, 500), "init");
    feed(&t, "ABCDE");
    feed(&t, "\x1b[2G");       /* cursor (1,0) */
    feed(&t, "\x1b[2X");       /* ECH erase 2 at col 1 */
    ASSERT_CELL(t, 0, 0, 'A', "col 0 unchanged");
    ASSERT_CELL(t, 1, 0, 0,  "col 1 erased");
    ASSERT_CELL(t, 2, 0, 0,  "col 2 erased");
    ASSERT_CELL(t, 3, 0, 'D', "col 3 preserved");
    ASSERT_CELL(t, 4, 0, 'E', "col 4 preserved");
    ghostcon_term_deinit(&t);
}

/* ================================================================== */
/* Insert/Delete lines                                                 */
/* ================================================================== */

TEST(insert_lines) {
    ghostcon_term_t t;
    ASSERT(ghostcon_term_init(&t, 80, 24, 500), "init");
    feed(&t, "AA"); feed(&t, "\r\n");
    feed(&t, "BB"); feed(&t, "\r\n");
    feed(&t, "CC");            /* rows 0-2 = AA/BB/CC */
    feed(&t, "\x1b[3;1H");     /* cursor (0,2) */
    feed(&t, "\x1b[2L");       /* IL insert 2 lines at row 2 */
    ASSERT_CELL(t, 0, 0, 'A', "row 0 untouched (above cursor)");
    ASSERT_CELL(t, 0, 1, 'B', "row 1 untouched (above cursor)");
    ASSERT_CELL(t, 0, 2, 0,  "row 2 cleared (inserted)");
    ASSERT_CELL(t, 0, 3, 0,  "row 3 cleared (inserted)");
    ASSERT_CELL(t, 0, 4, 'C', "old row 2 pushed down to row 4");
    ghostcon_term_deinit(&t);
}

TEST(delete_lines) {
    ghostcon_term_t t;
    ASSERT(ghostcon_term_init(&t, 80, 24, 500), "init");
    feed(&t, "\x1b[3;1H"); feed(&t, "CC");
    feed(&t, "\x1b[4;1H"); feed(&t, "DD");
    feed(&t, "\x1b[5;1H"); feed(&t, "EE");
    feed(&t, "\x1b[3;1H");     /* cursor (0,2) */
    feed(&t, "\x1b[1M");       /* DL delete 1 line at row 2 */
    ASSERT_CELL(t, 0, 2, 'D', "row 2 = D (pulled up)");
    ASSERT_CELL(t, 0, 3, 'E', "row 3 = E (pulled up)");
    ASSERT_CELL(t, 0, 4, 0,  "row 4 cleared");
    ghostcon_term_deinit(&t);
}

/* ================================================================== */
/* Scroll up/down                                                      */
/* ================================================================== */

TEST(scroll_up) {
    ghostcon_term_t t;
    ASSERT(ghostcon_term_init(&t, 80, 24, 500), "init");
    feed(&t, "AAAAA"); feed(&t, "\r\n");
    feed(&t, "BBBBB"); feed(&t, "\r\n");
    feed(&t, "CCCCC"); feed(&t, "\r\n");
    feed(&t, "DDDDD");         /* rows 0-3 filled */
    feed(&t, "\x1b[2S");       /* SU 2 */
    ASSERT_CELL(t, 0, 0, 'C', "row 0 = C (scrolled up 2)");
    ASSERT_CELL(t, 0, 1, 'D', "row 1 = D (scrolled up 2)");
    ASSERT_CELL(t, 0, 2, 0,  "row 2 cleared");
    ASSERT(t.screen.history_count > 0, "scrolled lines pushed to history");
    ghostcon_term_deinit(&t);
}

TEST(scroll_down) {
    ghostcon_term_t t;
    ASSERT(ghostcon_term_init(&t, 80, 24, 500), "init");
    feed(&t, "AAAAA"); feed(&t, "\r\n");
    feed(&t, "BBBBB");
    feed(&t, "\x1b[2T");       /* SD 2 */
    ASSERT_CELL(t, 0, 0, 0,  "row 0 cleared");
    ASSERT_CELL(t, 0, 1, 0,  "row 1 cleared");
    ASSERT_CELL(t, 0, 2, 'A', "row 2 = A (scrolled down 2)");
    ASSERT_CELL(t, 0, 3, 'B', "row 3 = B (scrolled down 2)");
    ghostcon_term_deinit(&t);
}

/* ================================================================== */
/* SCOSC / SCORC (CSI s / CSI u)                                       */
/* ================================================================== */

TEST(save_restore_cursor_csi) {
    ghostcon_term_t t;
    ASSERT(ghostcon_term_init(&t, 80, 24, 500), "init");
    feed(&t, "ABC");
    feed(&t, "\x1b[s");        /* SCOSC save cursor at (3,0) */
    feed(&t, "\x1b[10;20H");
    ASSERT_CURSOR(t, 19, 9, "moved away");
    feed(&t, "\x1b[u");        /* SCORC restore */
    ASSERT_CURSOR(t, 3, 0, "restored cursor");
    ghostcon_term_deinit(&t);
}

/* ================================================================== */
/* DECSLRM (CSI s with params) + DECLRMM (mode 69)                     */
/* ================================================================== */

TEST(set_left_right_margin) {
    ghostcon_term_t t;
    ASSERT(ghostcon_term_init(&t, 80, 24, 500), "init");
    feed(&t, "\x1b[?69h");     /* DECLRMM on */
    feed(&t, "\x1b[3;7s");     /* DECSLRM left=2, right=6 */
    ASSERT_EQ_U(t.screen.margin_region.left, 2, "left margin");
    ASSERT_EQ_U(t.screen.margin_region.right, 6, "right margin");
    ghostcon_term_deinit(&t);
}

TEST(set_left_right_margin_requires_mode) {
    ghostcon_term_t t;
    ASSERT(ghostcon_term_init(&t, 80, 24, 500), "init");
    feed(&t, "\x1b[3;7s");     /* no DECLRMM -> DECSLRM ignored */
    ASSERT_EQ_U(t.screen.margin_region.left, 0, "left margin default");
    ASSERT_EQ_U(t.screen.margin_region.right, 79, "right margin default");
    ghostcon_term_deinit(&t);
}

/* ================================================================== */
/* CSI intermediate discrimination                                     */
/* ================================================================== */

TEST(decsca) {
    ghostcon_term_t t;
    ASSERT(ghostcon_term_init(&t, 80, 24, 500), "init");
    feed(&t, "\x1b[1\"q");     /* DECSCA protect on */
    ASSERT(t.screen.cursor.protected, "cursor protected on");
    feed(&t, "AB");
    ASSERT_PROTECTED(t, 0, 0, true, "cell 0 protected");
    ASSERT_PROTECTED(t, 1, 0, true, "cell 1 protected");
    feed(&t, "\x1b[0\"q");     /* DECSCA protect off */
    ASSERT(!t.screen.cursor.protected, "cursor protected off");
    feed(&t, "C");
    ASSERT_PROTECTED(t, 2, 0, false, "cell 2 not protected");
    ghostcon_term_deinit(&t);
}

TEST(decscusr) {
    ghostcon_term_t t;
    ASSERT(ghostcon_term_init(&t, 80, 24, 500), "init");
    feed(&t, "\x1b[ q");       /* DECSCUSR default (no param) */
    ASSERT_EQ_U(t.screen.cursor.cursor_style, GC_CURSOR_BLOCK_BLINK, "default style");
    feed(&t, "\x1b[2 q");      /* steady block */
    ASSERT_EQ_U(t.screen.cursor.cursor_style, GC_CURSOR_BLOCK, "steady block");
    feed(&t, "\x1b[3 q");      /* blinking underline */
    ASSERT_EQ_U(t.screen.cursor.cursor_style, GC_CURSOR_UNDERLINE_BLINK, "blink underline");
    feed(&t, "\x1b[5 q");      /* blinking bar */
    ASSERT_EQ_U(t.screen.cursor.cursor_style, GC_CURSOR_BAR_BLINK, "blink bar");
    feed(&t, "\x1b[?0 q");     /* invalid (two intermediates) — ignored */
    ASSERT_EQ_U(t.screen.cursor.cursor_style, GC_CURSOR_BAR_BLINK, "invalid ignored");
    ghostcon_term_deinit(&t);
}

TEST(decscusr_without_space) {
    ghostcon_term_t t;
    ASSERT(ghostcon_term_init(&t, 80, 24, 500), "init");
    feed(&t, "\x1b[5 q");      /* establish a style */
    feed(&t, "\x1b[q");        /* no SP intermediate — ignored */
    ASSERT_EQ_U(t.screen.cursor.cursor_style, GC_CURSOR_BAR_BLINK, "CSI q ignored");
    feed(&t, "\x1b[1q");       /* no SP intermediate — ignored */
    ASSERT_EQ_U(t.screen.cursor.cursor_style, GC_CURSOR_BAR_BLINK, "CSI 1 q ignored");
    ghostcon_term_deinit(&t);
}

TEST(xtshiftescape) {
    ghostcon_term_t t;
    ASSERT(ghostcon_term_init(&t, 80, 24, 500), "init");
    feed(&t, "\x1b[>1s");      /* XTSHIFTESCAPE on */
    ASSERT(t.screen.mouse_shift_capture, "shift capture on");
    feed(&t, "\x1b[>s");       /* 0 params -> off */
    ASSERT(!t.screen.mouse_shift_capture, "shift capture off (no params)");
    feed(&t, "\x1b[>1s");
    ASSERT(t.screen.mouse_shift_capture, "shift capture on again");
    feed(&t, "\x1b[>0s");      /* off */
    ASSERT(!t.screen.mouse_shift_capture, "shift capture off");
    feed(&t, "\x1b[>2s");      /* invalid param — ignored */
    ASSERT(!t.screen.mouse_shift_capture, "shift capture unchanged on invalid");
    ghostcon_term_deinit(&t);
}

TEST(decsel) {
    ghostcon_term_t t;
    ASSERT(ghostcon_term_init(&t, 80, 24, 500), "init");
    feed(&t, "\x1b[1\"q");     /* DECSCA protect on */
    feed(&t, "ABC");
    feed(&t, "\x1b[0\"q");     /* protect off */
    feed(&t, "XYZ");
    feed(&t, "\x1b[0G");       /* to col 0 */
    feed(&t, "\x1b[?0K");      /* DECSEL erase to right */
    ASSERT_CELL(t, 0, 0, 'A', "protected survives");
    ASSERT_CELL(t, 1, 0, 'B', "protected survives");
    ASSERT_CELL(t, 2, 0, 'C', "protected survives");
    ASSERT_CELL(t, 3, 0, 0,   "unprotected erased");
    ASSERT_CELL(t, 5, 0, 0,   "unprotected erased");
    ghostcon_term_deinit(&t);
}

TEST(decsel_plain_erases_protected) {
    ghostcon_term_t t;
    ASSERT(ghostcon_term_init(&t, 80, 24, 500), "init");
    feed(&t, "\x1b[1\"q");
    feed(&t, "ABC");
    feed(&t, "\x1b[0\"q");
    feed(&t, "\x1b[0G");
    feed(&t, "\x1b[0K");       /* plain EL ignores protection (DEC mode) */
    ASSERT_CELL(t, 0, 0, 0,   "protected erased by plain EL");
    ASSERT_CELL(t, 2, 0, 0,   "protected erased by plain EL");
    ghostcon_term_deinit(&t);
}

TEST(decsed) {
    ghostcon_term_t t;
    ASSERT(ghostcon_term_init(&t, 80, 24, 500), "init");
    feed(&t, "\x1b[1\"q");
    feed(&t, "ABC");           /* row 0 protected */
    feed(&t, "\x1b[0\"q");
    feed(&t, "\x1b[2;1H");     /* row 1 */
    feed(&t, "ZZZ");
    feed(&t, "\x1b[?1J");      /* DECSED erase above */
    ASSERT_CELL(t, 0, 0, 'A', "protected survives DECSED");
    ASSERT_CELL(t, 2, 0, 'C', "protected survives DECSED");
    ASSERT_CELL(t, 0, 1, 0,   "unprotected row erased");
    /* plain ED all ignores protection */
    feed(&t, "\x1b[2J");
    ASSERT_CELL(t, 0, 0, 0,   "protected erased by plain ED");
    ghostcon_term_deinit(&t);
}

TEST(decsca_mode2_is_off) {
    ghostcon_term_t t;
    ASSERT(ghostcon_term_init(&t, 80, 24, 500), "init");
    feed(&t, "\x1b[1\"q");     /* DECSCA 1 = DEC protect on */
    ASSERT(t.screen.cursor.protected, "protected on");
    feed(&t, "\x1b[2\"q");     /* DECSCA 2 — Ghostty maps 2 to OFF (not ISO!) */
    ASSERT(!t.screen.cursor.protected, "DECSCA 2 clears protection");
    feed(&t, "AB");
    ASSERT_PROTECTED(t, 0, 0, false, "cells written in mode 2 not protected");
    feed(&t, "\x1b[0G\x1b[0K"); /* plain EL clears everything */
    ASSERT_CELL(t, 0, 0, 0,   "plain EL clears");
    ghostcon_term_deinit(&t);
}

TEST(save_restore_mode) {
    ghostcon_term_t t;
    ASSERT(ghostcon_term_init(&t, 80, 24, 500), "init");
    feed(&t, "\x1b[?6h");      /* DECOM on */
    ASSERT(t.screen.origin_mode, "origin on");
    feed(&t, "\x1b[?6s");      /* save mode 6 */
    feed(&t, "\x1b[?6l");      /* DECOM off */
    ASSERT(!t.screen.origin_mode, "origin off");
    feed(&t, "\x1b[?6r");      /* restore mode 6 */
    ASSERT(t.screen.origin_mode, "origin restored");

    feed(&t, "\x1b[?7l");      /* DECAWM off */
    ASSERT(!t.screen.auto_wrap, "wrap off");
    feed(&t, "\x1b[?7s");      /* save mode 7 */
    feed(&t, "\x1b[?7h");      /* DECAWM on */
    ASSERT(t.screen.auto_wrap, "wrap on");
    feed(&t, "\x1b[?7r");      /* restore mode 7 */
    ASSERT(!t.screen.auto_wrap, "wrap restored");
    ghostcon_term_deinit(&t);
}

TEST(save_mode_not_margin) {
    ghostcon_term_t t;
    ASSERT(ghostcon_term_init(&t, 80, 24, 500), "init");
    feed(&t, "\x1b[?6s");      /* save mode — must NOT set margins */
    ASSERT_EQ_U(t.screen.margin_region.left, 0, "margins untouched by ? s");
    ASSERT_EQ_U(t.screen.margin_region.right, 79, "margins untouched by ? s");
    ghostcon_term_deinit(&t);
}

TEST(decrqm_parses_without_side_effects) {
    ghostcon_term_t t;
    ASSERT(ghostcon_term_init(&t, 80, 24, 500), "init");
    feed(&t, "\x1b[?69h");     /* set a mode */
    ASSERT(t.screen.left_right_margin, "mode 69 set");
    feed(&t, "\x1b[1$p");      /* DECRQM ansi — no-op query, no side effects */
    ASSERT(t.screen.left_right_margin, "DECRQM does not touch mode");
    feed(&t, "\x1b[?1$p");     /* DECRQM dec — no-op query */
    ASSERT(t.screen.left_right_margin, "DECRQM does not touch mode");
    feed(&t, "\x1b[!p");       /* DECSTR — unhandled, ignored */
    ASSERT(t.screen.left_right_margin, "DECSTR ignored");
    feed(&t, "\x1b[\"p");      /* DECSCL — unhandled, ignored */
    ASSERT(t.screen.left_right_margin, "DECSCL ignored");
    ghostcon_term_deinit(&t);
}

/* ================================================================== */
/* Missing CSI finals: REP, CHT, CBT, HPA(`), VPA(d), HPR(a), VPR(e)  */
/* ================================================================== */

TEST(repeat_character) {
    ghostcon_term_t t;
    ASSERT(ghostcon_term_init(&t, 80, 24, 500), "init");
    feed(&t, "X");
    ASSERT_CELL(t, 0, 0, 'X', "char printed");
    feed(&t, "\x1b[4b");        /* REP 4 times */
    ASSERT_CELL(t, 0, 0, 'X', "cell 0 unchanged");
    ASSERT_CELL(t, 1, 0, 'X', "cell 1 repeated");
    ASSERT_CELL(t, 2, 0, 'X', "cell 2 repeated");
    ASSERT_CELL(t, 3, 0, 'X', "cell 3 repeated");
    ASSERT_CELL(t, 4, 0, 'X', "cell 4 repeated");
    ASSERT_CELL(t, 5, 0, 0,   "cell 5 empty");
    ASSERT_CURSOR(t, 5, 0, "cursor after REP");
    ghostcon_term_deinit(&t);
}

TEST(repeat_zero_uses_one) {
    ghostcon_term_t t;
    ASSERT(ghostcon_term_init(&t, 80, 24, 500), "init");
    feed(&t, "A");
    feed(&t, "\x1b[0b");        /* REP 0 → prints 1 (Ghostty @max(count,1)) */
    ASSERT_CELL(t, 0, 0, 'A', "first A");
    ASSERT_CELL(t, 1, 0, 'A', "REP 0 still prints once");
    ASSERT_CELL(t, 2, 0, 0,   "cell 2 empty");
    ghostcon_term_deinit(&t);
}

TEST(repeat_no_previous) {
    ghostcon_term_t t;
    ASSERT(ghostcon_term_init(&t, 80, 24, 500), "init");
    feed(&t, "\x1b[3b");        /* no previous char → no-op */
    ASSERT_CELL(t, 0, 0, 0,    "no output");
    ghostcon_term_deinit(&t);
}

TEST(cursor_horizontal_tab) {
    ghostcon_term_t t;
    ASSERT(ghostcon_term_init(&t, 80, 24, 500), "init");
    feed(&t, "\x1b[9;1H");      /* row 9 col 1 (1-based) = y=8, x=0 */
    ASSERT_CURSOR(t, 0, 8, "cursor at col 0");
    feed(&t, "\x1b[2I");        /* CHT: 2 tab stops forward */
    /* default tabstops are every 8 cols: 8, 16, ... */
    ASSERT_CURSOR(t, 16, 8, "cursor after 2 tabs");
    ghostcon_term_deinit(&t);
}

TEST(cursor_backward_tab) {
    ghostcon_term_t t;
    ASSERT(ghostcon_term_init(&t, 80, 24, 500), "init");
    feed(&t, "\x1b[1;17H");     /* col 17 (1-based) = x=16 */
    ASSERT_CURSOR(t, 16, 0, "cursor at col 16");
    feed(&t, "\x1b[3Z");        /* CBT: 3 tab stops backward */
    ASSERT_CURSOR(t, 0, 0, "cursor back to col 0");
    ghostcon_term_deinit(&t);
}

TEST(hpa_backtick) {
    ghostcon_term_t t;
    ASSERT(ghostcon_term_init(&t, 80, 24, 500), "init");
    feed(&t, "Hello");
    ASSERT_CURSOR(t, 5, 0, "after Hello");
    feed(&t, "\x1b[10`");       /* HPA: col 10 (1-based) */
    ASSERT_CURSOR(t, 9, 0, "cursor at col 9");
    feed(&t, "X");
    ASSERT_CELL(t, 9, 0, 'X', "X at col 9");
    ghostcon_term_deinit(&t);
}

TEST(vpa_cursor_row) {
    ghostcon_term_t t;
    ASSERT(ghostcon_term_init(&t, 80, 24, 500), "init");
    feed(&t, "A");
    ASSERT_CURSOR(t, 1, 0, "after A");
    feed(&t, "\x1b[12d");       /* VPA: row 12 (1-based) */
    ASSERT_CURSOR(t, 1, 11, "cursor at row 11, same col");
    feed(&t, "B");
    ASSERT_CELL(t, 1, 11, 'B', "B at row 11");
    ghostcon_term_deinit(&t);
}

TEST(hpr_cursor_forward) {
    ghostcon_term_t t;
    ASSERT(ghostcon_term_init(&t, 80, 24, 500), "init");
    feed(&t, "\x1b[1;1H");
    feed(&t, "\x1b[10a");       /* HPR: forward 10 (from x=0) */
    ASSERT_CURSOR(t, 10, 0, "cursor forward 10");
    ghostcon_term_deinit(&t);
}

TEST(vpr_cursor_down) {
    ghostcon_term_t t;
    ASSERT(ghostcon_term_init(&t, 80, 24, 500), "init");
    feed(&t, "\x1b[1;1H");
    feed(&t, "\x1b[5e");        /* VPR: down 5 (from y=0) */
    ASSERT_CURSOR(t, 0, 5, "cursor down 5");
    ghostcon_term_deinit(&t);
}

/* ================================================================== */
/* Output channel: DA1, DA2, DA3, DSR, DECRQM                         */
/* ================================================================== */

TEST(da1_response) {
    ghostcon_term_t t;
    gc_test_output_t out = { .len = 0 };
    ASSERT(ghostcon_term_init(&t, 80, 24, 500), "init");
    ghostcon_term_set_output(&t, gc_test_output_fn, &out);
    feed(&t, "\x1b[c");         /* DA1 */
    ASSERT_OUTPUT(out, "\x1b[?62;22c", "DA1 response");
    ghostcon_term_deinit(&t);
}

TEST(da2_response) {
    ghostcon_term_t t;
    gc_test_output_t out = { .len = 0 };
    ASSERT(ghostcon_term_init(&t, 80, 24, 500), "init");
    ghostcon_term_set_output(&t, gc_test_output_fn, &out);
    feed(&t, "\x1b[>c");        /* DA2 */
    ASSERT_OUTPUT(out, "\x1b[>1;0;0c", "DA2 response");
    ghostcon_term_deinit(&t);
}

TEST(da3_response) {
    ghostcon_term_t t;
    gc_test_output_t out = { .len = 0 };
    ASSERT(ghostcon_term_init(&t, 80, 24, 500), "init");
    ghostcon_term_set_output(&t, gc_test_output_fn, &out);
    feed(&t, "\x1b[=c");        /* DA3 */
    ASSERT_OUTPUT(out, "\x1bP!|00000000\x1b\\", "DA3 response");
    ghostcon_term_deinit(&t);
}

TEST(dsr_operating_status) {
    ghostcon_term_t t;
    gc_test_output_t out = { .len = 0 };
    ASSERT(ghostcon_term_init(&t, 80, 24, 500), "init");
    ghostcon_term_set_output(&t, gc_test_output_fn, &out);
    feed(&t, "\x1b[5n");        /* DSR operating status */
    ASSERT_OUTPUT(out, "\x1b[0n", "DSR status OK");
    ghostcon_term_deinit(&t);
}

TEST(dsr_cursor_position) {
    ghostcon_term_t t;
    gc_test_output_t out = { .len = 0 };
    ASSERT(ghostcon_term_init(&t, 80, 24, 500), "init");
    ghostcon_term_set_output(&t, gc_test_output_fn, &out);
    feed(&t, "\x1b[5;10H");     /* CUP row 5, col 10 (1-based) */
    gc_test_output_reset(&out);
    feed(&t, "\x1b[6n");        /* DSR cursor position */
    ASSERT_OUTPUT(out, "\x1b[5;10R", "CPR row=5 col=10");
    ghostcon_term_deinit(&t);
}

TEST(decrqm_ansi) {
    ghostcon_term_t t;
    gc_test_output_t out = { .len = 0 };
    ASSERT(ghostcon_term_init(&t, 80, 24, 500), "init");
    ghostcon_term_set_output(&t, gc_test_output_fn, &out);
    feed(&t, "\x1b[4h");        /* IRM set */
    gc_test_output_reset(&out);
    feed(&t, "\x1b[4$p");       /* DECRQM ansi mode 4 */
    ASSERT_OUTPUT(out, "\x1b[4;1$y", "DECRQM IRM set");
    feed(&t, "\x1b[4l");        /* IRM reset */
    gc_test_output_reset(&out);
    feed(&t, "\x1b[4$p");       /* DECRQM ansi mode 4 */
    ASSERT_OUTPUT(out, "\x1b[4;2$y", "DECRQM IRM reset");
    ghostcon_term_deinit(&t);
}

TEST(decrqm_dec) {
    ghostcon_term_t t;
    gc_test_output_t out = { .len = 0 };
    ASSERT(ghostcon_term_init(&t, 80, 24, 500), "init");
    ghostcon_term_set_output(&t, gc_test_output_fn, &out);
    feed(&t, "\x1b[?69h");      /* DECLRMM set */
    gc_test_output_reset(&out);
    feed(&t, "\x1b[?69$p");     /* DECRQM dec mode 69 */
    ASSERT_OUTPUT(out, "\x1b[?69;1$y", "DECRQM DECLRMM set");
    ghostcon_term_deinit(&t);
}

TEST(decrqm_unknown) {
    ghostcon_term_t t;
    gc_test_output_t out = { .len = 0 };
    ASSERT(ghostcon_term_init(&t, 80, 24, 500), "init");
    ghostcon_term_set_output(&t, gc_test_output_fn, &out);
    feed(&t, "\x1b[?999$p");    /* unknown mode */
    ASSERT_OUTPUT(out, "\x1b[?999;0$y", "DECRQM unknown");
    ghostcon_term_deinit(&t);
}

/* ================================================================== */
/* CSI parameter robustness                                            */
/* ================================================================== */

TEST(csi_too_many_params) {
    ghostcon_term_t t;
    ASSERT(ghostcon_term_init(&t, 80, 24, 500), "init");
    /* 30 params >= GC_STREAM_MAX_PARAMS (24) — Ghostty drops the entire
       sequence (csiDispatchFinal returns before dispatch). */
    feed(&t, "\x1b[1;2;3;4;5;6;7;8;9;10;11;12;13;14;15;16;17;18;19;20;21;22;23;24;25;26;27;28;29;30H");
    ASSERT_CURSOR(t, 0, 0, "over-parameterized CUP ignored");
    ghostcon_term_deinit(&t);
}

TEST(csi_param_count_strict) {
    ghostcon_term_t t;
    ASSERT(ghostcon_term_init(&t, 80, 24, 500), "init");
    feed(&t, "A");
    feed(&t, "\x1b[1;2;3H");    /* CUP with 3 params — Ghostty ignores */
    ASSERT_CURSOR(t, 1, 0, "CUP with 3 params ignored");
    feed(&t, "\x1b[1;2;3A");    /* CUU with 3 params — ignored */
    ASSERT_CURSOR(t, 1, 0, "CUU with 3 params ignored");
    feed(&t, "\x1b[2;3B");      /* CUD with 2 params — ignored */
    ASSERT_CURSOR(t, 1, 0, "CUD with 2 params ignored");
    feed(&t, "\x1b[3J");        /* ED with 3 params — ignored */
    ASSERT_CELL(t, 0, 0, 'A', "ED with 2 params ignored");
    ghostcon_term_deinit(&t);
}

TEST(dec_origin_mode_homes_cursor) {
    ghostcon_term_t t;
    ASSERT(ghostcon_term_init(&t, 80, 24, 500), "init");
    feed(&t, "\x1b[5;10H");     /* move away */
    ASSERT_CURSOR(t, 9, 4, "cursor at 5,10");
    feed(&t, "\x1b[?6h");       /* DECOM on — homes cursor */
    ASSERT_CURSOR(t, 0, 0, "DECOM on homes cursor");
    feed(&t, "\x1b[5;10H");
    feed(&t, "\x1b[?6l");       /* DECOM off — homes cursor */
    ASSERT_CURSOR(t, 0, 0, "DECOM off homes cursor");
    ghostcon_term_deinit(&t);
}

TEST(csi_param_overflow) {
    ghostcon_term_t t;
    ASSERT(ghostcon_term_init(&t, 80, 24, 500), "init");
    /* 18-digit param wraps at u16 (Ghostty *|/+|) -> 65535, clamped */
    feed(&t, "\x1b[999999999999999999;1H");
    ASSERT_CURSOR(t, 0, 23, "overflow param clamped to last row");
    ghostcon_term_deinit(&t);
}

/* ================================================================== */
/* UTF-8 decoding                                                      */
/* ================================================================== */

TEST(utf8_basic) {
    ghostcon_term_t t;
    ASSERT(ghostcon_term_init(&t, 80, 24, 500), "init");
    feed(&t, "h\xC3\xA9" "llo");   /* héllo */
    ASSERT_CELL(t, 0, 0, 'h', "h");
    ASSERT_CELL(t, 1, 0, 0x00E9, "é");
    ASSERT_CELL(t, 2, 0, 'l', "l1");
    ASSERT_CELL(t, 3, 0, 'l', "l2");
    ASSERT_CELL(t, 4, 0, 'o', "o");
    ASSERT_CURSOR(t, 5, 0, "cursor after héllo");
    ghostcon_term_deinit(&t);
}

TEST(utf8_wide_char) {
    ghostcon_term_t t;
    ASSERT(ghostcon_term_init(&t, 80, 24, 500), "init");
    feed(&t, "A\xE4\xB8\xAD");   /* A中 */
    ASSERT_CELL(t, 0, 0, 'A', "A");
    ASSERT_WIDE(t, 0, 0, GHOSTCON_CELL_WIDE_NARROW, "A narrow");
    ASSERT_CELL(t, 1, 0, 0x4E2D, "中");
    ASSERT_WIDE(t, 1, 0, GHOSTCON_CELL_WIDE_WIDE, "中 wide");
    ASSERT_CELL(t, 2, 0, 0, "spacer tail empty");
    ASSERT_WIDE(t, 2, 0, GHOSTCON_CELL_WIDE_SPACER_TAIL, "spacer tail wide");
    ASSERT_CURSOR(t, 3, 0, "cursor after wide char");
    ghostcon_term_deinit(&t);
}

TEST(utf8_wide_split_across_feed) {
    ghostcon_term_t t;
    ASSERT(ghostcon_term_init(&t, 80, 24, 500), "init");
    feed(&t, "A");
    ghostcon_term_feed(&t, (const uint8_t *)"\xE4\xB8", 2);
    ASSERT_CURSOR(t, 1, 0, "cursor during partial sequence");
    ghostcon_term_feed(&t, (const uint8_t *)"\xAD", 1);
    ASSERT_CELL(t, 1, 0, 0x4E2D, "中 completes");
    ASSERT_WIDE(t, 1, 0, GHOSTCON_CELL_WIDE_WIDE, "中 wide");
    ASSERT_CURSOR(t, 3, 0, "cursor after completed wide char");
    ghostcon_term_deinit(&t);
}

TEST(utf8_combining) {
    ghostcon_term_t t;
    ASSERT(ghostcon_term_init(&t, 80, 24, 500), "init");
    feed(&t, "e\xCC\x81");   /* e + combining acute */
    ASSERT_CELL(t, 0, 0, 'e', "base char preserved");
    ASSERT_TAG(t, 0, 0, GHOSTCON_CELL_CODEPOINT_GRAPHEME, "combining attached");
    ASSERT(ghostcon_screen_row(&t.screen, 0)->grapheme, "row grapheme flag");
    ASSERT_CURSOR(t, 1, 0, "cursor does not advance");
    ghostcon_term_deinit(&t);
}

TEST(utf8_combining_after_wide) {
    ghostcon_term_t t;
    ASSERT(ghostcon_term_init(&t, 80, 24, 500), "init");
    feed(&t, "\xE4\xB8\xAD" "\xCC\x81");   /* 中 + combining acute */
    ASSERT_CELL(t, 0, 0, 0x4E2D, "中 base");
    ASSERT_WIDE(t, 0, 0, GHOSTCON_CELL_WIDE_WIDE, "中 still wide");
    ASSERT_TAG(t, 0, 0, GHOSTCON_CELL_CODEPOINT_GRAPHEME, "combining attached to wide");
    ASSERT_WIDE(t, 1, 0, GHOSTCON_CELL_WIDE_SPACER_TAIL, "spacer tail intact");
    ASSERT_CURSOR(t, 2, 0, "cursor after wide+combining");
    ghostcon_term_deinit(&t);
}

TEST(utf8_invalid_replaced) {
    ghostcon_term_t t;
    ASSERT(ghostcon_term_init(&t, 80, 24, 500), "init");
    feed(&t, "\xFF\x80");   /* invalid lead + lone continuation */
    ASSERT_CELL(t, 0, 0, 0xFFFD, "FFFD from 0xFF");
    ASSERT_CELL(t, 1, 0, 0xFFFD, "FFFD from 0x80");
    ASSERT_CURSOR(t, 2, 0, "cursor after two replacement chars");
    ghostcon_term_deinit(&t);
}

TEST(utf8_incomplete_then_ascii) {
    ghostcon_term_t t;
    ASSERT(ghostcon_term_init(&t, 80, 24, 500), "init");
    feed(&t, "\xC3" "x");   /* truncated 2-byte lead then 'x' */
    ASSERT_CELL(t, 0, 0, 0xFFFD, "truncated sequence -> FFFD");
    ASSERT_CELL(t, 1, 0, 'x', "'x' retried and printed");
    ASSERT_CURSOR(t, 2, 0, "cursor after FFFD + x");
    ghostcon_term_deinit(&t);
}

TEST(utf8_partially_invalid_ghostty_parity) {
    ghostcon_term_t t;
    ASSERT(ghostcon_term_init(&t, 80, 24, 500), "init");
    /* Mirror of Ghostty's UTF8Decoder "Partially invalid utf-8" test:
       \xF0\x9F😄\xED\xA0\x80 -> FFFD, 1F604, FFFD, FFFD, FFFD.
       😄 (U+1F604) is width 2, so it occupies two cells. */
    feed(&t, "\xF0\x9F" "\xF0\x9F\x98\x84" "\xED\xA0\x80");
    ASSERT_CELL(t, 0, 0, 0xFFFD, "truncated 4-byte lead");
    ASSERT_CELL(t, 1, 0, 0x1F604, "😄 decodes");
    ASSERT_WIDE(t, 1, 0, GHOSTCON_CELL_WIDE_WIDE, "😄 is wide");
    ASSERT_WIDE(t, 2, 0, GHOSTCON_CELL_WIDE_SPACER_TAIL, "😄 spacer tail");
    ASSERT_CELL(t, 3, 0, 0xFFFD, "lone ED A0 80 part 1");
    ASSERT_CELL(t, 4, 0, 0xFFFD, "lone ED A0 80 part 2");
    ASSERT_CELL(t, 5, 0, 0xFFFD, "lone ED A0 80 part 3");
    ASSERT_CURSOR(t, 6, 0, "cursor after 6 cells");
    ghostcon_term_deinit(&t);
}

TEST(utf8_wide_at_margin) {
    ghostcon_term_t t;
    ASSERT(ghostcon_term_init(&t, 80, 24, 500), "init");
    /* 79 'A's fill cols 0-78 (cursor at 79, pending wrap), then a wide
       char wraps to the next line. */
    char buf[80];
    memset(buf, 'A', 79);
    ghostcon_term_feed(&t, (const uint8_t *)buf, 79);
    ASSERT_CURSOR(t, 79, 0, "cursor at last column");
    feed(&t, "\xE4\xB8\xAD");
    ASSERT_CELL(t, 78, 0, 'A', "row 0 col 78 keeps A");
    ASSERT_CELL(t, 79, 0, 0, "row 0 col 79 stays empty");
    ASSERT_CELL(t, 0, 1, 0x4E2D, "中 wraps to next row");
    ASSERT_WIDE(t, 0, 1, GHOSTCON_CELL_WIDE_WIDE, "中 wide on next row");
    ASSERT_WIDE(t, 1, 1, GHOSTCON_CELL_WIDE_SPACER_TAIL, "spacer tail on next row");
    ASSERT(ghostcon_screen_row(&t.screen, 0)->wrap, "row 0 marked wrapped");
    ASSERT(ghostcon_screen_row(&t.screen, 1)->wrap_continuation, "row 1 continuation");
    ASSERT_CURSOR(t, 2, 1, "cursor after wrapped wide char");
    ghostcon_term_deinit(&t);
}

TEST(utf8_wide_last_col_direct) {
    ghostcon_term_t t;
    ASSERT(ghostcon_term_init(&t, 80, 24, 500), "init");
    feed(&t, "\x1b[1;80H");   /* cursor to col 79 (row 1, col 80) */
    feed(&t, "\xE4\xB8\xAD");
    ASSERT_CELL(t, 79, 0, 0, "spacer head empty");
    ASSERT_WIDE(t, 79, 0, GHOSTCON_CELL_WIDE_SPACER_HEAD, "spacer head at last col");
    ASSERT_CELL(t, 0, 1, 0x4E2D, "wide char on next row");
    ASSERT_CURSOR(t, 2, 1, "cursor on next row");
    ghostcon_term_deinit(&t);
}

TEST(utf8_lone_c1_replaced) {
    ghostcon_term_t t;
    ASSERT(ghostcon_term_init(&t, 80, 24, 500), "init");
    /* A lone 8-bit C1 byte in ground is invalid UTF-8 -> U+FFFD */
    feed(&t, "\x9B");
    ASSERT_CELL(t, 0, 0, 0xFFFD, "lone CSI byte -> FFFD");
    ASSERT_CURSOR(t, 1, 0, "cursor advanced");
    ghostcon_term_deinit(&t);
}

/* ================================================================== */
/* Main — runs all registered tests                                    */
/* ================================================================== */

int main(void) {
    return gc_test_run_all();
}
