#include "ghostcon/term/term.h"
#include <stdio.h>
#include <string.h>

int main(void) {
    ghostcon_term_t term;

    /* Init 80x24 terminal with 500-line scrollback */
    if (!ghostcon_term_init(&term, 80, 24, 500)) {
        fprintf(stderr, "FAIL: term_init\n");
        return 1;
    }

    /* Test 1: basic text */
    ghostcon_term_feed(&term, (const uint8_t *)"Hello, World!", 13);
    ghostcon_cell_t *c = ghostcon_screen_cell(&term.screen, 0, 0);
    if (!c) { fprintf(stderr, "FAIL: null cell\n"); return 1; }
    if (ghostcon_cell_get_codepoint(*c) != 'H') {
        fprintf(stderr, "FAIL: expected 'H', got %u\n", ghostcon_cell_get_codepoint(*c));
        return 1;
    }
    printf("PASS: basic text\n");

    /* Test 2: cursor movement */
    ghostcon_term_feed(&term, (const uint8_t *)"\r\n", 2);
    if (term.screen.cursor.x != 0 || term.screen.cursor.y != 1) {
        fprintf(stderr, "FAIL: expected (0,1), got (%d,%d)\n",
                term.screen.cursor.x, term.screen.cursor.y);
        return 1;
    }
    printf("PASS: cursor movement\n");

    /* Test 3: escape sequences */
    ghostcon_term_feed(&term, (const uint8_t *)"\x1b[H", 3);  /* cursor home */
    if (term.screen.cursor.x != 0 || term.screen.cursor.y != 0) {
        fprintf(stderr, "FAIL: home expected (0,0), got (%d,%d)\n",
                term.screen.cursor.x, term.screen.cursor.y);
        return 1;
    }
    printf("PASS: cursor home\n");

    /* Test 4: erase display */
    ghostcon_term_feed(&term, (const uint8_t *)"\x1b[2J", 4);  /* erase all */
    c = ghostcon_screen_cell(&term.screen, 0, 0);
    if (!ghostcon_cell_is_empty(*c)) {
        fprintf(stderr, "FAIL: expected empty cell\n");
        return 1;
    }
    printf("PASS: erase display\n");

    /* Test 5: line wrapping */
    char buf[96];
    memset(buf, 'A', 80);    /* exactly one line */
    buf[80] = 'B';
    buf[81] = '\0';
    ghostcon_term_feed(&term, (const uint8_t *)buf, 81);
    if (term.screen.cursor.y != 1 || term.screen.cursor.x != 1) {
        fprintf(stderr, "FAIL: after wrap expected (1,1), got (%d,%d)\n",
                term.screen.cursor.x, term.screen.cursor.y);
        return 1;
    }
    printf("PASS: line wrapping\n");

    /* Test 6: scroll region */
    ghostcon_screen_set_scroll_region(&term.screen, 0, 10);
    for (int i = 0; i < 15; i++)
        ghostcon_screen_linefeed(&term.screen);
    if (term.screen.cursor.y > 10) {
        fprintf(stderr, "FAIL: scroll region overflow\n");
        return 1;
    }
    printf("PASS: scroll region\n");

    /* Test 7: alt screen */
    ghostcon_term_deinit(&term);
    if (!ghostcon_term_init(&term, 80, 24, 500)) {
        fprintf(stderr, "FAIL: reinit\n");
        return 1;
    }
    ghostcon_term_feed(&term, (const uint8_t *)"TEST", 4);
    ghostcon_screen_alt_screen_enter(&term.screen);
    ghostcon_term_feed(&term, (const uint8_t *)"ALT", 3);
    c = ghostcon_screen_cell(&term.screen, 0, 0);
    if (ghostcon_cell_get_codepoint(*c) != 'A') {
        fprintf(stderr, "FAIL: alt screen expected 'A', got '%c'\n",
                (char)ghostcon_cell_get_codepoint(*c));
        return 1;
    }
    ghostcon_screen_alt_screen_exit(&term.screen);
    c = ghostcon_screen_cell(&term.screen, 0, 0);
    if (ghostcon_cell_get_codepoint(*c) != 'T') {
        fprintf(stderr, "FAIL: alt screen exit expected 'T', got '%c' (%u)\n",
                (char)ghostcon_cell_get_codepoint(*c),
                ghostcon_cell_get_codepoint(*c));
        return 1;
    }
    printf("PASS: alt screen\n");

    /* Test 8: resize */
    if (!ghostcon_term_resize(&term, 40, 12)) {
        fprintf(stderr, "FAIL: resize\n");
        return 1;
    }
    if (term.screen.cols != 40 || term.screen.rows_visible != 12) {
        fprintf(stderr, "FAIL: resize dimensions\n");
        return 1;
    }
    printf("PASS: resize\n");

    ghostcon_term_deinit(&term);
    printf("ALL PASS\n");
    return 0;
}
