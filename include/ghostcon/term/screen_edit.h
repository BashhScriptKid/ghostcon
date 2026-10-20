#pragma once

#include <stddef.h>
#include <stdint.h>

#include "screen_fwd.h"

/* Editing operations
 *
 * The editing half of the screen model: printing text, erasing, and the
 * insert/delete operations that move existing content around.
 */

/* Insert a single character at cursor position, advancing cursor */
extern void ghostcon_screen_put_char(ghostcon_screen_t *screen, uint32_t codepoint);

/* Insert a string of codepoints */
extern void ghostcon_screen_put_text(ghostcon_screen_t *screen, const uint32_t *codepoints, size_t len);

/* Erase operations */

#define GC_ERASE_DISPLAY_BELOW       0
#define GC_ERASE_DISPLAY_ABOVE       1
#define GC_ERASE_DISPLAY_ALL         2
#define GC_ERASE_DISPLAY_SCROLLBACK  3
#define GC_ERASE_DISPLAY_SCROLL_COMPLETE 4  /* scroll everything off screen */

#define GC_ERASE_LINE_RIGHT  0
#define GC_ERASE_LINE_LEFT   1
#define GC_ERASE_LINE_ALL    2

extern void ghostcon_screen_erase_display(ghostcon_screen_t *screen, int mode);
extern void ghostcon_screen_erase_line(ghostcon_screen_t *screen, int mode);
extern void ghostcon_screen_erase_chars(ghostcon_screen_t *screen, uint16_t n);
extern void ghostcon_screen_insert_chars(ghostcon_screen_t *screen, uint16_t n);
extern void ghostcon_screen_delete_chars(ghostcon_screen_t *screen, uint16_t n);

/* Selective erase (DECSED/DECSEL): like erase display/line but skips cells
 * with the protected bit set.
 */
extern void ghostcon_screen_erase_display_protected(ghostcon_screen_t *screen, int mode);
extern void ghostcon_screen_erase_line_protected(ghostcon_screen_t *screen, int mode);

/* Save/restore a single DEC mode (CSI ? Ps s / CSI ? Ps r) */
extern void ghostcon_screen_save_mode(ghostcon_screen_t *screen, int mode);
extern void ghostcon_screen_restore_mode(ghostcon_screen_t *screen, int mode);

/* Insert/Delete lines */
extern void ghostcon_screen_insert_lines(ghostcon_screen_t *screen, uint16_t n);
extern void ghostcon_screen_delete_lines(ghostcon_screen_t *screen, uint16_t n);
