#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <time.h>
#include "cell.h"
#include "row.h"
#include "style.h"
#include "color.h"
#include "modes.h"
#include "screen_fwd.h"
#include "screen_cursor.h"
#include "screen_edit.h"
#include "screen_scroll.h"
#include "selection.h"
#include "kitty_keyboard.h"
#include "kitty_graphics.h"
#include "sixel.h"
#include "hyperlink.h"

/* Cursor style */
typedef enum
{
    GC_CURSOR_DEFAULT   = 0,
    GC_CURSOR_BLOCK     = 1,
    GC_CURSOR_UNDERLINE = 2,
    GC_CURSOR_BAR       = 3,
    GC_CURSOR_BLOCK_BLINK   = 4,
    GC_CURSOR_UNDERLINE_BLINK = 5,
    GC_CURSOR_BAR_BLINK     = 6,
} ghostcon_cursor_style_t;

/* Saved cursor (DECSC/DECRC) */
typedef struct
{
    int16_t x, y;
    ghostcon_cursor_style_t cursor_style;
    bool pending_wrap;
    ghostcon_style_id_t style_id;
} ghostcon_saved_cursor_t;

/* Cursor state */
typedef struct
{
    int16_t               x, y;
    ghostcon_cursor_style_t cursor_style;
    bool                  pending_wrap;  /* last-column flag */
    bool                  protected;
    ghostcon_style_id_t   style_id;       /* current text style */
    ghostcon_style_id_t   hyperlink_id;   /* current hyperlink (0 = none) */
} ghostcon_cursor_t;

/* Scroll region (DECSTBM) */
typedef struct
{
    int16_t top;    /* inclusive, 0-based */
    int16_t bottom; /* inclusive, 0-based */
} ghostcon_scroll_region_t;

/* Character protection mode (DECSCA) */
typedef enum
{
    GC_PROTECTED_OFF = 0, /* no protection */
    GC_PROTECTED_ISO,     /* ISO mode: ED/EL always respect protected cells */
    GC_PROTECTED_DEC,     /* DEC mode: only DECSED/DECSEL respect them */
} ghostcon_protected_mode_t;

/* Saved DEC modes (CSI ? ... s / CSI ? ... r, XTSAVE/XTRESTORE) */
typedef struct
{
    bool origin_mode;        /* DECOM  */
    bool auto_wrap;          /* DECAWM */
    bool reverse_video;      /* DECSCNM */
    bool insert_mode;        /* IRM */
    bool application_cursor; /* DECCKM */
    bool left_right_margin;  /* DECLRMM */
} ghostcon_saved_modes_t;

/* Margin (DECSLRM) */
typedef struct
{
    int16_t left;
    int16_t right;
} ghostcon_margin_region_t;

/* Tabstop state */
typedef struct
{
    bool *stops;      /* array of cols booleans (owned) */
    uint16_t cols;
} ghostcon_tabstops_t;

/* Accumulated dirty region for efficient damage tracking */
typedef struct
{
    int16_t y_min;  /* first dirty line, -1 if none */
    int16_t y_max;  /* last dirty line,  -1 if none */
} ghostcon_dirty_region_t;

/* Screen
 *
 * the terminal screen buffer
 *
 * Manages the grid, cursor, scrollback, alternate screen, and damage tracking.
 */
