#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "cell.h"
#include "row.h"
#include "style.h"
#include "color.h"
#include "modes.h"
#include "selection.h"
#include "kitty.h"
#include "hyperlink.h"

/* ------------------------------------------------------------------ */
/* Cursor style                                                        */
/* ------------------------------------------------------------------ */
typedef enum {
    GC_CURSOR_DEFAULT   = 0,
    GC_CURSOR_BLOCK     = 1,
    GC_CURSOR_UNDERLINE = 2,
    GC_CURSOR_BAR       = 3,
    GC_CURSOR_BLOCK_BLINK   = 4,
    GC_CURSOR_UNDERLINE_BLINK = 5,
    GC_CURSOR_BAR_BLINK     = 6,
} ghostcon_cursor_style_t;

/* ------------------------------------------------------------------ */
/* Saved cursor (DECSC/DECRC)                                          */
/* ------------------------------------------------------------------ */
typedef struct {
    int16_t x, y;
    ghostcon_cursor_style_t cursor_style;
    bool pending_wrap;
    ghostcon_style_id_t style_id;
} ghostcon_saved_cursor_t;

/* ------------------------------------------------------------------ */
/* Cursor state                                                        */
/* ------------------------------------------------------------------ */
typedef struct {
    int16_t               x, y;
    ghostcon_cursor_style_t cursor_style;
    bool                  pending_wrap;  /* last-column flag */
    bool                  protected;
    ghostcon_style_id_t   style_id;       /* current text style */
    ghostcon_style_id_t   hyperlink_id;   /* current hyperlink (0 = none) */
} ghostcon_cursor_t;

/* ------------------------------------------------------------------ */
/* Scroll region (DECSTBM)                                             */
/* ------------------------------------------------------------------ */
typedef struct {
    int16_t top;    /* inclusive, 0-based */
    int16_t bottom; /* inclusive, 0-based */
} ghostcon_scroll_region_t;

/* ------------------------------------------------------------------ */
/* Character protection mode (DECSCA)                                  */
/* ------------------------------------------------------------------ */
typedef enum {
    GC_PROTECTED_OFF = 0, /* no protection */
    GC_PROTECTED_ISO,     /* ISO mode: ED/EL always respect protected cells */
    GC_PROTECTED_DEC,     /* DEC mode: only DECSED/DECSEL respect them */
} ghostcon_protected_mode_t;

/* ------------------------------------------------------------------ */
/* Saved DEC modes (CSI ? ... s / CSI ? ... r, XTSAVE/XTRESTORE)       */
/* ------------------------------------------------------------------ */
typedef struct {
    bool origin_mode;        /* DECOM  */
    bool auto_wrap;          /* DECAWM */
    bool reverse_video;      /* DECSCNM */
    bool insert_mode;        /* IRM */
    bool application_cursor; /* DECCKM */
    bool left_right_margin;  /* DECLRMM */
} ghostcon_saved_modes_t;

/* ------------------------------------------------------------------ */
/* Margin (DECSLRM)                                                    */
/* ------------------------------------------------------------------ */
typedef struct {
    int16_t left;
    int16_t right;
} ghostcon_margin_region_t;

/* ------------------------------------------------------------------ */
/* Tabstop state                                                       */
/* ------------------------------------------------------------------ */
typedef struct {
    bool *stops;      /* array of cols booleans (owned) */
    uint16_t cols;
} ghostcon_tabstops_t;

/* ------------------------------------------------------------------ */
/* Accumulated dirty region for efficient damage tracking              */
/* ------------------------------------------------------------------ */
typedef struct {
    int16_t y_min;  /* first dirty line, -1 if none */
    int16_t y_max;  /* last dirty line,  -1 if none */
} ghostcon_dirty_region_t;

/* ------------------------------------------------------------------ */
/* Screen — the terminal screen buffer                                 */
/*                                                                     */
/* Manages the grid, cursor, scrollback, alternate screen, and         */
/* damage tracking.                                                    */
/* ------------------------------------------------------------------ */
/* Tagged (not anonymous) specifically so lower-level headers that only
   need a pointer type (e.g. selection.h's extract_text() declaration)
   can forward-declare `struct ghostcon_screen;` without including this
   header -- screen.h itself includes selection.h transitively (via the
   `selection` field below), so selection.h including screen.h back
   would be circular. */
struct ghostcon_screen {
    /* Grid */
    ghostcon_row_t  *rows;        /* visible grid rows (ring buffer) */
    uint16_t         cols;
    uint16_t         rows_visible;
    int16_t          scrollback_top;  /* index of first visible row in ring buffer */

