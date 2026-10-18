/*
 * ghostcon-core — per-VT renderer entry point.
 *
 * Ties together every module built so far into the real event loop
 * (see IMPLEMENTATION.md "Renderer Architecture" pipeline and PLAN.md's
 * "Architecture overview"): vtctl owns the VT_PROCESS acquire/release
 * handshake, kms+egl+gles+atlas+machine render, transport talks to the
 * pty child, input captures the keyboard, diag reports crashes, term
 * holds the actual terminal state.
 *
 * The one genuinely new piece here (not exercised by any earlier
 * scratch harness, which only ever handled a single startup-activate-
 * then-exit cycle) is the repeated acquire/release lifecycle: a real
 * desktop user switches VTs back and forth many times over a session,
 * and this process must survive that indefinitely, not just once.
 * Display resources (DRM master, kms/egl/gles) are torn down on
 * release and rebuilt on reacquire; terminal state, the font atlas,
 * and the input/transport connections all persist across the cycle —
 * a VT switch away and back must not lose scrollback or reconnect the
 * shell.
 *
 * No TOML config yet (PLAN.md's own step list defers this past Phase
 * 1's early items — ghost-ptyserv already set this precedent). VT
 * number and DRM node come from argv; the canary fd (if a supervisor
 * is running this) comes from GHOSTCON_CANARY_FD.
 *
 * Usage: ghostcon-core <vtnum> [drm_node] [registry_socket]
 */

#define _DEFAULT_SOURCE

#include "ghostcon/config/config.h"
#include "ghostcon/core/diag.h"
#include "ghostcon/core/display.h"
#include "ghostcon/core/input.h"
#include "ghostcon/core/log.h"
#include "ghostcon/core/transport.h"
#include "ghostcon/core/ui.h"
#include "ghostcon/core/vtctl.h"
#include "ghostcon/ptyserv/protocol.h"
#include "ghostcon/render/atlas.h"
#include "ghostcon/render/machine.h"
#include "ghostcon/term/term.h"
#include "ghostcon/term/dump.h"
#include "ghostcon/term/theme.h"
#include "ghostcon/term/color.h"
#include <time.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <unistd.h>

/* This dev machine's GPU is card1, not the more common card0 -- no
   config file exists yet to make this a real per-machine setting (see
   supervisor/main.c's GHOSTCON_DRM_NODE for the override path once one
   does), and `pkexec env VAR=val CMD` turned out not to reliably show
   its auth prompt in practice, so hardcoding the value that's actually
   correct here beats depending on that. */
#define DEFAULT_DRM_NODE "/dev/dri/card1"
#define FONT_SIZE 16
/* Default true, unlike every other GHOSTCON_* boolean flag in this tree
   (which are all off-by-default, presence-means-on) -- see config.c's
   ghostcon_config_export_env() for why: undead-head only setenv()s this
   when the config explicitly disables it, so absence here means "use
   the true default", not "off". */
#define CLEAR_ON_LOGOUT_DEFAULT true
#define ATLAS_DIM 1024
#define ZOOM_MIN_FONT_SIZE 6  /* below this, glyphs are unreadable anyway */
#define ZOOM_MAX_FONT_SIZE 96 /* well within ATLAS_DIM's headroom for a monospace face */
/* Matches config.c's own default -- see core/input.c's
   handle_zoom_shortcut() doc comment for why 1pt wasn't enough. */
#define ZOOM_STEP_DEFAULT 2
#define POLL_INTERVAL_MS 1000 /* canary heartbeat cadence when otherwise idle */
#define SYNC_OUTPUT_TIMEOUT_MS 200.0 /* safety net -- see render gate's doc comment on mode 2026 */

typedef struct {
    int vt_num;
    const char *drm_node;
    int canary_fd;
    bool clear_on_logout;
    /* Hot-reload -- see PLAN.md's "General config hot-reload" section.
       config_path is what config_watch_fd (-1 if unavailable) watches;
       font_size is tracked here (not just derived from the atlas each
       time) purely to detect "did this actually change" cheaply on
       reload without re-deriving it from the atlas first. */
    const char *config_path;
    int config_watch_fd;
    int font_size;
    int zoom_step; /* points per Ctrl+=/Ctrl+Minus press, config-driven */

    /* [general] font_family/font_variant -- see config.h's own doc
       comment. Persisted here (not just used transiently at load
       time) for the same reason cursor_theme etc. below are: both
       apply_font_size() (zoom) and apply_config_reload() need to
       re-call ghostcon_atlas_create() with these, without re-reading
       config just for that. */
    char font_family[256];
    char font_variant[64];
    char antialiasing[16];
    char subpixel_order[8]; /* "rgb" (default) or "bgr" -- only meaningful
                                when antialiasing == "cleartype" */
    bool gamma_correct;
    bool ligatures;

    /* Cursor sprite config -- see PLAN.md's "Cursor sprite: raster
       images, per-state, config-driven" section. Persisted here (not
       just used transiently at load time) so ghostcon_display_reload_cursor()
       can be re-run from apply_font_size() (new fallback_font_height,
       same configured paths) without re-reading config. */
    /* [cursor] config -- persisted here (not just pushed into the
       display backend transiently at load time) so apply_config_reload()
       can diff old vs new before touching the live cursor plane, and so
       push_cursor_config() can be re-run whenever needed without
       re-reading config just for that. See display.h's
       ghostcon_display_cursor_config_t for the fields' meaning. */
    char cursor_theme[256];
    char cursor_default_path[256];
    char cursor_link_path[256];
    float cursor_base_scale;
    bool cursor_scale_with_terminal;

    /* Configurable copy/paste shortcuts -- persisted here (not just
       applied transiently at load time) so acquire_display() can
       re-push them into a freshly-opened ghostcon_input_t after every
       VT release/reacquire cycle (a fresh input context always starts
       back at ghostcon_input_open()'s own hardcoded defaults otherwise,
       silently reverting a user's configured rebind until the next
       unrelated config edit happens to re-trigger apply_config_reload()). */
    char copy_to_clipboard_binding[64];
    char paste_from_clipboard_binding[64];

    /* [general] theme / [colors] -- persisted so apply_config_reload()
       can tell whether any of it actually changed since the last load
       (same reasoning font_config_changed exists for font_family/
       font_variant/antialiasing) before touching the live palette --
       a running program can customize colors at runtime via OSC 4,
       and an unrelated config edit re-triggering a reload must NOT
       silently clobber that just because reload happened to run. */
    char theme[32];
    char color_background[16];
    char color_foreground[16];
    char color_cursor[16];
    char color[16][16];

    /* [mouse]/[touchpad] config -- see config.h's own doc comment.
       Persisted here for the same reason the keybinding fields above
       are: apply_mouse_touchpad_config() needs to re-push these into
       a fresh app->input after every ghostcon_input_open() (a new
       input context always starts back at that function's own
       hardcoded defaults), not just once at initial config load. */
    bool  mouse_enable, touchpad_enable;
    float mouse_scroll_speed, touchpad_scroll_speed;
    float mouse_sensitivity, touchpad_sensitivity;
    bool  touchpad_tap_to_click, touchpad_natural_scroll;

    /* [general] repeat_delay_ms/repeat_rate_ms -- see
       ghostcon_input_open()'s own doc comment on why these are
       "per-acquire-cycle, not hot-reloadable in place" rather than
       persisted+re-pushed like mouse/touchpad above. */
    int repeat_delay_ms, repeat_rate_ms;

    /* Explicit BMP hotspot overrides -- see config.h's own doc comment
       on cursor_default_hot_pos/cursor_link_hot_pos. */
    char cursor_default_hot_pos[32];
    char cursor_link_hot_pos[32];

    /* argv[0]'s original address/length, captured once in main() before
       anything overwrites it -- OSC 0/2 process-identity repurposing
       (see term_title_callback) needs to know how many bytes it's safe
       to write without corrupting adjacent argv/environ strings. */
    char  *argv0;
    size_t argv0_len;

    ghostcon_vtctl_t *vt;

    /* Long-lived: created once, survive every acquire/release cycle. */
    ghostcon_atlas_t *atlas;
    int cell_w, cell_h;
    ghostcon_term_t term;
    ghostcon_transport_t transport;

    /* Per-acquire-cycle: the display backend (KMS/GBM/EGL/GLES + DRM
       master) inside it is fully torn down on release and rebuilt on
       reacquire; the ghostcon_display_t handle itself persists. */
    ghostcon_display_t *disp;
    /* input joined this category, not the long-lived one above, after a
       real leak found live: libinput keeps queuing events into this
       process's own open fd at the KERNEL level the entire time the VT
       is inactive, backpressure or not -- simply not polling/dispatching
       it while inactive (tried first) only delays consumption, it
       doesn't discard the backlog. The whole multi-minute backlog (any
       keystrokes typed anywhere else on the machine while this VT
       wasn't focused) gets replayed in one burst the instant dispatch
       resumes, indistinguishable from real input typed on this VT.
       Closing the libinput context on release and opening a fresh one
       on reacquire (same as kms/egl/gles below) discards that backlog
       for free, since a brand-new context has no history. */
    ghostcon_input_t *input;

    /* Set when a render was skipped because the app had a synchronized-
       output batch (mode 2026) open -- see the render-gate site below.
       Not per-acquire-cycle state (survives release/reacquire trivially
       since it's just "is a render owed", not tied to any GPU handle). */
    bool render_deferred;
    bool screenshot_requested; /* Ctrl+Alt+D also asks render_frame() to
                                   glReadPixels the next frame it draws --
                                   see ghostcon_gles_screenshot_ppm()'s doc
                                   comment for why it must happen inside
                                   render_frame(), not from the input
                                   handler that sets this flag. */
} app_t;

