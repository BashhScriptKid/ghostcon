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
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

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
    if (now - last_broadcast[class_idx] < WALL_RATE_LIMIT_SECS)
        return;
    last_broadcast[class_idx] = now;

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
    const char *drm_node_env = getenv("GHOSTCON_DRM_NODE");
    const char *drm_node = argc > 3 ? argv[3]
                          : (drm_node_env && *drm_node_env) ? drm_node_env
                          : DEFAULT_DRM_NODE;

    const char *deadline_str = getenv("GHOSTCON_CANARY_DEADLINE_MS");
    int canary_deadline_ms = deadline_str ? atoi(deadline_str) : DEFAULT_CANARY_DEADLINE_MS;

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

            struct pollfd pfd = { .fd = canary_fd, .events = POLLIN };
            int rv = poll(&pfd, 1, canary_deadline_ms);
            if (rv > 0 && (pfd.revents & POLLIN)) {
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
                write_recovery_file(vtnum, -1, "startup_timeout");
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
            struct pollfd pfd = { .fd = canary_fd, .events = POLLIN };
            int rv = poll(&pfd, 1, canary_deadline_ms);
            if (rv > 0 && (pfd.revents & POLLHUP)) {
                fprintf(stderr, "supervisor: vt %d: renderer exited, respawning\n", vtnum);
                close(canary_fd);
                canary_fd = -1;
                waitpid(child_pid, NULL, 0);
                state = ST_SPAWNING;
            } else if (rv > 0 && (pfd.revents & POLLIN)) {
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
                write_recovery_file(vtnum, -1, "hang");
                wall_broadcast(vtnum, "hang");
                state = ST_SPAWNING; /* retry ghostcon, NOT fallback -- see file header */
            }
            break;
        }

        case ST_FALLBACK: {
            struct pollfd pfd = { .fd = g_sighup_pipe[0], .events = POLLIN };
            int rv = poll(&pfd, 1, -1);
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