    /* Scrollback ring buffer */
    ghostcon_row_t  *history;     /* historical rows (ring buffer) */
    uint16_t         history_cap;
    uint16_t         history_count;
    int16_t          history_head;  /* write cursor in history ring buffer */

    /* How many lines back into `history` the VIEW currently is (0 =
       live, showing `rows`; up to history_count = fully scrolled back).
       Distinct from `scrollback_top` above, which is an internal ring-
       buffer rotation index for `rows` itself, not a user-facing scroll
       position — see ghostcon_screen_row()'s own doc comment. */
    uint16_t         view_offset;

    /* Alternate screen */
    ghostcon_row_t  *alt_rows;        /* saved visible rows when switching to alt screen */
    uint16_t         alt_rows_visible;
    bool             alt_screen_active;
    int16_t          alt_scrollback_top;

    /* Cursor */
    ghostcon_cursor_t cursor;
    ghostcon_cursor_t saved_cursor;
    bool              cursor_saved;

    /* Last printed codepoint (REP — CSI b) */
    uint32_t          last_codepoint;

    /* Regions & tabs */
    ghostcon_scroll_region_t  scroll_region;
    ghostcon_margin_region_t  margin_region;
    ghostcon_tabstops_t       tabstops;

    /* Mode flags (also stored individually below for fast access) */
    ghostcon_modes_t modes;
    bool             origin_mode;         /* DECOM */
    bool             auto_wrap;            /* DECAWM */
    bool             cursor_visible;       /* DECTCEM -- defaults true (see
                                               screen_init), unlike every
                                               other private mode here, which
                                               correctly default off */
    bool             reverse_video;       /* DECSCNM */
    bool             insert_mode;          /* IRM */
    bool             application_cursor;  /* DECKPEM/Cursor keys app mode */
    bool             synchronized_output; /* Mode 2026 */
    bool             bracketed_paste;     /* Mode 2004 */
    bool             left_right_margin;   /* DECSLRM — mode 69 */
    bool             mouse_tracking;      /* Mode 1000/1002/1003/etc. */
    uint16_t         mouse_protocol;      /* 0=none, 1000=button, 1002=button+drag, 1003=motion, 1005=ext, 1006=SGR */
    bool             mouse_sgr;           /* Mode 1006 -- SGR extended coordinate framing.
                                              Was previously untracked entirely: DECSET 1006 was a
                                              no-op and DECRST had no 1006 case at all. See
                                              term/mouse.c's ghostcon_mouse_encode(), which needs
                                              this to choose SGR (unlimited coords) vs legacy X10
                                              (23-bit-clamped) framing. */
    bool             mouse_shift_capture; /* XTSHIFTESCAPE */
    ghostcon_protected_mode_t protected_mode; /* DECSCA */
    ghostcon_saved_modes_t    saved_modes;    /* CSI ? s / CSI ? r */

    /* Style management */
    ghostcon_style_set_t *styles;

    /* OSC 8 — interned hyperlink URIs. See hyperlink.h's doc comment for
       why this mirrors ghostcon_style_set_t's shape exactly. cursor's
       current hyperlink_id (above) is stamped onto each printed cell in
       print_cell(), same as style_id. */
    ghostcon_hyperlink_set_t *hyperlinks;

    /* Color palette */
    ghostcon_palette_t palette;

    /* OSC 7 — last-reported current working directory (decoded from
       a file://[host]/path URI, host discarded). Empty string if the
       shell has never sent one. */
    char cwd[1024];

    /* OSC 133/633 — shell integration semantic-prompt state. Stamped
       onto every printed cell (see print_cell() in screen.c) so the
       renderer/overlay can later distinguish prompt/input/output text
       (e.g. click-to-jump between prompts, dimming old output). */
    ghostcon_cell_semantic_t semantic_current;
    int semantic_last_exit_code; /* -1 = no command has finished yet */

    /* OSC 52 — clipboard, stub tier. Scoped to this single
       ghostcon[ttyN] instance only (no cross-VT sharing yet -- that
       needs the not-yet-built ghostcon-ipc broker). Stored verbatim as
       the base64 payload OSC 52's own wire format already uses -- no
       decode/re-encode needed since the bytes are never inspected.
       Empty string if nothing has been copied yet. */
    char clipboard[4096];

    /* Selection */
    ghostcon_selection_t selection;

    /* Kitty keyboard protocol state */
    ghostcon_kitty_state_t kitty;

    /* Damage tracking */
    ghostcon_dirty_region_t dirty;
};
typedef struct ghostcon_screen ghostcon_screen_t;

