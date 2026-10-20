#pragma once

#include <stdint.h>

#include "screen_fwd.h"

/* Cursor movement
 *
 * The cursor half of the screen model: relative and absolute moves, the
 * line-motion controls (CR, LF, RI), save/restore (DECSC/DECRC), and the
 * tab stops.
 *
 * The tab *stops* live here rather than with the rest of the editing
 * operations, because a tab stop is a cursor position
 *
 * ghostcon_screen_tab() moves the cursor to the next one,
 * ghostcon_screen_tab_set() puts one there.
 */

/* Move cursor by the scroll region (for linefeed/IND/RI) */
extern void ghostcon_screen_cursor_scroll_up(ghostcon_screen_t *screen);
extern void ghostcon_screen_cursor_scroll_down(ghostcon_screen_t *screen);

/* Save/restore cursor (DECSC/DECRC) */
extern void ghostcon_screen_cursor_save(ghostcon_screen_t *screen);
extern void ghostcon_screen_cursor_restore(ghostcon_screen_t *screen);

extern void ghostcon_screen_cursor_up(ghostcon_screen_t *screen, uint16_t n);
extern void ghostcon_screen_cursor_down(ghostcon_screen_t *screen, uint16_t n);
extern void ghostcon_screen_cursor_left(ghostcon_screen_t *screen, uint16_t n);
extern void ghostcon_screen_cursor_right(ghostcon_screen_t *screen, uint16_t n);
extern void ghostcon_screen_cursor_set(ghostcon_screen_t *screen, int16_t x, int16_t y);
extern void ghostcon_screen_cursor_horizontal_abs(ghostcon_screen_t *screen, int16_t x);
extern void ghostcon_screen_cursor_vertical_abs(ghostcon_screen_t *screen, int16_t y);
extern void ghostcon_screen_cursor_next_line(ghostcon_screen_t *screen);
extern void ghostcon_screen_cursor_prev_line(ghostcon_screen_t *screen);

/* Carriage return */
extern void ghostcon_screen_carriage_return(ghostcon_screen_t *screen);

/* Linefeed (scrolls screen if at bottom) */
extern void ghostcon_screen_linefeed(ghostcon_screen_t *screen);

/* Reverse index (scrolls up if at top) */
extern void ghostcon_screen_reverse_index(ghostcon_screen_t *screen);

/* Horizontal tab */
extern void ghostcon_screen_tab(ghostcon_screen_t *screen);

/* Backward tab */
extern void ghostcon_screen_tab_back(ghostcon_screen_t *screen);

/* Tab stops */
extern void ghostcon_screen_tab_clear(ghostcon_screen_t *screen, int mode);
extern void ghostcon_screen_tab_set(ghostcon_screen_t *screen);
