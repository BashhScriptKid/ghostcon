/*
 * supervisor[ttyN] — per-VT lifecycle manager.
 *
 * State machine (PLAN.md "Supervision layer"):
 *
 *   IDLE -> SPAWNING (spawn ghostcon-core, start canary/startup timer)
 *           |- ghostcon-core claims VT (first canary byte) -> ACTIVE
 *           `- timer fires before that -> kill it -> FALLBACK
 *   ACTIVE -> canary timeout (hang) -> kill it -> SPAWNING (retry ghostcon,
 *             NOT fallback -- fallback is only for "never even started")
 *   ACTIVE -> ghostcon-core exits cleanly (POLLHUP) -> SPAWNING
 *   FALLBACK -> SIGHUP ("user requests ghostcon") -> kill fallback -> SPAWNING
 *
 * The canary is deliberately dumb (PLAN.md "Philosophy"): silence past a
 * deadline = treat as dead, full stop, no attempt to diagnose why. The
 * startup race timer and the ongoing canary are the SAME mechanism (one
 * poll(canary_fd, POLLIN|POLLHUP, deadline) call) -- the only difference
 * is what "timeout" means for the current state (fall back vs. retry).
 *
 * Env var overrides (no TOML config yet, same precedent as ghost-ptyserv):
 *   GHOSTCON_CORE_BIN        - path to the renderer binary (default
 *                              "ghostcon-core"; tests substitute a stand-in)
 *   GHOSTCON_CANARY_DEADLINE_MS - default 4000
 *   GHOSTCON_RUN_DIR         - recovery file directory (default
 *                              "/run/ghostcon")
 *   GHOSTCON_DISABLE_WALL    - if set, skip the wall(1) broadcast (tests
 *                              must not spam real logged-in users)
 *   GHOSTCON_DISABLE_KMSCON_FALLBACK - if set, skip straight to the
 *                              agetty tier (tests only)
 *   GHOSTCON_DRM_NODE        - DRM node to render on (default
 *                              "/dev/dri/card1", this machine's actual
 *                              GPU; overridden by the [drm_node] argv
 *                              below if given). In principle settable
 *                              via `pkexec env GHOSTCON_DRM_NODE=... CMD`
 *                              since a plain `VAR=val pkexec CMD` prefix
 *                              doesn't survive pkexec's environment
 *                              reset -- but in practice `pkexec env ...`
 *                              didn't reliably show its auth prompt at
 *                              all when tried live, so don't depend on
 *                              this path working; fix the default
 *                              instead if it's ever wrong again.
 *
 * Usage: supervisor <vtnum> [registry_socket] [drm_node]
 * (registry_socket first since that's what undead-head actually has to
 * pass through -- drm_node is a supervisor-local hardware detail
 * undead-head doesn't need to know about. Internally reordered to
 * ghostcon-core's own <vtnum> [drm_node] [registry_socket] argv when
 * spawning it -- see spawn_renderer.)
 *
 * GHOSTCON_CORE_BIN's default resolution does NOT fall back to a bare
 * name + PATH lookup -- see undead_head/main.c's file comment (same
 * reasoning applies here: this runs under pkexec, PATH isn't trustworthy).
 */

#define _DEFAULT_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "ghostcon/config/config.h"
#include "ghostcon/ptyserv/protocol.h"

#define DEFAULT_DRM_NODE "/dev/dri/card1" /* mirrors core/main.c's own default -- see its comment */
#define DEFAULT_CANARY_DEADLINE_MS 4000

/* Resolves `name` relative to this executable's own directory (via
   /proc/self/exe) instead of trusting PATH -- see this file's header
   comment on GHOSTCON_CORE_BIN. Falls back to the bare name only if
   /proc/self/exe itself is unavailable. */
static void
resolve_sibling_path(char *out, size_t out_len, const char *name)
{
    char self_path[4096];
    ssize_t n = readlink("/proc/self/exe", self_path, sizeof(self_path) - 1);
    if (n <= 0) {
        snprintf(out, out_len, "%s", name);
        return;
    }
    self_path[n] = '\0';

    char *slash = strrchr(self_path, '/');
    if (!slash) {
        snprintf(out, out_len, "%s", name);
        return;
    }
    *slash = '\0';
    snprintf(out, out_len, "%s/%s", self_path, name);
}
#define DEFAULT_RUN_DIR "/run/ghostcon"
#define WALL_RATE_LIMIT_SECS 60 /* per failure class, per supervisor process lifetime */