/* Tagged (not anonymous) specifically so lower-level headers that only need a
 * pointer type (e.g. selection.h's extract_text() declaration) can name it
 * without including this header
 *
 * the declaration itself lives in screen_fwd.h, which both this header and
 * selection.h include: this header includes selection.h (for the `selection`
 * field below) and selection.h needs the screen type, so the two cannot include
 * each other.
 */
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

    /* How many lines back into `history` the VIEW currently is (0 = live, showing
     * `rows`; up to history_count = fully scrolled back).
     *
     * Distinct from `scrollback_top` above, which is an internal ring-buffer
     * rotation index for `rows` itself, not a user-facing scroll position
     *
     * see ghostcon_screen_row()'s own doc comment.
     */
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
    /* CLOCK_MONOTONIC timestamp of when synchronized_output last transitioned
     * false->true
     *
     * lets core/main.c's render gate cap how long it'll withhold a frame if an
     * app sets mode 2026 and never clears it (crash, bug), instead of freezing
     * rendering forever.
     *
     * Meaningless while synchronized_output is false.
     */
    struct timespec  synchronized_output_since;
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

    /* OSC 8
     *
     * interned hyperlink URIs.
     *
     * See hyperlink.h's doc comment for why this mirrors ghostcon_style_set_t's
     * shape exactly. cursor's current hyperlink_id (above) is stamped onto each
     * printed cell in print_cell(), same as style_id.
     */
    ghostcon_hyperlink_set_t *hyperlinks;

    /* Color palette */
    ghostcon_palette_t palette;

    /* OSC 7
     *
     * last-reported current working directory (decoded from a file://[host]/path
     * URI, host discarded). Empty string if the shell has never sent one.
     */
    char cwd[1024];

    /* OSC 133/633
     *
     * shell integration semantic-prompt state.
     *
     * Stamped onto every printed cell (see print_cell() in screen.c) so the
     * renderer/overlay can later distinguish prompt/input/output text (e.g.
     * click-to-jump between prompts, dimming old output).
     */
    ghostcon_cell_semantic_t semantic_current;
    int semantic_last_exit_code; /* -1 = no command has finished yet */

    /* OSC 52
     *
     * clipboard, stub tier. Scoped to this single ghostcon[ttyN] instance only
     * (no cross-VT sharing yet
     *
     * that needs the not-yet-built ghostcon-ipc broker).
     *
     * Stored verbatim as the base64 payload OSC 52's own wire format already uses
     *
     * no decode/re-encode needed since the bytes are never inspected. Empty
     * string if nothing has been copied yet.
     */
    char clipboard[4096];

    /* Selection */
    ghostcon_selection_t selection;

    /* Kitty keyboard protocol state */
    ghostcon_kitty_state_t kitty;

    /* Kitty graphics protocol state (images + placements).
     *
     * Lives here rather than on ghostcon_stream_t so the renderer (which only
     * ever takes a ghostcon_screen_t*, see render/machine.h) can reach placements
     * without needing stream.h at all.
     *
     * Two separate, always-live instances
     *
     * primary and alt
     *
     * not one shared field, matching Ghostty's own screens.primary/
     * screens.alternate.kitty_images split.
     *
     * Found live: switching to alt screen (e.g. entering a TUI that uses 1049h)
     * left a Kitty image placed on the primary screen still rendering on top of
     * the TUI, because alt_screen_enter/_exit save and restore the cell grid
     * (alt_rows below) but a single shared kitty_graphics field has no notion of
     * "which screen this placement belongs to"
     *
     * switching screens didn't hide it, and clearing it would have permanently
     * lost it when switching back.
     *
     * Use ghostcon_screen_active_kitty_graphics() rather than reaching into
     * kitty_graphics/kitty_graphics_alt directly.
     */
    ghostcon_kitty_graphics_t kitty_graphics;
    ghostcon_kitty_graphics_t kitty_graphics_alt;

    /* Sixel graphics state (DCS q ... ST placements). Same per-screen split and
     * same reasoning as kitty_graphics/kitty_graphics_alt above
     *
     * see ghostcon_screen_active_sixel_state().
     */
    ghostcon_sixel_state_t sixel;
    ghostcon_sixel_state_t sixel_alt;

    /* Damage tracking */
    ghostcon_dirty_region_t dirty;
};

/* Screen lifecycle */

/* Initialize a screen with given dimensions and scrollback capacity.
 * scrollback_cap is the max number of historical rows (0 = no scrollback).
 * Returns true on success.
 */
extern bool ghostcon_screen_init(ghostcon_screen_t *screen, uint16_t cols, uint16_t rows, uint16_t scrollback_cap);

/* Destroy screen, freeing all memory */
extern void ghostcon_screen_deinit(ghostcon_screen_t *screen);

/* Resize screen to new dimensions. Preserves contents as much as possible. */
extern bool ghostcon_screen_resize(ghostcon_screen_t *screen, uint16_t new_cols, uint16_t new_rows);