/* ------------------------------------------------------------------ */
/* Screen lifecycle                                                    */
/* ------------------------------------------------------------------ */

/* Initialize a screen with given dimensions and scrollback capacity.
   scrollback_cap is the max number of historical rows (0 = no scrollback).
   Returns true on success. */
bool ghostcon_screen_init(ghostcon_screen_t *screen,
                          uint16_t cols, uint16_t rows,
                          uint16_t scrollback_cap);

/* Destroy screen, freeing all memory */
void ghostcon_screen_deinit(ghostcon_screen_t *screen);

/* Resize screen to new dimensions. Preserves contents as much as possible. */
bool ghostcon_screen_resize(ghostcon_screen_t *screen,
                            uint16_t new_cols, uint16_t new_rows);

/* ------------------------------------------------------------------ */
/* Cursor movement                                                     */
/* ------------------------------------------------------------------ */

void ghostcon_screen_cursor_up(ghostcon_screen_t *screen, uint16_t n);
void ghostcon_screen_cursor_down(ghostcon_screen_t *screen, uint16_t n);
void ghostcon_screen_cursor_left(ghostcon_screen_t *screen, uint16_t n);
void ghostcon_screen_cursor_right(ghostcon_screen_t *screen, uint16_t n);
void ghostcon_screen_cursor_set(ghostcon_screen_t *screen, int16_t x, int16_t y);
void ghostcon_screen_cursor_horizontal_abs(ghostcon_screen_t *screen, int16_t x);
void ghostcon_screen_cursor_vertical_abs(ghostcon_screen_t *screen, int16_t y);
void ghostcon_screen_cursor_next_line(ghostcon_screen_t *screen);
void ghostcon_screen_cursor_prev_line(ghostcon_screen_t *screen);

/* Move cursor by the scroll region (for linefeed/IND/RI) */
void ghostcon_screen_cursor_scroll_up(ghostcon_screen_t *screen);
void ghostcon_screen_cursor_scroll_down(ghostcon_screen_t *screen);

/* Save/restore cursor (DECSC/DECRC) */
void ghostcon_screen_cursor_save(ghostcon_screen_t *screen);
void ghostcon_screen_cursor_restore(ghostcon_screen_t *screen);

/* ------------------------------------------------------------------ */
/* Text insertion                                                      */
/* ------------------------------------------------------------------ */

/* Insert a single character at cursor position, advancing cursor */
void ghostcon_screen_put_char(ghostcon_screen_t *screen, uint32_t codepoint);

/* Insert a string of codepoints */
void ghostcon_screen_put_text(ghostcon_screen_t *screen,
                              const uint32_t *codepoints, size_t len);

/* Carriage return */
void ghostcon_screen_carriage_return(ghostcon_screen_t *screen);

/* Linefeed (scrolls screen if at bottom) */
void ghostcon_screen_linefeed(ghostcon_screen_t *screen);

/* Reverse index (scrolls up if at top) */
void ghostcon_screen_reverse_index(ghostcon_screen_t *screen);

/* Horizontal tab */
void ghostcon_screen_tab(ghostcon_screen_t *screen);

/* Backward tab */
void ghostcon_screen_tab_back(ghostcon_screen_t *screen);

/* ------------------------------------------------------------------ */
/* Erase operations                                                    */
/* ------------------------------------------------------------------ */

#define GC_ERASE_DISPLAY_BELOW       0
#define GC_ERASE_DISPLAY_ABOVE       1
#define GC_ERASE_DISPLAY_ALL         2
#define GC_ERASE_DISPLAY_SCROLLBACK  3
#define GC_ERASE_DISPLAY_SCROLL_COMPLETE 4  /* scroll everything off screen */

#define GC_ERASE_LINE_RIGHT  0
#define GC_ERASE_LINE_LEFT   1
#define GC_ERASE_LINE_ALL    2

void ghostcon_screen_erase_display(ghostcon_screen_t *screen, int mode);
void ghostcon_screen_erase_line(ghostcon_screen_t *screen, int mode);
void ghostcon_screen_erase_chars(ghostcon_screen_t *screen, uint16_t n);
void ghostcon_screen_insert_chars(ghostcon_screen_t *screen, uint16_t n);
void ghostcon_screen_delete_chars(ghostcon_screen_t *screen, uint16_t n);

/* Selective erase (DECSED/DECSEL): like erase display/line but skips
   cells with the protected bit set. */
void ghostcon_screen_erase_display_protected(ghostcon_screen_t *screen, int mode);
void ghostcon_screen_erase_line_protected(ghostcon_screen_t *screen, int mode);