typedef enum {
    ST_SPAWNING,
    ST_ACTIVE,
    ST_FALLBACK,
} state_t;

/* Self-pipe for the "return from FALLBACK" trigger -- SIGHUP handlers
   can't safely do the actual state transition, same reasoning as
   core/vtctl.c's acquire/release handling. */
static int g_sighup_pipe[2] = { -1, -1 };

static void
sighup_handler(int signum)
{
    (void)signum;
    char c = 'H';
    ssize_t unused = write(g_sighup_pipe[1], &c, 1);
    (void)unused;
}

static void
drain_sighup_pipe(void)
{
    char buf[64];
    while (read(g_sighup_pipe[0], buf, sizeof(buf)) > 0)
        ;
}

/* ------------------------------------------------------------------ */
/* Renderer (ghostcon-core) spawn/kill with a canary socketpair         */
/* ------------------------------------------------------------------ */

static pid_t
spawn_renderer(int vtnum, const char *registry_socket, const char *drm_node, int *out_canary_fd)
{
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        perror("supervisor: socketpair");
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("supervisor: fork");
        close(sv[0]);
        close(sv[1]);
        return -1;
    }

    if (pid == 0) {
        close(sv[0]); /* child keeps sv[1] */

        char fd_str[16];
        snprintf(fd_str, sizeof(fd_str), "%d", sv[1]);
        setenv("GHOSTCON_CANARY_FD", fd_str, 1);

        const char *bin = getenv("GHOSTCON_CORE_BIN");
        char bin_buf[4096];
        if (!bin || !*bin) {
            resolve_sibling_path(bin_buf, sizeof(bin_buf), "ghostcon-core");
            bin = bin_buf;
        }

        char vt_str[16];
        snprintf(vt_str, sizeof(vt_str), "%d", vtnum);

        /* drm_node/registry_socket are always resolved to concrete
           values by main() before reaching here -- see its comment.
           execlp's variadic args must all be non-NULL until the
           terminating NULL, so that resolution matters, not just tidiness. */
        execlp(bin, bin, vt_str, drm_node, registry_socket, (char *)NULL);
        perror("supervisor: execlp ghostcon-core");
        _exit(127);
    }

    close(sv[1]); /* parent keeps sv[0] */
    *out_canary_fd = sv[0];
    return pid;
}

static void
kill_and_reap(pid_t pid, int canary_fd)
{
    if (canary_fd >= 0)
        close(canary_fd);
    if (pid <= 0)
        return;
    kill(pid, SIGTERM);

    /* Brief grace period, then escalate -- a process that's already
       confirmed unresponsive doesn't get to ignore SIGTERM politely. */
    for (int i = 0; i < 10; i++) {
        int status;
        if (waitpid(pid, &status, WNOHANG) == pid)
            return;
        usleep(50000);
    }
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
}

/* ------------------------------------------------------------------ */
/* Recovery file + wall broadcast                                      */
/* ------------------------------------------------------------------ */

/* Looks up the real pty child pid via ghost-ptyserv's registry (same
   "GET <vtnum>" protocol ghostcon-core's own transport.c uses to
   connect) so the recovery file records something a human/tool can
   actually act on, instead of a permanent placeholder. Returns -1 on
   any failure (registry unreachable, no pty child registered for this
   vt yet -- e.g. a startup_timeout so early ghostcon-core never got
   as far as querying the registry itself) -- same conservative
   fallback the caller already had before this existed, just no
   longer the ONLY possible outcome. Best-effort and deliberately
   simple: a short, bounded round-trip on the same registry socket
   the renderer itself would use, not a persistent connection. */