/* Copies the app's persisted [cursor] config fields into the display
   backend's own struct and pushes them in -- called after every config
   load/reload (the backend re-resolves and re-uploads the sprites
   itself, including lazily on the next acquire if the plane doesn't
   exist yet). */
static void
push_cursor_config(app_t *app)
{
    ghostcon_display_cursor_config_t cfg = {
        .base_scale = app->cursor_base_scale,
        .scale_with_terminal = app->cursor_scale_with_terminal,
    };
    snprintf(cfg.theme, sizeof(cfg.theme), "%s", app->cursor_theme);
    snprintf(cfg.default_path, sizeof(cfg.default_path), "%s", app->cursor_default_path);
    snprintf(cfg.link_path, sizeof(cfg.link_path), "%s", app->cursor_link_path);
    snprintf(cfg.default_hot_pos, sizeof(cfg.default_hot_pos), "%s", app->cursor_default_hot_pos);
    snprintf(cfg.link_hot_pos, sizeof(cfg.link_hot_pos), "%s", app->cursor_link_hot_pos);
    ghostcon_display_configure_cursor(app->disp, &cfg, app->cell_h);
}

/* Parses app->copy_to_clipboard_binding/paste_from_clipboard_binding
   and pushes them into app->input -- called after every
   ghostcon_input_open() (a fresh input context always starts back at
   that function's own hardcoded defaults otherwise) and from
   apply_config_reload() (see app_t's own doc comment on these two
   fields for why they're persisted rather than applied transiently).
   A no-op if app->input is NULL (not yet acquired) or either spec
   fails to parse (keeps whatever the input context already has --
   its own built-in defaults on first open, or the last-known-good
   parse on a reload). */
static void
apply_keybindings(app_t *app)
{
    if (!app->input)
        return;
    GhosttyMods copy_mods, paste_mods;
    uint32_t copy_evdev, paste_evdev;
    if (ghostcon_parse_keybinding(app->copy_to_clipboard_binding, &copy_mods, &copy_evdev) &&
        ghostcon_parse_keybinding(app->paste_from_clipboard_binding, &paste_mods, &paste_evdev)) {
        ghostcon_input_set_clipboard_bindings(app->input, copy_mods, copy_evdev,
                                               paste_mods, paste_evdev);
    } else {
        fprintf(stderr, "ghostcon-core: invalid [keybindings] entry -- keeping previous bindings\n");
    }
}

/* Same "called after every ghostcon_input_open() and again on every
   config reload" convention as apply_keybindings() right above, for
   [mouse]/[touchpad] instead. */
static void
apply_mouse_touchpad_config(app_t *app)
{
    if (!app->input)
        return;
    ghostcon_input_set_mouse_config(app->input, app->mouse_enable,
                                     app->mouse_scroll_speed, app->mouse_sensitivity);
    ghostcon_input_set_touchpad_config(app->input, app->touchpad_enable,
                                        app->touchpad_scroll_speed, app->touchpad_tap_to_click,
                                        app->touchpad_natural_scroll, app->touchpad_sensitivity);
}

/* Applies app->theme/color_* to the live screen palette: reset to the
   built-in ANSI defaults, layer the named theme preset (if any) on
   top, then layer any explicit per-color overrides on top of THAT --
   same preset-then-explicit-override precedence [cursor]'s fields
   already use. Only ever called when apply_config_reload() (or the
   initial startup load) detects theme/color config actually changed
   -- see app_t's own doc comment on these fields for why an unrelated
   reload must not call this unconditionally. Only resets the 16 ANSI
   + fg/bg/cursor slots (ghostcon_palette_init() also rebuilds the
   216-color cube/grayscale ramp, but those are deterministic/
   derived, not part of any theme's identity or something OSC 4 would
   plausibly have customized, so touching them here is harmless). */
static void
apply_theme_config(app_t *app)
{
    ghostcon_palette_t *pal = &app->term.screen.palette;
    ghostcon_palette_init(pal);
    ghostcon_theme_apply(pal, app->theme);

    GhosttyColorRgb rgb;
    if (app->color_background[0] && ghostcon_color_parse_spec(app->color_background, &rgb))
        ghostcon_palette_set_default_bg(pal, rgb);
    if (app->color_foreground[0] && ghostcon_color_parse_spec(app->color_foreground, &rgb))
        ghostcon_palette_set_default_fg(pal, rgb);
    if (app->color_cursor[0] && ghostcon_color_parse_spec(app->color_cursor, &rgb))
        ghostcon_palette_set_cursor(pal, rgb);
    for (int i = 0; i < 16; i++) {
        if (app->color[i][0] && ghostcon_color_parse_spec(app->color[i], &rgb))
            ghostcon_palette_set(pal, i, rgb);
    }

    /* Every cell's resolved color potentially changed -- full redraw. */
    app->term.screen.dirty.y_min = 0;
    app->term.screen.dirty.y_max = (int16_t)(app->term.screen.rows_visible - 1);
}

/* Defensive check against the kernel's OWN idea of which VT is
   currently foreground, independent of this process's own tracked
   display_acquired/have_master state -- found live: libinput's udev
   backend reads raw evdev events directly, completely independent of
   which VT the kernel console layer currently considers foreground
   (the same reasoning is_vt_switch_combo()'s own doc comment already
   covers for Ctrl+Alt+Fn specifically), so if this process's own
   VT_PROCESS release signal ever gets missed (leaving its internal
   state stuck "acquired" even though a different VT is what's
   actually visible -- exactly what a confused/desynced VT-switch
   state, e.g. from heavy manual chvt/session churn, can cause), it
   would otherwise keep acting on every keystroke typed ANYWHERE on
   the physical keyboard. This is a safety net for that specific
   failure mode, not the primary "don't process input while inactive"
   mechanism (that's already handled by app->input simply not existing
   outside an acquire/release cycle). Fails OPEN (returns true) on any
   read error -- if /sys/class/tty/tty0/active can't be read for some
   reason, that's not grounds to silently stop responding to input in
   the normal case. */
