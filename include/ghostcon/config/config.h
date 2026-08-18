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
    /* "grayscale" (default), "subpixel", "cleartype", or "none" --
       maps to FreeType's rasterization target
       (FT_LOAD_TARGET_NORMAL/LCD/LCD/MONO respectively). An
       unrecognized value falls back to "grayscale", not an error.
       "subpixel" uses FreeType's LCD-optimized hinting/rasterization,
       but averages the 3 LCD subchannels down to one value -- a real,
       visibly different rasterization from plain grayscale, but not
       true per-subpixel-channel blending. "cleartype" is the real
       thing: an RGB atlas texture storing genuine distinct R/G/B
       coverage, blended per-channel in the shader -- see
       render/gles.c's FRAG_SRC doc comment. See render/atlas.c's own
       doc comment on ghostcon_atlas_create() for the exact
       FT_LOAD_TARGET mapping. */
    char antialiasing[16];
    /* "rgb" (default) or "bgr" -- only meaningful when antialiasing
       is "cleartype": the physical left-to-right subpixel order of
       the actual display. Get this wrong and cleartype produces
       visible color fringing instead of removing it. */
    char subpixel_order[8];
    bool gamma_correct; /* luminance-based glyph-edge alpha correction
                            (ported from Ghostty's cell_text.f.glsl) --
                            on by default. See render/gles.c's FRAG_SRC
                            doc comment. Applies live, no restart. */
    /* Named color-theme preset (see term/theme.h) -- e.g. "base16-dark",
       "solarized-light". Empty (default) = the built-in xterm-ish
       defaults ghostcon_palette_init() already sets. An unrecognized
       name falls back to those same built-in defaults, not an error.
       Applied at startup and live on reload; see the [colors] table
       below for explicit per-color overrides applied ON TOP of
       whichever theme (or lack of one) is active. */
    char theme[32];

    /* Explicit per-color overrides, applied after `theme` above --
       same preset-then-explicit-override layering [cursor]'s fields
       already use. Empty string (the default) = "don't override this
       one, leave whatever the theme/built-in default set." Accepts
       any format ghostcon_color_parse_spec() understands (#RGB,
       #RRGGBB, #RRRGGGBBB, #RRRRGGGGBBBB, or X11 "rgb:R/G/B"); an
       unparseable value is ignored, not an error, same as an
       unrecognized `theme` name. color[0..15] are the ANSI palette
       slots in the standard order (0=black, 1=red, ... 7=white,
       8=bright black, ... 15=bright white). */
    char color_background[16];
    char color_foreground[16];
    char color_cursor[16];
    char color[16][16];

    bool clear_on_logout;
    int  zoom_step; /* points per Ctrl+=/Ctrl+Minus press -- see
                        core/input.c's handle_zoom_shortcut() doc
                        comment for why the default (2) isn't 1 */
    int  scrollback_lines; /* history ring buffer capacity passed to
                               ghostcon_term_init() -- was hardcoded to
                               2000 with no way to change it before
                               this key existed. Startup-only (not
                               hot-reloadable): resizing scrollback
                               live isn't something ghostcon_screen_t
                               supports, unlike the grid resize a font
                               zoom already does. */
    /* Key auto-repeat timing, milliseconds -- was hardcoded (250/50,
       kmscon's own documented defaults) with no way to change it
       before these existed. Startup-only, same reasoning as
       scrollback_lines: the repeat timerfd is armed once at
       ghostcon_input_open() time, not something a live reload
       re-arms. */
    int  repeat_delay_ms;
    int  repeat_rate_ms;

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

    /* [mouse] / [touchpad] -- per-device-class libinput tuning,
       applied to every currently-connected device of that class (via
       libinput's own tap-finger-count check, the library's own
       recommended way to distinguish a touchpad from a mouse) and to
       each newly-hotplugged one as it's discovered. See
       core/input.c's apply_device_config() for the libinput API each
       field maps to.
       - enable: false fully disables the device (no movement, clicks,
         or scroll at all) via LIBINPUT_CONFIG_SEND_EVENTS_DISABLED.
       - sensitivity: libinput's own pointer-acceleration speed knob,
         -1.0 (slowest) .. 1.0 (fastest), 0.0 = libinput's own default.
       - scroll_speed: NOT a libinput API -- libinput has no such
         knob, so this is implemented entirely in core/input.c as a
         fractional accumulator over the terminal-scroll "ticks" a raw
         scroll event would otherwise produce 1:1: add scroll_speed to
         a running accumulator per raw tick, fire one terminal-scroll
         tick and subtract 1.0 each time it crosses 1.0. 1.0 (default)
         is unchanged passthrough; > 1.0 amplifies (multiple ticks per
         raw event); < 1.0 throttles (multiple raw events needed
         before one tick reaches the app). */
    bool  mouse_enable;
    float mouse_scroll_speed;
    float mouse_sensitivity;

    bool  touchpad_enable;
    float touchpad_scroll_speed;
    bool  touchpad_tap_to_click;
    bool  touchpad_natural_scroll;
    float touchpad_sensitivity;
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