/* RIS (ESC c)
 *
 * full terminal reset, the standard escape hatch every real terminal offers for
 * recovering from ANY stuck/confused mode or attribute state (what `reset`/`tput
 * reset` send).
 *
 * Was previously a complete no-op (stream.c's RIS handler literally did nothing,
 * and this function didn't exist despite a stray comment referencing it as if it
 * did)
 *
 * found live: a DECRST 69 bug left a stale DECSLRM margin permanently wrapping
 * every subsequent line, and `reset` couldn't clear it because RIS itself was
 * unimplemented; nothing short of restarting the whole service could recover.
 *
 * Resets: cursor position/attributes/saved-cursor, scroll and margin regions to
 * full screen, all DEC private modes to their power-on defaults, tab stops,
 * character-protection state, Kitty keyboard state, selection, and clears the
 * visible screen (scrollback history is left intact, matching xterm's own RIS
 * convention).
 *
 * Does NOT reallocate the grid or reset the color palette
 *
 * this is a state reset, not screen_deinit()+screen_init().
 *
 * Drops out of the alternate screen if active (discarding it, not restoring the
 * saved main-screen content
 *
 * RIS is a hard reset, not a graceful exit).
 */
extern void ghostcon_screen_reset(ghostcon_screen_t *screen);

/* Kitty graphics state (images + placements) for whichever screen --primary or
 * alternate
 *
 * is currently active.
 *
 * All Kitty graphics command handling and rendering should go through this rather
 * than screen->kitty_graphics directly, so images stay correctly scoped to the
 * screen they were placed on (see kitty_graphics/ kitty_graphics_alt's own doc
 * comment above).
 */
extern ghostcon_kitty_graphics_t *ghostcon_screen_active_kitty_graphics(ghostcon_screen_t *screen);

/* Sixel state for whichever screen
 *
 * primary or alternate
 *
 * is currently active. Same reasoning as ghostcon_screen_active_kitty_graphics()
 * above.
 */
extern ghostcon_sixel_state_t *ghostcon_screen_active_sixel_state(ghostcon_screen_t *screen);

/* Attribute/style */

/* Set the current (cursor) style by applying an SGR attribute. Returns the
 * style_id for the new cursor style.
 */
extern ghostcon_style_id_t ghostcon_screen_set_style(ghostcon_screen_t *screen, ghostcon_style_id_t style_id);

/* Apply a raw SGR attribute to a style, returning the modified style. This
 * is the bridge to libghostty-vt's SGR parser output.
 */
extern ghostcon_style_t ghostcon_screen_apply_sgr_attribute(const ghostcon_style_t *base, int sgr_tag, const void *sgr_value);

/* Damage tracking */

/* Mark a single line as dirty */
extern void ghostcon_screen_mark_dirty(ghostcon_screen_t *screen, uint16_t y);

/* Clear all dirty flags */
extern void ghostcon_screen_clear_dirty(ghostcon_screen_t *screen);

/* Get the current dirty region. Returns region with y_min=-1 if nothing dirty. */
extern ghostcon_dirty_region_t ghostcon_screen_get_dirty(const ghostcon_screen_t *screen);

/* Direct cell access (for renderer) */

/* Get a pointer to the cell at (x, y) in the visible grid. y is 0-based
 * from the top of the visible area.
 */
extern ghostcon_cell_t *ghostcon_screen_cell(ghostcon_screen_t *screen, uint16_t x, uint16_t y);

/* Get a pointer to the row at y in the visible grid.
 *
 * When screen->view_offset > 0 (see its own doc comment), this transparently
 * splices in rows from `history` instead
 *
 * the sole read path the renderer uses (render/machine.c), so scrollback viewing
 * needs no changes anywhere else in the render pipeline.
 */
extern ghostcon_row_t *ghostcon_screen_row(ghostcon_screen_t *screen, uint16_t y);

/* Scrollback viewing (kmscon-style grab-scroll/grab-page shortcuts) */

/* Adjusts view_offset by `delta` lines (negative = toward live/bottom,
 * positive = further back into history), clamped to [0, history_count]. A
 * no-op if the clamped result doesn't change view_offset.
 *
 * Otherwise marks the whole visible grid dirty (the entire viewport's
 * content just changed, not any specific line) so the next render repaints
 * it.
 */
extern void ghostcon_screen_scroll_view(ghostcon_screen_t *screen, int delta);

/* Returns true when a render may proceed despite mode 2026: either no
 * synchronized-output batch is open, or the open one has outlived
 * `timeout_ms`. False only while a batch is open and still within budget.
 *
 * The timeout is the guard against an app that sets ?2026h and never clears
 * it (crash, bug) freezing rendering forever. `now` is CLOCK_MONOTONIC, the
 * same clock the timestamp is stamped with.
 */
extern bool ghostcon_screen_sync_output_expired(const ghostcon_screen_t *screen, const struct timespec *now, double timeout_ms);