static bool
is_vt_foreground(int vt_num)
{
    FILE *f = fopen("/sys/class/tty/tty0/active", "r");
    if (!f)
        return true;
    char buf[32] = {0};
    bool read_ok = fgets(buf, sizeof(buf), f) != NULL;
    fclose(f);
    if (!read_ok)
        return true;
    int active_vt = 0;
    if (sscanf(buf, "tty%d", &active_vt) != 1)
        return true;
    return active_vt == vt_num;
}

static void
release_display(app_t *app)
{
    if (!ghostcon_display_is_acquired(app->disp))
        return;

    /* Tears down GLES/EGL/KMS and drops DRM master synchronously, in
       vtctl_pre_ack()'s critical path order (drop master BEFORE the
       VT_RELDISP ack reaches the kernel -- see that function's doc
       comment below for why a late drop once hung VT-switching whole).
       Deliberately does NOT close the display backend's internal DRM
       fd itself (measured 70-80ms+ synchronously; see display.c's
       release for why the fd stays open-but-unmarked-until-reacquire).
       Idempotent: no-op if nothing is acquired. */
    ghostcon_display_release(app->disp);
    GC_LOG(vt, "released");

    if (app->input)
        ghostcon_input_close(app->input);
    app->input = NULL;

    /* A selection left mid-drag (pending) when the VT is released would
       never see its release event -- the libinput context (and its
       pressed_buttons state) is discarded here and rebuilt fresh on
       reacquire, but screen->selection lives outside ghostcon_input_t
       and survives untouched, so without this it would get stuck
       pending forever. Clearing rather than finishing: there's no
       release-in-visible-app-content event to treat as a real
       finish. */
    ghostcon_selection_clear(&app->term.screen.selection);
}

static bool
acquire_display(app_t *app)
{
    /* Opens the DRM node, takes master (with retry), initializes
       KMS/GBM/EGL/GLES, replays the font atlas into the fresh GPU
       texture (forced -- a freshly-created GLES context has an empty
       texture regardless of CPU-side atlas dirty state; see gles.h),
       and uploads cursor sprites. Returns dimensions via out params. */
    uint32_t width = 0, height = 0;
    GC_LOG(vt, "acquire request");
    if (!ghostcon_display_acquire(app->disp, app->cell_h, app->atlas, &width, &height)) {
        GC_LOG(vt, "acquire FAILED");
        return false;
    }

    /* Fresh input context every acquire -- see app_t's own doc comment
       on `input` for why this must not be reused across a release/
       reacquire cycle (kernel-level event backlog, not a polling
       issue). Not fatal if it fails: continue without keyboard input,
       matching main()'s own original startup behavior. */
    app->input = ghostcon_input_open("seat0", (int)width, (int)height,
                                      app->repeat_delay_ms, app->repeat_rate_ms);
    if (!app->input)
        fprintf(stderr, "ghostcon-core: input_open failed -- continuing without keyboard input\n");
    apply_keybindings(app); /* fresh input context otherwise reverts to hardcoded defaults */
    apply_mouse_touchpad_config(app); /* same reasoning -- see its own doc comment */

    /* Re-assert the pty's window size on EVERY acquire, not just once
       at initial connect (see the one-shot call right after
       ghostcon_transport_connect() in main(), which only covers the
       very first acquire -- before this connection even exists on a
       later reacquire, so that call can't run again here). This makes
       resize idempotent/self-healing: ghost-ptyserv/pty-ttyN are a
       long-lived, persistent pty pair by design (survives ghostcon-
       core restarts, for VT-switch/ring-buffer session continuity),
       so the pty's kernel-tracked size is mutable shared state that
       can drift out of sync with app.term's own correct size if
       ANYTHING clobbers a resize message in between (found live: a
       burst of spurious zoom-out events right after a restart left
       the pty stuck at a much-older, smaller font size's dimensions,
       with nothing to correct it afterward -- a plain resize-as-
       notification design has no way to recover from that; resending
       it here on every reacquire does, at the next VT switch). A
       harmless no-op before the transport is connected yet (ctl_fd
       still -1, resize's own "best-effort, not an error" early
       return) -- the existing one-shot call covers that exact case
       instead. */
    ghostcon_transport_resize(&app->transport, app->term.rows, app->term.cols,
                              app->term.cols * app->cell_w, app->term.rows * app->cell_h);

    return true;
}

/* Routes terminal-generated responses (DSR/DA/DECRPM, OSC color query
   replies, ...) back to the pty child, exactly like a real keystroke
   would be -- the application on the other end can't tell the
   difference. Was never wired up before this, meaning e.g. cursor-
   position queries (DSR 6) got no response at all; OSC color queries
   (ESC]4;n;?ESC\ and friends) need this same path. */
static void
term_output_callback(void *userdata, const uint8_t *data, size_t len)
{
    app_t *app = userdata;
    ghostcon_transport_write(&app->transport, data, len);
}

/* OSC 0/2 (window/icon title) is repurposed as process identity — there's
   no window chrome on a bare TTY to show a title bar in, but `ps`/`top`
   showing N identical "ghostcon-core" lines across N VTs is exactly the
   debugging pain that originally motivated this project (see PLAN.md's
   "Process identity" section). */
static void
term_title_callback(void *userdata, const char *title)
{
    app_t *app = userdata;

    /* argv[0]: verbatim "ghostcon@tty<N> -- " + title, visible via
       `ps -ef`/`/proc/<pid>/cmdline`. Truncated to argv[0]'s original
       length rather than relocated via prctl(PR_SET_MM_ARG_START/END)
       — that call requires CAP_SYS_RESOURCE and correct mm_struct
       bookkeeping to avoid corrupting the process's own memory map;
       PLAN.md explicitly calls out either choice as acceptable as long
       as it's deliberate, and truncation is the safe one. */
    if (app->argv0 && app->argv0_len > 0) {
        char full[256];
        int n = snprintf(full, sizeof(full), "ghostcon@tty%d -- %s", app->vt_num, title);
        if (n > 0) {
            size_t copy_len = (size_t)n < app->argv0_len ? (size_t)n : app->argv0_len;
            memcpy(app->argv0, full, copy_len);
            memset(app->argv0 + copy_len, 0, app->argv0_len - copy_len);
        }
    }

    /* PR_SET_NAME (the `comm` field): fixed-width, truncated form for
       top/htop. */
    char token[64];
    ghostcon_ui_extract_command_token(title, token, sizeof(token));
    char comm[16];
    ghostcon_ui_build_comm_name(app->vt_num, token, comm);
    prctl(PR_SET_NAME, comm, 0, 0, 0);
}

/* OSC 9/777 (desktop notifications) -- stub tier, no ghostcon-ipc broker
   exists yet (PLAN.md's own deferred item). Logged to journald via the
   existing diag path rather than silently discarded, so a notification
   an application sent is at least visible somewhere. */
static void
term_notify_callback(void *userdata, const char *message)
{
    app_t *app = userdata;
    ghostcon_diag_log_warning("vt %d: notification: %s", app->vt_num, message);
}

/* ghostcon_vtctl_process_pending()'s pre_ack hook (see vtctl.h's own
   doc comment on ghostcon_vtctl_pre_ack_fn): on a release, this MUST
   drop DRM master before the VT_RELDISP ack goes out, or the kernel
   tells whoever's switching in (e.g. the desktop's compositor) that
   it's safe to take over while ghostcon-core still holds master --
   found live: paging back to the desktop hung on a black screen until
   forced through an unrelated VT, because the ack used to fire before
   release_display() (which drops master) ever ran. release_display()
   is itself idempotent (early-returns if nothing's acquired), so
   calling it here and then again from the normal post-process_pending
   bookkeeping below is safe, not a double-teardown. */