/* Save/restore a single DEC mode (CSI ? Ps s / CSI ? Ps r) */
void ghostcon_screen_save_mode(ghostcon_screen_t *screen, int mode);
void ghostcon_screen_restore_mode(ghostcon_screen_t *screen, int mode);

/* ------------------------------------------------------------------ */
/* Insert/Delete lines                                                 */
/* ------------------------------------------------------------------ */

void ghostcon_screen_insert_lines(ghostcon_screen_t *screen, uint16_t n);
void ghostcon_screen_delete_lines(ghostcon_screen_t *screen, uint16_t n);

/* ------------------------------------------------------------------ */
/* Scrolling                                                           */
/* ------------------------------------------------------------------ */

/* Scroll the visible region up/down by n lines (new lines are blank) */
void ghostcon_screen_scroll_up(ghostcon_screen_t *screen, uint16_t n);
void ghostcon_screen_scroll_down(ghostcon_screen_t *screen, uint16_t n);

/* Set scroll region (DECSTBM). top/bottom are 0-based, inclusive.
   Pass -1 for both to reset to full screen. */
void ghostcon_screen_set_scroll_region(ghostcon_screen_t *screen,
                                       int16_t top, int16_t bottom);

/* Set left/right margin region (DECSLRM). Pass -1 for both to reset. */
void ghostcon_screen_set_margin_region(ghostcon_screen_t *screen,
                                       int16_t left, int16_t right);

/* ------------------------------------------------------------------ */
/* Alternate screen                                                    */
/* ------------------------------------------------------------------ */

/* Switch to alternate screen (saves main screen grid) */
void ghostcon_screen_alt_screen_enter(ghostcon_screen_t *screen);

/* Switch back to main screen (restores saved grid) */
void ghostcon_screen_alt_screen_exit(ghostcon_screen_t *screen);

/* ------------------------------------------------------------------ */
/* Tab stops                                                           */
/* ------------------------------------------------------------------ */

void ghostcon_screen_tab_clear(ghostcon_screen_t *screen, int mode);
void ghostcon_screen_tab_set(ghostcon_screen_t *screen);

/* ------------------------------------------------------------------ */
/* Attribute/style                                                     */
/* ------------------------------------------------------------------ */

/* Set the current (cursor) style by applying an SGR attribute.
   Returns the style_id for the new cursor style. */
ghostcon_style_id_t ghostcon_screen_set_style(ghostcon_screen_t *screen,
                                              ghostcon_style_id_t style_id);

/* Apply a raw SGR attribute to a style, returning the modified style.
   This is the bridge to libghostty-vt's SGR parser output. */
ghostcon_style_t ghostcon_screen_apply_sgr_attribute(
    const ghostcon_style_t *base,
    int sgr_tag,
    const void *sgr_value);

/* ------------------------------------------------------------------ */
/* Damage tracking                                                     */
/* ------------------------------------------------------------------ */

/* Mark a single line as dirty */
void ghostcon_screen_mark_dirty(ghostcon_screen_t *screen, uint16_t y);

/* Clear all dirty flags */
void ghostcon_screen_clear_dirty(ghostcon_screen_t *screen);

/* Get the current dirty region. Returns region with y_min=-1 if nothing dirty. */
ghostcon_dirty_region_t ghostcon_screen_get_dirty(const ghostcon_screen_t *screen);

/* ------------------------------------------------------------------ */
/* Direct cell access (for renderer)                                   */
/* ------------------------------------------------------------------ */

/* Get a pointer to the cell at (x, y) in the visible grid.
   y is 0-based from the top of the visible area. */
ghostcon_cell_t *ghostcon_screen_cell(ghostcon_screen_t *screen, uint16_t x, uint16_t y);

/* Get a pointer to the row at y in the visible grid. When
   screen->view_offset > 0 (see its own doc comment), this transparently
   splices in rows from `history` instead — the sole read path the
   renderer uses (render/machine.c), so scrollback viewing needs no
   changes anywhere else in the render pipeline. */
ghostcon_row_t *ghostcon_screen_row(ghostcon_screen_t *screen, uint16_t y);

/* ------------------------------------------------------------------ */
/* Scrollback viewing (kmscon-style grab-scroll/grab-page shortcuts)   */
/* ------------------------------------------------------------------ */

/* Adjusts view_offset by `delta` lines (negative = toward live/bottom,
   positive = further back into history), clamped to [0, history_count].
   A no-op if the clamped result doesn't change view_offset. Otherwise
   marks the whole visible grid dirty (the entire viewport's content
   just changed, not any specific line) so the next render repaints it. */
void ghostcon_screen_scroll_view(ghostcon_screen_t *screen, int delta);
