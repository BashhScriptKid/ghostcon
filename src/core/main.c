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

#include "ghostcon/core/diag.h"
#include "ghostcon/core/egl.h"
#include "ghostcon/core/input.h"
#include "ghostcon/core/kms.h"
#include "ghostcon/core/transport.h"
#include "ghostcon/core/vtctl.h"
#include "ghostcon/ptyserv/protocol.h"
#include "ghostcon/render/atlas.h"
#include "ghostcon/render/gles.h"
#include "ghostcon/render/machine.h"
#include "ghostcon/term/term.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <unistd.h>
#include <xf86drm.h>

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
#define POLL_INTERVAL_MS 1000 /* canary heartbeat cadence when otherwise idle */
#define DRM_MASTER_RETRIES 30
#define DRM_MASTER_RETRY_DELAY_US 100000

typedef struct {
    int vt_num;
    const char *drm_node;
    int canary_fd;
    bool clear_on_logout;

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

    /* Per-acquire-cycle: torn down on release, rebuilt on reacquire. */
    bool display_acquired;
    int drm_fd;
    bool have_master;
    ghostcon_kms_t kms;
    ghostcon_egl_t egl;
    ghostcon_gles_t *gles;
    bool did_modeset;
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
} app_t;

static void
release_display(app_t *app)
{
    if (!app->display_acquired)
        return;

    if (app->gles)
        ghostcon_gles_destroy(app->gles);
    app->gles = NULL;
    ghostcon_egl_deinit(&app->egl);
    ghostcon_kms_deinit(&app->kms);
    if (app->have_master)
        drmDropMaster(app->drm_fd);
    if (app->drm_fd >= 0)
        close(app->drm_fd);
    app->drm_fd = -1;
    app->have_master = false;
    app->did_modeset = false;
    app->display_acquired = false;

    if (app->input)
        ghostcon_input_close(app->input);
    app->input = NULL;

    ghostcon_diag_log_warning("vt %d: display released", app->vt_num);
}

static bool
acquire_display(app_t *app)
{
    app->drm_fd = open(app->drm_node, O_RDWR | O_CLOEXEC);
    if (app->drm_fd < 0) {
        fprintf(stderr, "ghostcon-core: open %s: %s\n", app->drm_node, strerror(errno));
        return false;
    }

    for (int i = 0; i < DRM_MASTER_RETRIES && !app->have_master; i++) {
        if (drmSetMaster(app->drm_fd) == 0)
            app->have_master = true;
        else
            usleep(DRM_MASTER_RETRY_DELAY_US);
    }
    if (!app->have_master) {
        fprintf(stderr, "ghostcon-core: drmSetMaster failed: %s\n", strerror(errno));
        close(app->drm_fd);
        app->drm_fd = -1;
        return false;
    }

    if (!ghostcon_kms_init(&app->kms, app->drm_fd) ||
        !ghostcon_kms_create_scanout_surface(&app->kms)) {
        fprintf(stderr, "ghostcon-core: kms init failed\n");
        goto fail;
    }

    if (!ghostcon_egl_init_with_gbm(&app->egl, app->kms.gbm_dev, app->kms.gbm_surf,
                                     app->kms.width, app->kms.height) ||
        !ghostcon_egl_make_current(&app->egl)) {
        fprintf(stderr, "ghostcon-core: egl init failed\n");
        goto fail;
    }

    app->gles = ghostcon_gles_create(app->kms.width, app->kms.height);
    if (!app->gles) {
        fprintf(stderr, "ghostcon-core: gles init failed\n");
        goto fail;
    }
    /* Forced: this fresh instance's GPU texture starts empty regardless
       of whether the (separate, longer-lived) CPU-side atlas bitmap has
       changed recently -- see gles.h's doc comment on why a dirty-gated
       sync here left the texture "incomplete" and glyphs rendering as
       solid rectangles after a reacquire. */
    ghostcon_gles_sync_atlas(app->gles, app->atlas, true);

    /* Fresh input context every acquire -- see app_t's own doc comment
       on `input` for why this must not be reused across a release/
       reacquire cycle (kernel-level event backlog, not a polling
       issue). Not fatal if it fails: continue without keyboard input,
       matching main()'s own original startup behavior. */
    app->input = ghostcon_input_open("seat0");
    if (!app->input)
        fprintf(stderr, "ghostcon-core: input_open failed -- continuing without keyboard input\n");

    app->display_acquired = true;
    app->did_modeset = false;
    ghostcon_diag_log_warning("vt %d: display acquired (%ux%u)",
                               app->vt_num, app->kms.width, app->kms.height);
    return true;

fail:
    if (app->gles)
        ghostcon_gles_destroy(app->gles);
    app->gles = NULL;
    ghostcon_egl_deinit(&app->egl);
    ghostcon_kms_deinit(&app->kms);
    drmDropMaster(app->drm_fd);
    close(app->drm_fd);
    app->drm_fd = -1;
    app->have_master = false;
    return false;
}