static pid_t
query_pty_child_pid(const char *registry_socket, int vtnum)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    /* Bounded, not best-effort-eventually: this runs inline in the
       supervisor's own state-machine loop, so a registry that accepts
       the connection but never writes back (ghost-ptyserv wedged,
       mid-restart, or simply slow) must not be able to block this
       process indefinitely -- that would turn a "write a diagnostic
       recovery file" call into exactly the kind of wedged-event-loop
       condition this whole project exists to detect and recover from
       in OTHER processes. Found live via test_undead_head.c timing
       out after this function was added without a timeout: a
       plain blocking read() here has no such protection on its own. */
    struct timeval tv = { .tv_sec = 0, .tv_usec = 500000 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, registry_socket, sizeof(addr.sun_path) - 1);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    char req[GHOSTCON_PTYSERV_LINE_MAX];
    size_t req_len = ghostcon_ptyserv_format_get(req, sizeof(req), vtnum);
    if (req_len == 0 || write(fd, req, req_len) != (ssize_t)req_len) {
        close(fd);
        return -1;
    }

    char resp[GHOSTCON_PTYSERV_LINE_MAX];
    ssize_t r = read(fd, resp, sizeof(resp) - 1);
    close(fd);
    if (r <= 0)
        return -1;
    resp[r] = '\0';

    int pid = -1;
    char socket_path[GHOSTCON_PTYSERV_LINE_MAX];
    if (!ghostcon_ptyserv_parse_ok(resp, &pid, socket_path))
        return -1;
    return (pid_t)pid;
}

static void
write_recovery_file(int vtnum, pid_t pty_child_pid, const char *reason)
{
    const char *run_dir = getenv("GHOSTCON_RUN_DIR");
    if (!run_dir || !*run_dir)
        run_dir = DEFAULT_RUN_DIR;

    mkdir(run_dir, 0755); /* best-effort; ignore EEXIST/errors */

    char path[512];
    snprintf(path, sizeof(path), "%s/recovery-tty%d.json", run_dir, vtnum);

    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "supervisor: could not write recovery file %s: %s\n",
                path, strerror(errno));
        return;
    }
    fprintf(f,
            "{\"vt\":%d,\"pty_child_pid\":%d,\"timestamp\":%ld,\"reason\":\"%s\"}\n",
            vtnum, (int)pty_child_pid, (long)time(NULL), reason);
    fclose(f);
}

static void
wall_broadcast(int vtnum, const char *reason)
{
    if (getenv("GHOSTCON_DISABLE_WALL"))
        return;

    static time_t last_broadcast[2]; /* [0]=hang-class, [1]=startup-class -- see index below */
    int class_idx = (strcmp(reason, "hang") == 0) ? 0 : 1;
    time_t now = time(NULL);

    /* Test-only override -- a real rate limit test waiting out the
       actual 60s window would be needlessly slow; this exercises the
       exact same rate-limit logic against a much shorter window
       instead of changing what ships. Defaults to the real constant
       when unset. */
    long rate_limit_secs = WALL_RATE_LIMIT_SECS;
    const char *rate_override = getenv("GHOSTCON_WALL_RATE_LIMIT_SECS");
    if (rate_override)
        rate_limit_secs = atol(rate_override);

    if (now - last_broadcast[class_idx] < rate_limit_secs)
        return;
    last_broadcast[class_idx] = now;

    /* Test-only: substitute the real wall(1) exec with a log line so a
       test can observe how many times a broadcast WOULD have fired --
       unlike GHOSTCON_DISABLE_WALL above (a silent, no-side-effects
       skip that returns before any rate-limit bookkeeping at all, so
       it can never be used to test the rate limiter itself), this
       still runs the real rate-limit check above, it just doesn't
       spam real logged-in users with the result. */
    if (getenv("GHOSTCON_WALL_DEBUG_LOG")) {
        fprintf(stderr, "would wall-broadcast: reason=%s vt=%d\n", reason, vtnum);
        return;
    }

    pid_t pid = fork();
    if (pid < 0)
        return;
    if (pid == 0) {
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "ghostcon[tty%d]: renderer %s, recovering", vtnum, reason);
        execlp("wall", "wall", msg, (char *)NULL);
        _exit(127);
    }
    waitpid(pid, NULL, 0);
}

/* ------------------------------------------------------------------ */
/* Fallback tiers                                                      */
/* ------------------------------------------------------------------ */

static pid_t
spawn_kmscon_fallback(int vtnum)
{
    /* Test-only: force straight to the agetty tier for determinism --
       real kmscon behavior against a bogus test VT number is
       unverified and not worth risking in an automated test. */
    if (getenv("GHOSTCON_DISABLE_KMSCON_FALLBACK"))
        return -1;
    if (access("/usr/bin/kmscon", X_OK) != 0)
        return -1;

    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        char vt_arg[32];
        snprintf(vt_arg, sizeof(vt_arg), "--vt=tty%d", vtnum);
        /* Matches this system's real kmsconvt@.service invocation
           (verified: /usr/lib/systemd/system/kmsconvt@.service) rather
           than a guessed one. --no-switchvt: don't fight the VT switch
           that's presumably already in flight (or absent, if we're
           just starting this VT fresh). */
        execlp("kmscon", "kmscon", vt_arg, "--no-switchvt", "--login", "--",
               "/sbin/agetty", "-8", "-o", "-p -- \\u", "--noclear", "--", "-",
               (char *)NULL);
        _exit(127);
    }
    return pid;
}

