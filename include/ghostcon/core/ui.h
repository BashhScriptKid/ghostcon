#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "ghostcon/core/cursor_image.h"
#include "ghostcon/core/input.h"
#include "ghostcon/term/screen.h"

/* UI Interactions Layer
 *
 * Pure logic for mouse hover hit-testing, text selection transitions, zoom
 * calculations, and terminal process-identity formatting. Independent of
 * GPU/DRM/KMS hardware rendering context.
 */

/* Computes which cursor state (e.g. DEFAULT vs LINK for hyperlinks) the
 * pointer should display based on its pixel position over the terminal
 * grid. Returns GC_CURSOR_STATE_DEFAULT if not over a hyperlink or out of
 * bounds.
 */
ghostcon_cursor_state_t ghostcon_ui_compute_cursor_state(ghostcon_screen_t *screen,
                                                         int cell_w, int cell_h,
                                                         int pointer_x, int pointer_y);

/* Handles click-drag text selection transitions (left pressed, moved,
 * released). Updates screen->selection directly. Returns true if the
 * selection changed state and a screen redraw is needed.
 */
bool ghostcon_ui_update_selection(ghostcon_screen_t *screen,
                                  const ghostcon_input_pointer_t *pointer,
                                  int cell_w, int cell_h);

/* Computes the clamped font size given a zoom delta (+1 or -1 tick), zoom
 * step, and min/max bounds.
 */
int ghostcon_ui_compute_zoom(int current_font_size, int zoom_delta, int zoom_step,
                             int min_font_size, int max_font_size);

/* Extracts the "command token" from a terminal window title (OSC 0/2): the
 * first word if the title looks like "cmd args...", reduced to its basename
 * if that word is a path.
 */
void ghostcon_ui_extract_command_token(const char *title, char *out, size_t out_len);

/* Builds the truncated `comm` name (PR_SET_NAME, 15 bytes max incl. NUL) in
 * the form "gc@tty<N> " + token, tail-truncated with a leading "..." if it
 * exceeds budget. `out` must be at least 16 bytes.
 */
void ghostcon_ui_build_comm_name(int vt_num, const char *token, char *out);