static bool
render_frame(app_t *app)
{
    GhosttyColorRgb bg_rgb = app->term.screen.palette.bg_default;
    float bg[3] = {
        (float)bg_rgb.r / 255.0f,
        (float)bg_rgb.g / 255.0f,
        (float)bg_rgb.b / 255.0f,
    };

    /* Clear + redraw the full accumulated dirty region EVERY frame,
       not just what changed since the previous one. This looks
       wasteful, and an earlier version of this code "optimized" it
       (conditional clear + ghostcon_screen_clear_dirty() after each
       frame) -- but that optimization is actually wrong given how
       core/kms.c presents frames: the GBM/EGL surface rotates across
       multiple physical buffers (typical for tear-free presentation),
       and each buffer has its OWN independent history of what's been
       painted onto it. "Redraw only what changed since the last
       render() call" implicitly assumes a single continuous buffer;
       against N rotating buffers, each one only ever receives the
       damage from every Nth frame, so content silently disappears and
       reappears on alternating frames (found live: characters flashed
       in and out on literally every keystroke). Proper per-buffer
       damage tracking would fix this correctly, but is real complexity
       this doesn't need yet -- full repaint every frame is what the
       earlier live interactive-bash test already proved correct on
       real hardware, before this regression was introduced. See
       PLAN.md's own renderer design note: "redrawing all dirty cells
       each frame is sufficient" for a terminal at this scale. */
    ghostcon_gles_begin(app->gles, true, bg[0], bg[1], bg[2]);
    ghostcon_machine_render_dirty(&app->term.screen, app->atlas, app->gles,
                                   app->cell_w, app->cell_h);
    ghostcon_machine_render_cursor(&app->term.screen, app->gles,
                                    app->cell_w, app->cell_h);
    ghostcon_gles_sync_atlas(app->gles, app->atlas, false);
    ghostcon_gles_end(app->gles);

    if (!ghostcon_egl_swap(&app->egl))
        return false;

    if (!app->did_modeset) {
        if (!ghostcon_kms_modeset(&app->kms))
            return false;
        app->did_modeset = true;
    } else {
        if (!ghostcon_kms_page_flip(&app->kms))
            return false;
    }
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

/* Extracts the "command token" PLAN.md's PR_SET_NAME algorithm uses:
   the first word if the title looks like "cmd args...", then reduced
   to its basename if that word looks like a path. Matches the spec's
   worked example (a script path -> just the script's basename). */
static void
extract_command_token(const char *title, char *out, size_t out_len)
{
    size_t word_len = strcspn(title, " \t");
    const char *word_end = title + word_len;
    const char *slash = NULL;
    for (const char *p = title; p < word_end; p++)
        if (*p == '/')
            slash = p;
    const char *start = slash ? slash + 1 : title;
    size_t n = (size_t)(word_end - start);
    if (n >= out_len)
        n = out_len - 1;
    memcpy(out, start, n);
    out[n] = '\0';
}

/* Builds the truncated `comm` (PR_SET_NAME, 15 bytes incl. NUL -> 14
   usable chars) form: "gc@tty<N> " + token, tail-truncated with a
   leading "..." if it doesn't fit -- see PLAN.md's "Process identity"
   section for the full worked rationale (tail truncation keeps the
   usually-more-informative trailing part of a long name). `out` must
   be at least 16 bytes. */
static void
build_comm_name(int vt_num, const char *token, char *out)
{
    char prefix[16];
    int prefix_len = snprintf(prefix, sizeof(prefix), "gc@tty%d ", vt_num);
    if (prefix_len < 0) prefix_len = 0;
    if (prefix_len > 14) prefix_len = 14; /* pathological VT number, hard clamp */

    int budget = 14 - prefix_len;
    size_t token_len = strlen(token);

    if (budget <= 0) {
        /* No room left even for the prefix alone once clamped to 14 —
           just hard-truncate the whole "prefix+token" concatenation. */
        char combined[128];
        snprintf(combined, sizeof(combined), "%.*s%s", prefix_len, prefix, token);
        size_t n = strlen(combined);
        if (n > 14) n = 14;
        memcpy(out, combined, n);
        out[n] = '\0';
        return;
    }

    if ((int)token_len <= budget) {
        snprintf(out, 16, "%.*s%s", prefix_len, prefix, token);
        return;
    }

    int keep = budget - 3; /* room for "..." + `keep` trailing chars */
    if (keep <= 0) {
        /* Not even room for "..." + 1 char — hard-truncate instead. */
        char combined[128];
        snprintf(combined, sizeof(combined), "%.*s%s", prefix_len, prefix, token);
        size_t n = strlen(combined);
        if (n > 14) n = 14;
        memcpy(out, combined, n);
        out[n] = '\0';
        return;
    }

    snprintf(out, 16, "%.*s...%s", prefix_len, prefix, token + (token_len - (size_t)keep));
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
    extract_command_token(title, token, sizeof(token));
    char comm[16];
    build_comm_name(app->vt_num, token, comm);
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

    ghostcon_diag_init("ghostcon-core", app.vt_num);

    app.vt = ghostcon_vtctl_open(app.vt_num);
    if (!app.vt) {
        fprintf(stderr, "ghostcon-core: vtctl_open(%d) failed\n", app.vt_num);
        return 1;
    }

    app.atlas = ghostcon_atlas_create(NULL, FONT_SIZE, ATLAS_DIM);
    if (!app.atlas) {
        fprintf(stderr, "ghostcon-core: atlas_create failed\n");
        ghostcon_vtctl_close(app.vt);
        return 1;
    }
    ghostcon_atlas_cell_size(app.atlas, &app.cell_w, &app.cell_h);

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
    if (app.display_acquired) {
        cols = (uint16_t)(app.kms.width / (uint32_t)app.cell_w);
        rows = (uint16_t)(app.kms.height / (uint32_t)app.cell_h);
    }
    if (!ghostcon_term_init(&app.term, cols, rows, 2000)) {
        fprintf(stderr, "ghostcon-core: term_init failed\n");
        release_display(&app);
        ghostcon_atlas_destroy(app.atlas);
        ghostcon_vtctl_close(app.vt);
        return 1;
    }

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
    ghostcon_transport_resize(&app.transport, app.term.rows, app.term.cols);

    /* No separate input_open() here -- acquire_display() now owns the
       whole input lifecycle (opened there, closed in release_display()),
       including the initial acquire above if the VT was already active
       at startup. A second unconditional open here would leak that
       first context and silently replace it. */

    bool running = true;
    while (running) {
        struct pollfd fds[4];
        int nfds = 0;
        int vtctl_idx = nfds++;
        fds[vtctl_idx] = (struct pollfd){ .fd = ghostcon_vtctl_signal_fd(app.vt), .events = POLLIN };
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
        if (app.input) {
            input_idx = nfds++;
            fds[input_idx] = (struct pollfd){ .fd = ghostcon_input_fd(app.input), .events = POLLIN };
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
                    uint16_t new_cols = (uint16_t)(app.kms.width / (uint32_t)app.cell_w);
                    uint16_t new_rows = (uint16_t)(app.kms.height / (uint32_t)app.cell_h);
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
                        ghostcon_transport_resize(&app.transport, new_rows, new_cols);
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
            uint8_t buf[4096];
            ssize_t n = ghostcon_transport_read(&app.transport, buf, sizeof(buf));
            if (n <= 0) {
                /* pty child gone (shell exited, or ghost-ptyserv itself
                   died) — this VT's session is over. Exiting is the
                   correct reaction; a future supervisor decides what
                   runs here next, not this process. */
                running = false;
            } else {
                ghostcon_term_feed(&app.term, buf, (size_t)n);
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
            if (!ghostcon_input_dispatch(app.input, &app.transport, &app.term.screen))
                fprintf(stderr, "ghostcon-core: input dispatch error (continuing)\n");
            /* A scrollback shortcut (Shift+Up/Down/PageUp/PageDown)
               changes the screen directly and produces no pty output,
               so it can't rely on the transport_idx branch below to
               notice and trigger a render -- check dirty state here too. */
            if (ghostcon_screen_get_dirty(&app.term.screen).y_min >= 0)
                need_render = true;
        }

        if (need_render && app.display_acquired) {
            if (!render_frame(&app)) {
                fprintf(stderr, "ghostcon-core: render_frame failed, releasing display\n");
                release_display(&app);
            }
        }
    }

    if (app.input)
        ghostcon_input_close(app.input);
    ghostcon_transport_close(&app.transport);
    ghostcon_term_deinit(&app.term);
    release_display(&app);
    ghostcon_atlas_destroy(app.atlas);
    ghostcon_vtctl_close(app.vt);

    return 0;
}
