#pragma once

#include <stdint.h>

#include "screen_fwd.h"

/* Scrolling
 *
 * The scrolling half of the screen model: the scroll region, the margin
 * region, and the alternate screen.
 */

/* Scroll the visible region up/down by n lines (new lines are blank) */
extern void ghostcon_screen_scroll_up(ghostcon_screen_t *screen, uint16_t n);
extern void ghostcon_screen_scroll_down(ghostcon_screen_t *screen, uint16_t n);

/* Set scroll region (DECSTBM). top/bottom are 0-based, inclusive. Pass -1
 * for both to reset to full screen.
 */
extern void ghostcon_screen_set_scroll_region(ghostcon_screen_t *screen, int16_t top, int16_t bottom);

/* Set left/right margin region (DECSLRM). Pass -1 for both to reset. */
extern void ghostcon_screen_set_margin_region(ghostcon_screen_t *screen, int16_t left, int16_t right);

/* Alternate screen */

/* Switch to alternate screen (saves main screen grid) */
extern void ghostcon_screen_alt_screen_enter(ghostcon_screen_t *screen);

/* Switch back to main screen (restores saved grid) */
extern void ghostcon_screen_alt_screen_exit(ghostcon_screen_t *screen);
