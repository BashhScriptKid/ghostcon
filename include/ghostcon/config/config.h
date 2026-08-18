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
    /* Empty = unset -- ghostcon_atlas_create() falls back to
       fontconfig's own default monospace match, same as before either
       of these existed. font_family maps to FC_FAMILY; font_variant
       maps to FC_STYLE (e.g. "Bold", "Light", "Medium Italic" -- run
       `fc-list <family>` to see what a given family actually offers;
       an unrecognized value just falls through to fontconfig's normal
       substitution/default-style behavior, not an error). No
       dedicated GHOSTCON_* env var -- same as [cursor]'s fields,
       loaded directly from the config file at startup (see
       core/main.c's own doc comment on why cursor fields skip that,
       right where the initial config load happens). */
    char font_family[256];
    char font_variant[64];
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

    /* Explicit hotspot override for a BMP-format per-state override
       (BMP has no hotspot field in the format at all, unlike Xcursor,
       which already carries a real one). "x,y" in the ORIGINAL,
       uncropped asset's own pixel coordinates -- empty string (the
       default) means "no explicit override": core/main.c falls back
       to the asset's own real hotspot if it has one (Xcursor), or
       auto-centers on the cropped glyph's own bounding box otherwise
       (a much better generic guess than a fixed corner for most
       cursor shapes -- see PLAN.md's "Mouse support, pass 2" section). */
    char cursor_default_hot_pos[32];
    char cursor_link_hot_pos[32];

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

    /* Configurable copy/paste shortcuts -- see PLAN.md's "Mouse
       support, pass 2" section. Syntax matches Ghostty's own Trigger
       strings ("ctrl+shift+c"), parsed via core/input.c's
       ghostcon_parse_keybinding(). Defaults are Ghostty's own Linux
       defaults for copy_to_clipboard/paste_from_clipboard. */
    char copy_to_clipboard_binding[64];
    char paste_from_clipboard_binding[64];
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
