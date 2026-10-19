/* test_ui
 *
 * unit tests for the UI interactions layer: cursor hover hit-testing, text
 * selection transitions, zoom calculation, and command token / process name
 * formatting.
 */

#include "ghostcon/core/ui.h"
#include "ghostcon/term/screen.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
    else { printf("PASS: %s\n", msg); } \
} while (0)

int
main(void)
{
    /* --- Zoom computation --- */
    CHECK(ghostcon_ui_compute_zoom(16, 0, 2, 6, 96) == 16, "zoom delta 0 preserves size");
    CHECK(ghostcon_ui_compute_zoom(16, 1, 2, 6, 96) == 18, "zoom delta +1 adds step");
    CHECK(ghostcon_ui_compute_zoom(16, -1, 2, 6, 96) == 14, "zoom delta -1 subtracts step");
    CHECK(ghostcon_ui_compute_zoom(7, -1, 2, 6, 96) == 6, "zoom clamps to min size");
    CHECK(ghostcon_ui_compute_zoom(95, 1, 2, 6, 96) == 96, "zoom clamps to max size");

    /* --- Command token extraction --- */
    char token[64];
    ghostcon_ui_extract_command_token("htop", token, sizeof(token));
    CHECK(strcmp(token, "htop") == 0, "extract token: simple command");

    ghostcon_ui_extract_command_token("/usr/bin/htop --delay 10", token, sizeof(token));
    CHECK(strcmp(token, "htop") == 0, "extract token: path with arguments");

    ghostcon_ui_extract_command_token("python3 myscript.py", token, sizeof(token));
    CHECK(strcmp(token, "python3") == 0, "extract token: command with arguments");

    ghostcon_ui_extract_command_token("", token, sizeof(token));
    CHECK(token[0] == '\0', "extract token: empty string");

    /* --- Comm name formatting (PR_SET_NAME) --- */
    char comm[16];
    ghostcon_ui_build_comm_name(4, "bash", comm);
    CHECK(strcmp(comm, "gc@tty4 bash") == 0, "comm name: short token");
    CHECK(strlen(comm) < 16, "comm name: fits in 16 bytes");

    ghostcon_ui_build_comm_name(4, "verylongcommandnamehere", comm);
    CHECK(strlen(comm) <= 15, "comm name: long token truncated to <= 15 chars");
    CHECK(strstr(comm, "...") != NULL, "comm name: tail truncation uses dots");

    /* --- Screen-dependent tests: Cursor Hover and Selection --- */
    ghostcon_screen_t screen;
    ghostcon_screen_init(&screen, 80, 24, 100);

    const int cell_w = 10;
    const int cell_h = 20;

    /* Cell (5, 2) is a hyperlink, other cells are not */
    ghostcon_row_t *row2 = ghostcon_screen_row(&screen, 2);
    ghostcon_cell_t link_cell = {0};
    ghostcon_cell_set_hyperlink_id(&link_cell, 42); /* non-zero hyperlink id */
    row2->cells[5] = link_cell;

    /* Hover test */
    ghostcon_cursor_state_t state;
    state = ghostcon_ui_compute_cursor_state(&screen, cell_w, cell_h, 55, 45); /* col 5, row 2 */
    CHECK(state == GC_CURSOR_STATE_LINK, "hover on hyperlink cell returns GC_CURSOR_STATE_LINK");

    state = ghostcon_ui_compute_cursor_state(&screen, cell_w, cell_h, 25, 45); /* col 2, row 2 */
    CHECK(state == GC_CURSOR_STATE_DEFAULT, "hover on normal cell returns GC_CURSOR_STATE_DEFAULT");

    state = ghostcon_ui_compute_cursor_state(&screen, cell_w, cell_h, -5, 10); /* out of bounds */
    CHECK(state == GC_CURSOR_STATE_DEFAULT, "hover out of bounds returns GC_CURSOR_STATE_DEFAULT");

    state = ghostcon_ui_compute_cursor_state(&screen, cell_w, cell_h, 1000, 1000); /* beyond grid */
    CHECK(state == GC_CURSOR_STATE_DEFAULT, "hover beyond grid returns GC_CURSOR_STATE_DEFAULT");

    /* Selection interaction test */
    ghostcon_input_pointer_t ptr = {0};

    /* Click press at col 2, row 1 */
    ptr.x = 25;
    ptr.y = 25;
    ptr.left_pressed = true;
    bool need_render = ghostcon_ui_update_selection(&screen, &ptr, cell_w, cell_h);
    CHECK(need_render, "selection press triggers render");
    CHECK(screen.selection.active && screen.selection.pending, "selection is active and pending");
    CHECK(screen.selection.x1 == 2 && screen.selection.y1 == 1, "selection starts at (2, 1)");

    /* Drag to col 8, row 1 */
    ptr.left_pressed = false;
    ptr.moved = true;
    ptr.x = 85;
    ptr.y = 25;
    need_render = ghostcon_ui_update_selection(&screen, &ptr, cell_w, cell_h);
    CHECK(need_render, "selection drag triggers render");
    CHECK(screen.selection.x2 == 8 && screen.selection.y2 == 1, "selection moves to (8, 1)");

    /* Release drag */
    ptr.moved = false;
    ptr.left_released = true;
    need_render = ghostcon_ui_update_selection(&screen, &ptr, cell_w, cell_h);
    CHECK(need_render, "selection release triggers render");
    CHECK(screen.selection.active && !screen.selection.pending, "selection finished and active");
    CHECK(screen.selection.x1 == 2 && screen.selection.x2 == 8, "endpoints preserved");

    /* Click in place (no drag) clears selection */
    ptr.left_released = false;
    ptr.left_pressed = true;
    ptr.x = 40;
    ptr.y = 20;
    ghostcon_ui_update_selection(&screen, &ptr, cell_w, cell_h);

    ptr.left_pressed = false;
    ptr.left_released = true;
    ghostcon_ui_update_selection(&screen, &ptr, cell_w, cell_h);
    CHECK(!screen.selection.active, "click in place (no drag) clears selection");

    ghostcon_screen_deinit(&screen);

    if (failures == 0)
        printf("ALL TESTS PASSED\n");
    return failures > 0 ? 1 : 0;
}