static void
vtctl_pre_ack(char event, void *userdata)
{
    if (event == 'R')
        release_display((app_t *)userdata);
}

/* Shared by config hot-reload and the Ctrl+=/Ctrl+- zoom shortcut
   below -- rebuilds the glyph atlas at `new_size` (using app->font_family/
   font_variant, which the caller must already have updated to whatever
   it wants baked into the rebuild) and resizes the terminal grid to
   match, reusing the exact cols/rows-from-kms-dimensions/term_resize/
   transport_resize sequence acquire_display()'s own reacquire path
   already uses. `force` rebuilds even if new_size == app->font_size --
   config hot-reload needs this for a font_family/font_variant-only
   edit, where the size genuinely didn't change but the atlas still
   needs to be recreated against the new font; the zoom shortcut
   always passes false, since a zoom keypress by definition means the
   size changed. Returns true if it actually did something (caller
   should render); false for a no-op (size unchanged and not forced,
   or invalid) or a failed rebuild (logged, old atlas left in place). */
static bool
apply_font_size(app_t *app, int new_size, bool force)
{
    if (new_size <= 0)
        return false;
    if (!force && new_size == app->font_size)
        return false;

    /* Create the new atlas FIRST -- only destroy the old one and swap
       pointers if creation actually succeeded, so a failed rebuild
       (e.g. the requested size doesn't fit ATLAS_DIM) can't leave
       app->atlas NULL and every subsequent render crashing on a null
       deref. */
    ghostcon_atlas_t *new_atlas = ghostcon_atlas_create(app->font_family[0] ? app->font_family : NULL,
                                                          app->font_variant, app->antialiasing,
                                                          app->subpixel_order,
                                                          new_size, ATLAS_DIM);
    if (!new_atlas) {
        fprintf(stderr, "ghostcon-core: font_size change to %d failed, keeping %d\n",
                new_size, app->font_size);
        return false;
    }

    ghostcon_atlas_destroy(app->atlas);
    app->atlas = new_atlas;
    app->font_size = new_size;
    ghostcon_atlas_cell_size(app->atlas, &app->cell_w, &app->cell_h);
    ghostcon_stream_set_cell_size(&app->term.stream, app->cell_w, app->cell_h);

    if (ghostcon_display_is_acquired(app->disp)) {
        uint16_t new_cols = (uint16_t)(ghostcon_display_width(app->disp) / (uint32_t)app->cell_w);
        uint16_t new_rows = (uint16_t)(ghostcon_display_height(app->disp) / (uint32_t)app->cell_h);
        if (!ghostcon_term_resize(&app->term, new_cols, new_rows))
            fprintf(stderr, "ghostcon-core: term_resize after font_size change failed\n");
        else
            ghostcon_transport_resize(&app->transport, new_rows, new_cols,
                                      new_cols * app->cell_w, new_rows * app->cell_h);

        /* A selection recorded against the OLD grid width can point
           past the new one's bounds (ghostcon_selection_t.cols exists
           precisely to detect this) -- clear rather than try to remap
           it, matching how selection is already dropped wholesale on a
           VT release (see release_display()'s own doc comment). */
        if (app->term.screen.selection.active &&
            app->term.screen.selection.cols != (int16_t)app->term.screen.cols)
            ghostcon_selection_clear(&app->term.screen.selection);
        /* If the VT isn't currently acquired, cell_w/cell_h are already
           updated above -- acquire_display()'s own reacquire path
           recomputes cols/rows from them on the next acquire, no
           special-casing needed here for that case. */

        /* Rescale the hardware cursor too, matching kmscon's own
           refresh_hw_cursor() being called on every font/resize change
           (src/terminal.c) -- a procedural-fallback I-beam sized for
           the old font would look visibly wrong (too small/large) at
           the new one. Re-resolves configured raster assets too, not
           just the fallback -- redundant work for a state that has one
           (same image gets redecoded/reuploaded), but simplest given
           this only runs on a zoom keypress, not a hot path. */
        ghostcon_display_reload_cursor(app->disp, app->cell_h);
    }
    return true;
}

static bool
apply_config_reload(app_t *app)
{
    ghostcon_config_t new_cfg;
    if (!ghostcon_config_load(app->config_path, &new_cfg)) {
        fprintf(stderr, "ghostcon-core: %s changed but failed to parse -- keeping previous values\n",
                app->config_path);
        return false;
    }

    app->clear_on_logout = new_cfg.clear_on_logout;
    if (new_cfg.zoom_step > 0)
        app->zoom_step = new_cfg.zoom_step;

    /* A family/variant-only edit (font_size unchanged) still needs an
       atlas rebuild -- apply_font_size()'s own early-return would
       otherwise treat this as a no-op. Update app's copies BEFORE
       calling it, since that's what the rebuild actually reads. */
    bool font_config_changed =
        strcmp(app->font_family, new_cfg.font_family) != 0 ||
        strcmp(app->font_variant, new_cfg.font_variant) != 0 ||
        strcmp(app->antialiasing, new_cfg.antialiasing) != 0 ||
        strcmp(app->subpixel_order, new_cfg.subpixel_order) != 0;
    snprintf(app->font_family, sizeof(app->font_family), "%s", new_cfg.font_family);
    snprintf(app->font_variant, sizeof(app->font_variant), "%s", new_cfg.font_variant);
    snprintf(app->antialiasing, sizeof(app->antialiasing), "%s", new_cfg.antialiasing);
    snprintf(app->subpixel_order, sizeof(app->subpixel_order), "%s", new_cfg.subpixel_order);
    bool need_render = apply_font_size(app, new_cfg.font_size, font_config_changed);

    if (app->gamma_correct != new_cfg.gamma_correct) {
        app->gamma_correct = new_cfg.gamma_correct;
        ghostcon_display_set_gamma_correct(app->disp, app->gamma_correct);
        need_render = true;
    }

    if (app->ligatures != new_cfg.ligatures) {
        app->ligatures = new_cfg.ligatures;
        need_render = true;
    }

    bool theme_changed =
        strcmp(app->theme, new_cfg.theme) != 0 ||
        strcmp(app->color_background, new_cfg.color_background) != 0 ||
        strcmp(app->color_foreground, new_cfg.color_foreground) != 0 ||
        strcmp(app->color_cursor, new_cfg.color_cursor) != 0;
    for (int i = 0; !theme_changed && i < 16; i++)
        theme_changed = strcmp(app->color[i], new_cfg.color[i]) != 0;
    snprintf(app->theme, sizeof(app->theme), "%s", new_cfg.theme);
    snprintf(app->color_background, sizeof(app->color_background), "%s", new_cfg.color_background);
    snprintf(app->color_foreground, sizeof(app->color_foreground), "%s", new_cfg.color_foreground);
    snprintf(app->color_cursor, sizeof(app->color_cursor), "%s", new_cfg.color_cursor);
    for (int i = 0; i < 16; i++)
        snprintf(app->color[i], sizeof(app->color[i]), "%s", new_cfg.color[i]);
    if (theme_changed) {
        apply_theme_config(app);
        need_render = true;
    }

    /* apply_font_size() above already calls reload_cursor_images() when
       font_size actually changed -- but if ONLY the [cursor] section
       changed (the common case: font_size usually doesn't move on the
       same edit), that early-returns before ever reaching it. Always
       re-copy the cursor config and reload here too; redundant (one
       extra decode/reupload) on the rare edit that changes both, not
       incorrect. */
    snprintf(app->cursor_theme, sizeof(app->cursor_theme), "%s", new_cfg.cursor_theme);
    snprintf(app->cursor_default_path, sizeof(app->cursor_default_path), "%s", new_cfg.cursor_default_path);
    snprintf(app->cursor_link_path, sizeof(app->cursor_link_path), "%s", new_cfg.cursor_link_path);
    app->cursor_base_scale = new_cfg.cursor_base_scale;
    app->cursor_scale_with_terminal = new_cfg.cursor_scale_with_terminal;
    snprintf(app->cursor_default_hot_pos, sizeof(app->cursor_default_hot_pos), "%s", new_cfg.cursor_default_hot_pos);
    snprintf(app->cursor_link_hot_pos, sizeof(app->cursor_link_hot_pos), "%s", new_cfg.cursor_link_hot_pos);
    push_cursor_config(app);

    snprintf(app->copy_to_clipboard_binding, sizeof(app->copy_to_clipboard_binding),
             "%s", new_cfg.copy_to_clipboard_binding);
    snprintf(app->paste_from_clipboard_binding, sizeof(app->paste_from_clipboard_binding),
             "%s", new_cfg.paste_from_clipboard_binding);
    apply_keybindings(app);

    app->mouse_enable = new_cfg.mouse_enable;
    app->mouse_scroll_speed = new_cfg.mouse_scroll_speed;
    app->mouse_sensitivity = new_cfg.mouse_sensitivity;
    app->touchpad_enable = new_cfg.touchpad_enable;
    app->touchpad_scroll_speed = new_cfg.touchpad_scroll_speed;
    app->touchpad_tap_to_click = new_cfg.touchpad_tap_to_click;
    app->touchpad_natural_scroll = new_cfg.touchpad_natural_scroll;
    app->touchpad_sensitivity = new_cfg.touchpad_sensitivity;
    /* Not re-armed immediately -- see ghostcon_input_open()'s own doc
       comment. Persisted here so the NEXT VT release/reacquire (which
       rebuilds the input context from scratch regardless) picks up
       the new value, rather than reverting to whatever was live at
       process startup. */
    if (new_cfg.repeat_delay_ms > 0)
        app->repeat_delay_ms = new_cfg.repeat_delay_ms;
    if (new_cfg.repeat_rate_ms > 0)
        app->repeat_rate_ms = new_cfg.repeat_rate_ms;
    apply_mouse_touchpad_config(app);

    GC_LOG(config, "reload applied, need_render=%d", need_render);
    return need_render;
}