static pid_t
spawn_agetty_fallback(int vtnum)
{
    if (access("/sbin/agetty", X_OK) != 0 && access("/usr/bin/agetty", X_OK) != 0)
        return -1;

    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        char tty_arg[16];
        snprintf(tty_arg, sizeof(tty_arg), "tty%d", vtnum);
        execlp("agetty", "agetty", tty_arg, "linux", (char *)NULL);
        _exit(127);
    }
    return pid;
}

/* Re-reads config_path and applies whichever fields this process cares
   about. disable_wall/disable_kmscon_fallback need no explicit handling
   here at all -- wall_broadcast()/spawn_kmscon_fallback() already call
   getenv() fresh at each point of use rather than caching a value at
   startup, so re-exporting the env (ghostcon_config_export_env) alone
   makes them live. canary_deadline_ms applies immediately (the state
   machine below re-reads it every poll() call). drm_node only applies
   on the NEXT spawn_renderer() call -- see drm_node_buf's own doc
   comment at its declaration in main() for why -- and never overrides
   an explicit argv[3], at reload same as at startup. */
static void
apply_config_reload(const char *config_path, int vtnum, bool drm_node_from_argv,
                     char *drm_node_buf, size_t drm_node_buf_len, int *canary_deadline_ms)
{
    ghostcon_config_t new_cfg;
    if (!ghostcon_config_load(config_path, &new_cfg)) {
        fprintf(stderr, "supervisor: vt %d: %s changed but failed to parse -- keeping previous values\n",
                vtnum, config_path);
        return;
    }
    ghostcon_config_export_env(&new_cfg, config_path, true);
    *canary_deadline_ms = new_cfg.canary_deadline_ms;
    if (!drm_node_from_argv)
        snprintf(drm_node_buf, drm_node_buf_len, "%s", new_cfg.drm_node);
    fprintf(stderr, "supervisor: vt %d: config changed, applied\n", vtnum);
}

/* Polls `state_fd` (canary_fd or the SIGHUP pipe, depending on which
   state calls this) for `timeout_ms`, transparently handling and
   consuming config hot-reload wakeups along the way so callers see
   exactly the same poll()-return semantics they had before config
   watching existed. Without this, a config-file-only wakeup (state_fd
   itself has nothing pending) would make poll() return rv>0 with
   state_fd's revents at 0 -- indistinguishable, to the three existing
   state-machine call sites below, from an actual timeout, which would
   incorrectly fire "renderer hung"/"did not claim VT in time" purely
   because someone saved ghostcon.toml. Instead: a config-only wakeup
   applies the reload and polls again rather than returning. */
static int
poll_state_fd(int state_fd, int timeout_ms, int config_watch_fd, const char *config_path,
              int vtnum, bool drm_node_from_argv, char *drm_node_buf, size_t drm_node_buf_len,
              int *canary_deadline_ms, short *out_revents)
{
    for (;;) {
        struct pollfd fds[2];
        fds[0] = (struct pollfd){ .fd = state_fd, .events = POLLIN };
        int nfds = 1;
        int watch_idx = -1;
        if (config_watch_fd >= 0) {
            watch_idx = 1;
            fds[1] = (struct pollfd){ .fd = config_watch_fd, .events = POLLIN };
            nfds = 2;
        }

        int rv = poll(fds, (nfds_t)nfds, timeout_ms);
        *out_revents = fds[0].revents;

        if (rv > 0 && watch_idx >= 0 && (fds[watch_idx].revents & POLLIN) && fds[0].revents == 0) {
            if (ghostcon_config_watch_check(config_watch_fd, config_path))
                apply_config_reload(config_path, vtnum, drm_node_from_argv,
                                     drm_node_buf, drm_node_buf_len, canary_deadline_ms);
            continue; /* config-only wakeup -- doesn't count, poll again */
        }
        return rv;
    }
}

