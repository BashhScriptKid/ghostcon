#pragma once

#include <stdbool.h>

/* ------------------------------------------------------------------ */
/* TOML config                                                         */
/*                                                                     */
/* Startup: undead-head loads this file, then setenv()s (overwrite=false)*/
/* the corresponding GHOSTCON_* variable for each config-backed value it */
/* found, before spawning ghost-ptyserv/supervisor -- an already-set    */
/* env var (e.g. from a test harness) still wins the first time. Those  */
/* child processes inherit the environment normally, so the whole tree  */
/* picks the values up at spawn with no other file needing to parse a   */
/* config file itself for that part. Precedence at startup: explicit    */
/* env var > config file > hardcoded default.                          */
/*                                                                       */
/* Hot-reload: undead-head/supervisor/ghostcon-core EACH independently   */
/* watch GHOSTCON_CONFIG_PATH (see ghostcon_config_watch_open() below)   */
/* and re-call ghostcon_config_load() on change -- there is no reload-  */
/* propagation IPC between them; each just re-reads the same file and   */
/* applies whichever fields it individually understands. Env vars can't */
/* change under an already-running process, so this bypasses that path  */
/* entirely for live values -- see PLAN.md's "General config hot-reload"*/
/* section for why (and which keys apply live vs. only on next respawn).*/
/* ------------------------------------------------------------------ */

typedef struct {
    char drm_node[256];
    char run_dir[256];
    int  canary_deadline_ms;
    bool disable_wall;
    bool disable_kmscon_fallback;
    int  font_size;
    bool clear_on_logout;
    int  zoom_step; /* points per Ctrl+=/Ctrl+Minus press -- see
                        core/input.c's handle_zoom_shortcut() doc
                        comment for why the default (2) isn't 1 */

    /* Hardware cursor sprite -- see PLAN.md's "Cursor sprite: raster
       images, per-state, config-driven" section. Empty string = unset.
       Precedence per state: cursor_<state>_path (explicit override) >
       cursor_theme's auto-resolved asset for that state > the built-in
       procedural I-beam fallback. */
    char cursor_theme[256];
    char cursor_default_path[256];
    char cursor_link_path[256];

    /* Raster cursor scaling -- see core/main.c's load_cursor_state(),
       which resizes decoded pixels (via ghostcon_cursor_scale()) before
       handing them to ghostcon_kms_set_cursor_image(). base_scale is a
       flat multiplier applied to the asset's own decoded pixel size
       (1.0 = native size, at the reference cell height below).
       scale_with_terminal additionally scales that baseline by the
       ratio of the terminal's current cell height to a fixed reference
       cell height (CURSOR_SCALE_REFERENCE_CELL_H in main.c), so a
       raster cursor grows/shrinks with Ctrl+=/Ctrl+Minus the same way
       the procedural I-beam already does. Doesn't apply to the
       procedural fallback itself (pixels==NULL path), which is already
       always sized from the live cell height regardless of these two
       knobs. */
    float cursor_base_scale;
    bool  cursor_scale_with_terminal;
} ghostcon_config_t;

/* Fills `cfg` with the hardcoded defaults (matching what every binary
   already falls back to today when no env var is set). */
void ghostcon_config_defaults(ghostcon_config_t *cfg);

/* Loads `path`, overlaying its values onto the defaults. A missing file
   is not an error -- returns true with `cfg` left at pure defaults.
   Returns false only if the file exists but fails to parse. */
bool ghostcon_config_load(const char *path, ghostcon_config_t *cfg);

/* setenv()s GHOSTCON_* for every config-backed value in `cfg`, plus
   GHOSTCON_CONFIG_PATH itself (unconditionally, so supervisor/
   ghostcon-core know what to watch for hot-reload without duplicating
   the default-path/env-override resolution logic undead-head already
   did). `overwrite` false = startup semantics (an already-set env var
   wins, e.g. from a test harness); true = reload semantics (the file's
   new value must actually take effect). Call at startup with false,
   and again on every hot-reload with true. */
void ghostcon_config_export_env(const ghostcon_config_t *cfg, const char *config_path,
                                 bool overwrite);

/* ------------------------------------------------------------------ */
/* Hot-reload watch (inotify)                                          */
/* ------------------------------------------------------------------ */

/* Opens an inotify watch on the parent DIRECTORY of `path`, not the
   file itself -- editors that save via write-temp+rename (vim, most
   editors' default) orphan a watch placed on the file's own inode,
   which then silently never fires again. Returns the inotify fd
   (poll()-able, POLLIN when something in the directory changed), or -1
   on failure (not fatal -- caller should just skip live-reload). */
int ghostcon_config_watch_open(const char *path);

/* Call when the fd from ghostcon_config_watch_open() is POLLIN-ready.
   Drains ALL pending events (coalescing a burst from one save into a
   single reload) and returns true if any of them referred to `path`'s
   own basename specifically -- false for an unrelated file changing in
   the same directory, or a spurious wakeup. */
bool ghostcon_config_watch_check(int inotify_fd, const char *path);