int
main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <vtnum> [drm_node] [registry_socket]\n", argv[0]);
        return 2;
    }

    app_t app = {0};
    app.argv0 = argv[0];
    app.argv0_len = strlen(argv[0]);
    app.vt_num = atoi(argv[1]);
    app.drm_node = argc > 2 ? argv[2] : DEFAULT_DRM_NODE;
    const char *registry_socket = argc > 3 ? argv[3] : GHOSTCON_PTYSERV_SOCKET_PATH;

    const char *canary_fd_str = getenv("GHOSTCON_CANARY_FD");
    app.canary_fd = canary_fd_str ? atoi(canary_fd_str) : -1;

    const char *clear_on_logout_str = getenv("GHOSTCON_CLEAR_ON_LOGOUT");
    app.clear_on_logout = (clear_on_logout_str && strcmp(clear_on_logout_str, "0") == 0)
                               ? false : CLEAR_ON_LOGOUT_DEFAULT;

    const char *font_size_str = getenv("GHOSTCON_FONT_SIZE");
    app.font_size = font_size_str ? atoi(font_size_str) : FONT_SIZE;
    if (app.font_size <= 0)
        app.font_size = FONT_SIZE; /* malformed/nonsensical env value -- fall back rather than pass 0/negative to atlas_create */

    const char *zoom_step_str = getenv("GHOSTCON_ZOOM_STEP");
    app.zoom_step = zoom_step_str ? atoi(zoom_step_str) : ZOOM_STEP_DEFAULT;
    if (app.zoom_step <= 0)
        app.zoom_step = ZOOM_STEP_DEFAULT; /* malformed/nonsensical env value */

    /* Hot-reload watch -- see PLAN.md's "General config hot-reload"
       section. GHOSTCON_CONFIG_PATH is exported unconditionally by
       undead-head regardless of whether this process needed a config
       file before (config.h's doc comment); not fatal if either step
       fails, live-reload just silently doesn't happen. */
    app.config_path = getenv("GHOSTCON_CONFIG_PATH");
    if (!app.config_path)
        app.config_path = "/etc/ghostcon/ghostcon.toml";
    app.config_watch_fd = ghostcon_config_watch_open(app.config_path);

    /* [cursor] fields have no individual GHOSTCON_* env var (unlike
       font_size/zoom_step/clear_on_logout above) -- undead-head only
       exports one env var per config key for values every process in
       the tree might plausibly need at spawn time, and cursor assets
       are ghostcon-core-only. Simplest to just load the file directly
       here once for these three fields, same struct/function
       apply_config_reload() already uses for every later reload. */
    app.cursor_base_scale = 1.0f;
    app.cursor_scale_with_terminal = true;
    snprintf(app.copy_to_clipboard_binding, sizeof(app.copy_to_clipboard_binding), "%s", "ctrl+shift+c");
    snprintf(app.paste_from_clipboard_binding, sizeof(app.paste_from_clipboard_binding), "%s", "ctrl+shift+v");
    app.mouse_enable = true;
    app.mouse_scroll_speed = 1.0f;
    app.touchpad_enable = true;
    app.touchpad_scroll_speed = 1.0f;
    app.touchpad_tap_to_click = true;
    app.repeat_delay_ms = 250;
    app.repeat_rate_ms = 50;
    int scrollback_lines = 2000;
    {
        ghostcon_config_t initial_cfg;
        if (ghostcon_config_load(app.config_path, &initial_cfg)) {
            snprintf(app.font_family, sizeof(app.font_family), "%s", initial_cfg.font_family);
            snprintf(app.font_variant, sizeof(app.font_variant), "%s", initial_cfg.font_variant);
            snprintf(app.antialiasing, sizeof(app.antialiasing), "%s", initial_cfg.antialiasing);
            snprintf(app.subpixel_order, sizeof(app.subpixel_order), "%s", initial_cfg.subpixel_order);
            app.gamma_correct = initial_cfg.gamma_correct;
            app.ligatures = initial_cfg.ligatures;
            snprintf(app.theme, sizeof(app.theme), "%s", initial_cfg.theme);
            snprintf(app.color_background, sizeof(app.color_background), "%s", initial_cfg.color_background);
            snprintf(app.color_foreground, sizeof(app.color_foreground), "%s", initial_cfg.color_foreground);
            snprintf(app.color_cursor, sizeof(app.color_cursor), "%s", initial_cfg.color_cursor);
            for (int i = 0; i < 16; i++)
                snprintf(app.color[i], sizeof(app.color[i]), "%s", initial_cfg.color[i]);
            snprintf(app.cursor_theme, sizeof(app.cursor_theme), "%s", initial_cfg.cursor_theme);
            snprintf(app.cursor_default_path, sizeof(app.cursor_default_path), "%s", initial_cfg.cursor_default_path);
            snprintf(app.cursor_link_path, sizeof(app.cursor_link_path), "%s", initial_cfg.cursor_link_path);
            app.cursor_base_scale = initial_cfg.cursor_base_scale;
            app.cursor_scale_with_terminal = initial_cfg.cursor_scale_with_terminal;
            snprintf(app.cursor_default_hot_pos, sizeof(app.cursor_default_hot_pos),
                     "%s", initial_cfg.cursor_default_hot_pos);
            snprintf(app.cursor_link_hot_pos, sizeof(app.cursor_link_hot_pos),
                     "%s", initial_cfg.cursor_link_hot_pos);
            snprintf(app.copy_to_clipboard_binding, sizeof(app.copy_to_clipboard_binding),
                     "%s", initial_cfg.copy_to_clipboard_binding);
            snprintf(app.paste_from_clipboard_binding, sizeof(app.paste_from_clipboard_binding),
                     "%s", initial_cfg.paste_from_clipboard_binding);
            app.mouse_enable = initial_cfg.mouse_enable;
            app.mouse_scroll_speed = initial_cfg.mouse_scroll_speed;
            app.mouse_sensitivity = initial_cfg.mouse_sensitivity;
            app.touchpad_enable = initial_cfg.touchpad_enable;
            app.touchpad_scroll_speed = initial_cfg.touchpad_scroll_speed;
            app.touchpad_tap_to_click = initial_cfg.touchpad_tap_to_click;
            app.touchpad_natural_scroll = initial_cfg.touchpad_natural_scroll;
            app.touchpad_sensitivity = initial_cfg.touchpad_sensitivity;
            if (initial_cfg.repeat_delay_ms > 0)
                app.repeat_delay_ms = initial_cfg.repeat_delay_ms;
            if (initial_cfg.repeat_rate_ms > 0)
                app.repeat_rate_ms = initial_cfg.repeat_rate_ms;
            if (initial_cfg.scrollback_lines > 0)
                scrollback_lines = initial_cfg.scrollback_lines;
        }
    }

    ghostcon_diag_init("ghostcon-core", app.vt_num);

    app.vt = ghostcon_vtctl_open(app.vt_num);
    if (!app.vt) {
        fprintf(stderr, "ghostcon-core: vtctl_open(%d) failed\n", app.vt_num);
        return 1;
    }

    app.atlas = ghostcon_atlas_create(app.font_family[0] ? app.font_family : NULL,
                                       app.font_variant, app.antialiasing,
                                       app.subpixel_order,
                                       app.font_size, ATLAS_DIM);
    if (!app.atlas) {
        fprintf(stderr, "ghostcon-core: atlas_create failed\n");
        ghostcon_vtctl_close(app.vt);
        return 1;
    }
    ghostcon_atlas_cell_size(app.atlas, &app.cell_w, &app.cell_h);

    app.disp = ghostcon_display_create(app.drm_node, app.vt_num, app.gamma_correct);
    if (!app.disp) {
        fprintf(stderr, "ghostcon-core: display_create failed\n");
        ghostcon_atlas_destroy(app.atlas);
        ghostcon_vtctl_close(app.vt);
        return 1;
    }
    push_cursor_config(&app); /* backend replays cursor sprites on each acquire */

    if (ghostcon_vtctl_state(app.vt) == GC_VT_STATE_ACTIVE) {
        if (!acquire_display(&app)) {
            fprintf(stderr, "ghostcon-core: initial display acquire failed\n");
            /* Not fatal — the VT may become active later via a real
               switch, at which point the vtctl signal path retries. */
        }
    }

    /* Terminal grid is sized once, from whatever the first successful
       acquire reports (or a sane default if we're starting inactive).
       A later reacquire at a different resolution just keeps this
       grid — proper live resize (ghostcon_screen_resize + SIGWINCH to
       the shell) is a follow-up, not built here; see PLAN.md's own
       phasing notes on this being provisional. */
    uint16_t cols = 80, rows = 24;
    if (ghostcon_display_is_acquired(app.disp)) {
        cols = (uint16_t)(ghostcon_display_width(app.disp) / (uint32_t)app.cell_w);
        rows = (uint16_t)(ghostcon_display_height(app.disp) / (uint32_t)app.cell_h);
    }
    if (scrollback_lines > 65535)
        scrollback_lines = 65535; /* ghostcon_term_init()'s scrollback_cap is uint16_t */
    if (!ghostcon_term_init(&app.term, cols, rows, (uint16_t)scrollback_lines)) {
        fprintf(stderr, "ghostcon-core: term_init failed\n");
        release_display(&app);
        ghostcon_atlas_destroy(app.atlas);
        ghostcon_vtctl_close(app.vt);
        return 1;
    }
    apply_theme_config(&app);
    ghostcon_stream_set_cell_size(&app.term.stream, app.cell_w, app.cell_h);

    bool transport_connected = false;
    for (int i = 0; i < 50 && !transport_connected; i++) {
        transport_connected = ghostcon_transport_connect(&app.transport, registry_socket, app.vt_num);
        if (!transport_connected)
            usleep(20000);
    }
    if (!transport_connected) {
        fprintf(stderr, "ghostcon-core: could not connect to pty child for vt %d\n", app.vt_num);
        ghostcon_term_deinit(&app.term);
        release_display(&app);
        ghostcon_atlas_destroy(app.atlas);
        ghostcon_vtctl_close(app.vt);
        return 1;
    }
    ghostcon_term_set_output(&app.term, term_output_callback, &app);
    ghostcon_term_set_title(&app.term, term_title_callback, &app);
    ghostcon_term_set_notify(&app.term, term_notify_callback, &app);

    /* Covers the case the later reacquire-path resize (below, in the
       main loop) doesn't: the VT already being active at process start,
       so `app.term` was already correctly sized (from real KMS
       dimensions, not the 80x24 placeholder) before this connection
       even existed. Without this, a program launched before the first
       actual resize event -- which might never come, if the VT is
       never released and reacquired during this run -- would never see
       the right size at all. */
    ghostcon_transport_resize(&app.transport, app.term.rows, app.term.cols,
                              app.term.cols * app.cell_w, app.term.rows * app.cell_h);

    /* No separate input_open() here -- acquire_display() now owns the
       whole input lifecycle (opened there, closed in release_display()),
       including the initial acquire above if the VT was already active
       at startup. A second unconditional open here would leak that
       first context and silently replace it. */

    bool running = true;
    while (running) {
        struct pollfd fds[6];
        int nfds = 0;
        int vtctl_idx = nfds++;
        fds[vtctl_idx] = (struct pollfd){ .fd = ghostcon_vtctl_signal_fd(app.vt), .events = POLLIN };
        int config_watch_idx = -1;
        if (app.config_watch_fd >= 0) {
            config_watch_idx = nfds++;
            fds[config_watch_idx] = (struct pollfd){ .fd = app.config_watch_fd, .events = POLLIN };
        }
        int transport_idx = nfds++;
        fds[transport_idx] = (struct pollfd){ .fd = ghostcon_transport_fd(&app.transport), .events = POLLIN };
        /* app.transport.ctl_fd is -1 when the control connection never
           came up (see its own doc comment) -- not fatal, CLEAR
           notifications just silently won't arrive, same tolerance
           already given to RESIZE going the other direction. */
        int ctl_idx = -1;
        if (app.transport.ctl_fd >= 0) {
            ctl_idx = nfds++;
            fds[ctl_idx] = (struct pollfd){ .fd = app.transport.ctl_fd, .events = POLLIN };
        }
        /* app.input only exists while this VT is actually active/
           acquired (see app_t's own doc comment on `input`, and
           acquire_display()/release_display() -- a fresh libinput
           context is opened per acquire and closed on release, rather
           than kept open across the inactive period). That's the real
           fix for a leak found live: with a single long-lived libinput
           context, every keystroke typed anywhere else on the machine
           while this VT was inactive still arrived at (and queued in)
           this process's own fd at the kernel level regardless of
           whether we called dispatch -- so the moment this VT
           reactivated, the whole accumulated backlog replayed in one
           burst into the pty, indistinguishable from real input typed
           here. Simply skipping dispatch while inactive (tried first)
           only delayed consumption of that backlog, it didn't discard
           it. `app.input` being NULL while inactive is what keeps it
           out of the pollfd set below. */
        int input_idx = -1;
        int repeat_idx = -1;
        if (app.input) {
            input_idx = nfds++;
            fds[input_idx] = (struct pollfd){ .fd = ghostcon_input_fd(app.input), .events = POLLIN };
            /* -1 when timerfd_create() failed at input_open() time --
               key auto-repeat just silently doesn't work, same
               tolerance given to the ctl_fd/CLEAR path above. */
            int rfd = ghostcon_input_repeat_fd(app.input);
            if (rfd >= 0) {
                repeat_idx = nfds++;
                fds[repeat_idx] = (struct pollfd){ .fd = rfd, .events = POLLIN };
            }
        }

        int rv = poll(fds, (nfds_t)nfds, POLL_INTERVAL_MS);
        if (rv < 0) {
            if (errno == EINTR)
                continue;
            break;
        }

        if (app.canary_fd >= 0) {
            uint8_t heartbeat = 1;
            ssize_t ignored = write(app.canary_fd, &heartbeat, 1);
            (void)ignored; /* best-effort — supervisor's poll() is what matters */
        }

        bool need_render = false;

        if (config_watch_idx >= 0 && (fds[config_watch_idx].revents & POLLIN)) {
            if (ghostcon_config_watch_check(app.config_watch_fd, app.config_path)) {
                if (apply_config_reload(&app))
                    need_render = true;
            }
        }

        if (fds[vtctl_idx].revents & POLLIN) {
            ghostcon_vt_state_t before = ghostcon_vtctl_state(app.vt);
            ghostcon_vtctl_process_pending(app.vt, vtctl_pre_ack, &app);
            ghostcon_vt_state_t after = ghostcon_vtctl_state(app.vt);

            if (before != GC_VT_STATE_ACTIVE && after == GC_VT_STATE_ACTIVE) {
                if (!acquire_display(&app)) {
                    fprintf(stderr, "ghostcon-core: reacquire failed, will keep retrying on next switch\n");
                } else {
                    /* The terminal grid was sized at startup from
                       app.kms.width/height -- but if the VT wasn't
                       already active at process start (the normal
                       case: you switch to it later), acquire_display()
                       hadn't run yet at that point, so term_init fell
                       back to a hardcoded 80x24 placeholder instead of
                       the real screen size. That placeholder was never
                       revisited once the real size became known here,
                       so the terminal only ever occupied a small
                       corner of the actual display and scrolled well
                       before reaching the real bottom of the screen —
                       found live ("halfway through it just scrolls up
                       instead of keeping to allocate the next row").
                       Recompute and resize on every acquire (not just
                       detecting the placeholder case) so a reacquire at
                       a genuinely different resolution is handled the
                       same way, closing the "known Phase 1 limitation"
                       noted elsewhere in this file too. */
                    uint16_t new_cols = (uint16_t)(ghostcon_display_width(app.disp) / (uint32_t)app.cell_w);
                    uint16_t new_rows = (uint16_t)(ghostcon_display_height(app.disp) / (uint32_t)app.cell_h);
                    if (new_cols != app.term.cols || new_rows != app.term.rows) {
                        if (!ghostcon_term_resize(&app.term, new_cols, new_rows))
                            fprintf(stderr, "ghostcon-core: term_resize to %ux%u failed\n",
                                    new_cols, new_rows);
                        /* Propagate to the actual kernel pty -- without
                           this, ghostcon's own idea of the grid size
                           changes but nothing tells programs running in
                           the shell (found live: a TUI rendered into a
                           small, wrong-sized corner because ioctl(
                           TIOCGWINSZ) never reflected reality, not even
                           once). See transport.h's ghostcon_transport_resize()
                           and ptyserv/pty_child.c's control-socket
                           handler for the other end of this. */
                        ghostcon_transport_resize(&app.transport, new_rows, new_cols,
                                                  new_cols * app.cell_w, new_rows * app.cell_h);
                    }

                    /* Two things need forcing here, not just one:
                       (1) Acquiring the display alone doesn't paint
                       anything -- render_frame only runs below when
                       need_render is set, which otherwise only happens
                       when NEW PTY bytes arrive THIS SAME poll
                       iteration. Whatever was already fed while
                       inactive (e.g. the shell's startup prompt) would
                       sit undrawn until the next unrelated PTY write --
                       found via exactly that symptom on the first real
                       VT-switch test ("stays blank until you press a
                       key"). (2) acquire_display() just destroyed and
                       recreated the entire GPU context (kms/egl/gles)
                       from scratch, so the terminal's own dirty-region
                       tracking -- which assumes a CONTINUOUS GPU
                       context and only remembers cells changed since
                       the last render -- is not sufficient on its own
                       for a reacquire (as opposed to the very first
                       acquire): if a prior render cycle already cleared
                       it, there's nothing marked dirty even though the
                       fresh GPU context has no prior frame content at
                       all. Force the whole screen dirty explicitly,
                       same convention ghostcon_screen_reset() already
                       uses internally. */
                    app.term.screen.dirty.y_min = 0;
                    app.term.screen.dirty.y_max = (int16_t)(app.term.screen.rows_visible - 1);
                    need_render = true;
                }
            } else if (before == GC_VT_STATE_ACTIVE && after != GC_VT_STATE_ACTIVE) {
                release_display(&app);
            }
        }

        if (fds[transport_idx].revents & (POLLIN | POLLHUP | POLLERR)) {
            /* Drain everything currently queued, not just one 4096-byte
               read -- t->fd is nonblocking (see transport.c's connect),
               so this loop costs nothing extra once the socket runs
               dry. Without this, a large transfer split across many
               escape-sequence chunks (e.g. a chunked Kitty image) paid
               a full render_frame() -- including its blocking,
               vsync-gated page flip -- per 4096 bytes instead of once
               per actual wakeup, which is what made such transfers
               visibly slower than Ghostty despite no more total work
               being done. Capped so a pathological sustained writer
               can't starve the other polled fds forever. */
            bool got_data = false;
            size_t total_drained = 0;
            for (;;) {
                uint8_t buf[65536];
                ssize_t n = ghostcon_transport_read(&app.transport, buf, sizeof(buf));
                if (n > 0) {
                    ghostcon_term_feed(&app.term, buf, (size_t)n);
                    got_data = true;
                    total_drained += (size_t)n;
                    if (total_drained >= (1u << 20))
                        break;
                    continue;
                }
                if (n < 0 && errno == EINTR)
                    continue;
                if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                    break;
                /* pty child gone (shell exited, or ghost-ptyserv itself
                   died) — this VT's session is over. Exiting is the
                   correct reaction; a future supervisor decides what
                   runs here next, not this process. */
                running = false;
                break;
            }
            if (got_data) {
                GC_LOG(pty, "drained %zu bytes", total_drained);
                need_render = true;
            }
        }

        if (ctl_idx >= 0 && (fds[ctl_idx].revents & (POLLIN | POLLHUP | POLLERR))) {
            bool got_clear = false;
            ghostcon_transport_read_ctl(&app.transport, &got_clear);
            if (got_clear && app.clear_on_logout) {
                ghostcon_screen_erase_display(&app.term.screen, GC_ERASE_DISPLAY_ALL);
                ghostcon_screen_erase_display(&app.term.screen, GC_ERASE_DISPLAY_SCROLLBACK);
                app.term.screen.cursor.x = 0;
                app.term.screen.cursor.y = 0;
                need_render = true;
            }
        }

        if (input_idx >= 0 && (fds[input_idx].revents & POLLIN)) {
            ghostcon_input_sync_modes(app.input, &app.term.screen);
            int zoom_delta = 0;
            ghostcon_input_pointer_t pointer = {0};
            bool dump_requested = false;
            bool vt_active = is_vt_foreground(app.vt_num);
            if (!ghostcon_input_dispatch(app.input, &app.transport, &app.term.screen,
                                          app.cell_w, app.cell_h, &zoom_delta, &pointer,
                                          &dump_requested, vt_active))
                fprintf(stderr, "ghostcon-core: input dispatch error (continuing)\n");
            /* Hardware cursor movement is deliberately NOT folded into
               need_render/render_frame() -- see kms.h's own doc comment
               on ghostcon_kms_move_cursor() for why: the whole point of
               a real DRM cursor plane over a GLES quad is that its
               latency doesn't depend on content rendering. Called
               directly here instead. */
            if (pointer.moved)
                GC_LOG(selection, "pointer pixel=(%d,%d)", pointer.x, pointer.y);
            if (pointer.moved && ghostcon_display_is_acquired(app.disp)) {
                /* Hardware cursor movement is deliberately NOT folded
                   into need_render/render_frame() -- see kms.h's own
                   doc comment on ghostcon_kms_move_cursor() for why:
                   the whole point of a real DRM cursor plane over a
                   GLES quad is that its latency doesn't depend on
                   content rendering. Called directly here instead. */
                ghostcon_display_move_cursor(app.disp, pointer.x, pointer.y);

                /* Hover-state detection: switch to the "link" cursor
                   sprite while over an OSC-8 hyperlink cell, back to
                   "default" otherwise (pure grid hit-testing, no DRM
                   involved). ghostcon_display_set_cursor_state()
                   internally no-ops unless the state actually differs
                   from last time, so hovering the same cell across
                   many motion events in a row doesn't re-commit the
                   plane every time. */
                ghostcon_cursor_state_t state = ghostcon_ui_compute_cursor_state(
                    &app.term.screen, app.cell_w, app.cell_h, pointer.x, pointer.y);
                ghostcon_display_set_cursor_state(app.disp, state);
            }

            /* Click-drag text selection -- pointer.left_pressed/
               left_released are only ever set when core/input.c's
               should_intercept_for_selection() decided this click is
               LOCAL (app hasn't grabbed mouse reporting, or Shift is
               overriding it), so no further mode-checking is needed
               here. Independent `if`s, not `else if` -- a fast click
               (press+release within one dispatch() call) must apply
               both in order, not have the release silently dropped.
               Copying to the clipboard is a separate, explicit
               keyboard-shortcut step (input.c's copy_to_clipboard
               binding), not automatic on release -- see PLAN.md. */
            /* Click-drag text selection transitions. pointer.left_pressed/
               left_released are only ever set when core/input.c's
               should_intercept_for_selection() decided this click is
               LOCAL (app hasn't grabbed mouse reporting, or Shift is
               overriding it), so no further mode-checking is needed
               here. Independent `if`s inside, not `else if` -- a fast
               click (press+release within one dispatch() call) must
               apply both in order, not have the release silently
               dropped. Copying to the clipboard is a separate, explicit
               keyboard-shortcut step (input.c's copy_to_clipboard
               binding), not automatic on release -- see PLAN.md. A
               plain click (no drag) deselects rather than "selecting"
               the one cell under the pointer -- matches how a click in
               most terminals clears any existing selection instead of
               creating a zero-width one. */
            if (ghostcon_ui_update_selection(&app.term.screen, &pointer,
                                              app.cell_w, app.cell_h))
                need_render = true;

            /* A scrollback shortcut (Shift+Up/Down/PageUp/PageDown)
               changes the screen directly and produces no pty output,
               so it can't rely on the transport_idx branch below to
               notice and trigger a render -- check dirty state here too. */
            if (ghostcon_screen_get_dirty(&app.term.screen).y_min >= 0)
                need_render = true;
            /* Ctrl+=/Ctrl+Minus zoom shortcut -- same reasoning as
               above, plus it needs core/main.c's own apply_font_size()
               since core/input.c has no atlas/font ownership. input.c
               reports DIRECTION only (+-1 per press, "one zoom tick");
               the actual magnitude is app.zoom_step, config-driven
               (default 2pt -- see core/input.c's handle_zoom_shortcut()
               doc comment for why 1pt wasn't enough) and reloadable via
               ghostcon.toml like everything else in this block.
               ZOOM_MIN/MAX_FONT_SIZE keep it away from nonsensical
               extremes (a 0/negative size, or one large enough to
               plausibly not fit ATLAS_DIM -- apply_font_size already
               handles a failed rebuild gracefully regardless, this is
               just to keep the common case sane). */
            if (zoom_delta != 0) {
                int new_size = ghostcon_ui_compute_zoom(app.font_size, zoom_delta,
                                                         app.zoom_step,
                                                         ZOOM_MIN_FONT_SIZE,
                                                         ZOOM_MAX_FONT_SIZE);
                if (apply_font_size(&app, new_size, false))
                    need_render = true;
            }

            /* Ctrl+Alt+D -- dump the live screen state right now, at
               exactly the cell/cursor/mode state a visible glitch was
               caught at, rather than trying to reproduce it offline.
               Fixed path (not per-VT) is deliberate: this is a manual
               debug action, one dump at a time, no need for the
               per-vtN namespacing the ctl sockets use. */
            if (dump_requested) {
                GC_LOG(input, "screen dump requested (Ctrl+Alt+D)");
                app.screenshot_requested = true;
                need_render = true;
                FILE *df = fopen("/tmp/ghostcon-dump.txt", "w");
                if (df) {
                    ghostcon_screen_dump(df, &app.term.screen);
                    fclose(df);
                } else {
                    fprintf(stderr, "ghostcon-core: dump requested but "
                                    "/tmp/ghostcon-dump.txt couldn't be opened: %s\n",
                            strerror(errno));
                }
                /* Raw bytes leading up to this exact moment, alongside
                   the screen-state dump above -- see transport_request_dump's
                   own doc comment. */
                ghostcon_transport_request_dump(&app.transport);
            }
        }

        if (repeat_idx >= 0 && (fds[repeat_idx].revents & POLLIN)) {
            if (!ghostcon_input_repeat_fire(app.input, &app.transport))
                fprintf(stderr, "ghostcon-core: repeat-fire transport write error (continuing)\n");
        }

        if (need_render)
            app.render_deferred = true;

        if (app.render_deferred && ghostcon_display_is_acquired(app.disp)) {
            /* Mode 2026 (Synchronized Output): an app mid-batch (cursor
               moved, some cells rewritten, more still pending) wants us
               to hold off presenting until it sends ?2026l -- otherwise
               a redraw bigger than one 4KB pty read (main loop reads
               above) spans multiple poll iterations and each one would
               present a torn, half-applied frame. Withholding here is
               bounded by SYNC_OUTPUT_TIMEOUT_MS below, not held open
               indefinitely: a crashed/buggy app that sets ?2026h and
               never clears it would otherwise freeze rendering forever
               instead of just missing this one deadline. render_deferred
               (not the loop-local need_render) is what's checked, so a
               batch that spans multiple poll iterations with no other
               fd activity in between still gets flushed once the
               POLL_INTERVAL_MS canary wakeup notices the timeout has
               elapsed. */
            bool still_withholding = app.term.screen.synchronized_output;
            if (still_withholding) {
                struct timespec now;
                clock_gettime(CLOCK_MONOTONIC, &now);
                double elapsed_ms =
                    (now.tv_sec - app.term.screen.synchronized_output_since.tv_sec) * 1000.0 +
                    (now.tv_nsec - app.term.screen.synchronized_output_since.tv_nsec) / 1e6;
                still_withholding = elapsed_ms < SYNC_OUTPUT_TIMEOUT_MS;
            }

            if (!still_withholding) {
                app.render_deferred = false;
                if (!ghostcon_display_render_frame(app.disp, &app.term.screen,
                                                    app.atlas, app.cell_w, app.cell_h,
                                                    app.ligatures, app.screenshot_requested)) {
                    fprintf(stderr, "ghostcon-core: render_frame failed, releasing display\n");
                    release_display(&app);
                }
                /* screenshot_requested consumed by the present above
                   (read inside the frame draw, per gles.h's doc comment
                   on why it must happen there) */
                app.screenshot_requested = false;
            }
        }
    }

    if (app.input)
        ghostcon_input_close(app.input);
    ghostcon_transport_close(&app.transport);
    ghostcon_term_deinit(&app.term);
    ghostcon_display_destroy(app.disp);
    ghostcon_atlas_destroy(app.atlas);
    ghostcon_vtctl_close(app.vt);

    return 0;
}