/* ------------------------------------------------------------------ */
/* main                                                                 */
/* ------------------------------------------------------------------ */

int
main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <vtnum> [registry_socket] [drm_node]\n", argv[0]);
        return 2;
    }
    int vtnum = atoi(argv[1]);
    /* Always resolved to a concrete value (never NULL) before being
       handed to spawn_renderer -- see spawn_renderer's own comment on
       why a positional gap here would be a real bug, not just untidy. */
    const char *registry_socket = argc > 2 ? argv[2] : GHOSTCON_PTYSERV_SOCKET_PATH;
    /* argv takes priority; otherwise GHOSTCON_DRM_NODE env var (this
       machine's GPU is card1, not the common-case card0 default --
       found the hard way: DEFAULT_DRM_NODE alone left ghostcon-core
       unable to open any DRM device, silently never rendering while
       the display just kept showing whatever was on screen at the VT
       switch). undead-head doesn't thread a drm_node argv through to
       supervisor (deliberately -- see fork_supervisor's own comment),
       so this env var is the actual override path when launched via
       undead-head; note pkexec resets the environment, so it must be
       set via `pkexec env GHOSTCON_DRM_NODE=... CMD`, not a plain
       `VAR=val pkexec CMD` prefix (that only sets it in the calling
       shell, never reaching the exec'd program). */
    /* drm_node is a mutable buffer (not a plain pointer into argv/getenv
       storage) so a config hot-reload can update it in place -- only
       takes effect on the NEXT spawn_renderer() call, since an
       already-running ghostcon-core has an already-open EGL/GBM context
       on the OLD device; see PLAN.md's "General config hot-reload"
       section for why that's an accepted limitation, not a bug. argv
       always wins over the file, at startup AND on every reload --
       drm_node_from_argv tracks that so the reload handler below knows
       not to clobber an explicit override. */
    bool drm_node_from_argv = argc > 3;
    char drm_node_buf[256];
    if (drm_node_from_argv) {
        snprintf(drm_node_buf, sizeof(drm_node_buf), "%s", argv[3]);
    } else {
        const char *drm_node_env = getenv("GHOSTCON_DRM_NODE");
        snprintf(drm_node_buf, sizeof(drm_node_buf), "%s",
                 (drm_node_env && *drm_node_env) ? drm_node_env : DEFAULT_DRM_NODE);
    }
    const char *drm_node = drm_node_buf;

    const char *deadline_str = getenv("GHOSTCON_CANARY_DEADLINE_MS");
    int canary_deadline_ms = deadline_str ? atoi(deadline_str) : DEFAULT_CANARY_DEADLINE_MS;

    /* Hot-reload: GHOSTCON_CONFIG_PATH is exported unconditionally by
       undead-head (see config.h's doc comment) regardless of whether
       THIS process needed a config file before -- watch it directly,
       independent of undead-head, same reasoning as every other
       process in this tree per PLAN.md's "General config hot-reload"
       section (no reload-propagation IPC between processes, each just
       re-reads the same file). Not fatal if either step fails --
       hot-reload just silently doesn't happen. */
    const char *config_path = getenv("GHOSTCON_CONFIG_PATH");
    if (!config_path)
        config_path = "/etc/ghostcon/ghostcon.toml";
    int config_watch_fd = ghostcon_config_watch_open(config_path);

    /* PLAN.md: "Each supervisor calls setpgid(0, 0) at startup so its
       renderer and overlay children belong to a single group that
       undead-head can kill atomically." */
    setpgid(0, 0);

    if (pipe(g_sighup_pipe) != 0) {
        perror("supervisor: pipe");
        return 1;
    }
    fcntl(g_sighup_pipe[0], F_SETFL, O_NONBLOCK);
    fcntl(g_sighup_pipe[1], F_SETFL, O_NONBLOCK);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sighup_handler;
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGHUP, &sa, NULL);

    state_t state = ST_SPAWNING;
    pid_t child_pid = -1;   /* renderer (SPAWNING/ACTIVE) or fallback (FALLBACK) */
    int canary_fd = -1;     /* only meaningful in SPAWNING/ACTIVE */

    for (;;) {
        switch (state) {
        case ST_SPAWNING: {
            child_pid = spawn_renderer(vtnum, registry_socket, drm_node, &canary_fd);
            if (child_pid < 0) {
                fprintf(stderr, "supervisor: vt %d: failed to spawn renderer, retrying in 1s\n", vtnum);
                sleep(1);
                continue;
            }

            short revents;
            int rv = poll_state_fd(canary_fd, canary_deadline_ms, config_watch_fd, config_path,
                                    vtnum, drm_node_from_argv, drm_node_buf, sizeof(drm_node_buf),
                                    &canary_deadline_ms, &revents);
            if (rv > 0 && (revents & POLLIN)) {
                uint8_t byte;
                read(canary_fd, &byte, 1);
                fprintf(stderr, "supervisor: vt %d: renderer claimed VT, ACTIVE\n", vtnum);
                state = ST_ACTIVE;
            } else {
                /* Startup race timer expired, or the renderer exited/
                   crashed before ever heartbeating (POLLHUP / rv==0
                   both land here -- doesn't matter which, "no heartbeat
                   in time" is the only thing that matters per the
                   deliberately-dumb canary philosophy). */
                fprintf(stderr, "supervisor: vt %d: renderer did not claim VT in time, falling back\n", vtnum);
                kill_and_reap(child_pid, canary_fd);
                canary_fd = -1;
                write_recovery_file(vtnum, query_pty_child_pid(registry_socket, vtnum), "startup_timeout");
                wall_broadcast(vtnum, "failed to start");

                child_pid = spawn_kmscon_fallback(vtnum);
                if (child_pid < 0)
                    child_pid = spawn_agetty_fallback(vtnum);
                if (child_pid < 0) {
                    fprintf(stderr, "supervisor: vt %d: no fallback available either, retrying ghostcon in 1s\n", vtnum);
                    sleep(1);
                    continue;
                }
                state = ST_FALLBACK;
            }
            break;
        }

        case ST_ACTIVE: {
            short revents;
            int rv = poll_state_fd(canary_fd, canary_deadline_ms, config_watch_fd, config_path,
                                    vtnum, drm_node_from_argv, drm_node_buf, sizeof(drm_node_buf),
                                    &canary_deadline_ms, &revents);
            if (rv > 0 && (revents & POLLHUP)) {
                fprintf(stderr, "supervisor: vt %d: renderer exited, respawning\n", vtnum);
                close(canary_fd);
                canary_fd = -1;
                waitpid(child_pid, NULL, 0);
                state = ST_SPAWNING;
            } else if (rv > 0 && (revents & POLLIN)) {
                /* POLLIN guarantees at least one byte is available, so
                   a single read is safe without blocking -- looping
                   read() until it returns <=0 is wrong here: on a
                   blocking stream socket that's still open, the read
                   AFTER the last available byte blocks waiting for
                   more data instead of returning 0 (that only happens
                   on EOF), so a "drain until empty" loop like that
                   hangs forever the moment the renderer actually goes
                   silent -- exactly the case this poll loop exists to
                   detect. Any coalesced extra heartbeat bytes just get
                   left for the next iteration's read; discarding them
                   isn't needed since we don't count bytes, only
                   liveness. Found via test_supervisor.c's hang
                   scenario never completing. */
                uint8_t buf[64];
                read(canary_fd, buf, sizeof(buf));
            } else {
                fprintf(stderr, "supervisor: vt %d: renderer hung (canary silent), killing and retrying\n", vtnum);
                kill_and_reap(child_pid, canary_fd);
                canary_fd = -1;
                write_recovery_file(vtnum, query_pty_child_pid(registry_socket, vtnum), "hang");
                wall_broadcast(vtnum, "hang");
                state = ST_SPAWNING; /* retry ghostcon, NOT fallback -- see file header */
            }
            break;
        }

        case ST_FALLBACK: {
            short revents;
            int rv = poll_state_fd(g_sighup_pipe[0], -1, config_watch_fd, config_path,
                                    vtnum, drm_node_from_argv, drm_node_buf, sizeof(drm_node_buf),
                                    &canary_deadline_ms, &revents);
            (void)revents; /* this state only cares that rv>0 happened, not which bit */
            if (rv > 0) {
                drain_sighup_pipe();
                fprintf(stderr, "supervisor: vt %d: SIGHUP received, retrying ghostcon\n", vtnum);
                if (child_pid > 0) {
                    kill(child_pid, SIGTERM);
                    waitpid(child_pid, NULL, 0);
                }
                state = ST_SPAWNING;
            }
            break;
        }
        }
    }
}
