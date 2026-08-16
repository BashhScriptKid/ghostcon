/*
 * undead-head — process-group anchor, the single process systemd starts
 * (ghostcon.service). Forks ghost-ptyserv and one supervisor per VT, then
 * enters a waitpid() reaper/restart loop. PLAN.md "undead-head":
 *
 *   - ghost-ptyserv dies -> kill everything (every supervisor's whole
 *     process group, since without ghost-ptyserv no renderer can reach
 *     its pty child anyway) -> restart the entire tree.
 *   - a supervisor dies -> kill just its own process group (renderer +
 *     future overlay) -> restart that one supervisor.
 *
 * No PTY data, renderer state, IPC, or KMS/DRM lives here -- just fork,
 * waitpid, and process-group signal delivery. If this process did any of
 * those things, it would reintroduce a single point of failure
 * structurally identical to the problem this whole layer exists to solve.
 *
 * Env var overrides:
 *   GHOSTCON_PTYSERV_BIN    - path to ghost-ptyserv (default: resolved
 *                             relative to this binary's own location)
 *   GHOSTCON_SUPERVISOR_BIN - path to supervisor (same default rule)
 *   GHOSTCON_CONFIG_PATH    - path to the TOML config file this process
 *                             loads and re-exports as GHOSTCON_* env vars
 *                             for the whole tree (default:
 *                             /etc/ghostcon/ghostcon.toml, missing file is
 *                             not an error). See config.h's doc comment --
 *                             this is the only binary that parses the
 *                             config file at all; everything downstream
 *                             still just reads getenv() as before.
 *
 * Default resolution deliberately does NOT fall back to a bare name +
 * PATH lookup: this is normally launched via `pkexec`, which resets the
 * environment (including PATH) for security -- a bare "ghost-ptyserv"
 * silently fails to exec under pkexec even though it works fine in a
 * normal shell, and env var overrides don't survive pkexec either.
 * Found the hard way: undead-head spun in a fast fork/exec-fail/restart
 * loop under pkexec before this existed. Resolving siblings via
 * /proc/self/exe's own directory works regardless of PATH/environment.
 *
 * Usage: undead-head <registry_socket> <vtnum> [vtnum...]
 */

#define _DEFAULT_SOURCE

#include "ghostcon/config/config.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_VTS 64

/* Restart backoff: if a restart happens within RESTART_STABLE_SECS of
   the previous one, something is dying too fast to actually be useful
   (a persistent misconfiguration, not a transient blip), and blindly
   restarting instantly is dangerous, not just wasteful -- each restart
   of ghost-ptyserv tears down and rebuilds EVERY supervisor's tree,
   including a fresh ghostcon-core doing a fresh vtctl_open()/claim/
   close() on its VT with no pause between cycles. A tight loop of that
   is a rapid-fire VT_PROCESS acquire/release pattern the kernel isn't
   necessarily built to survive gracefully -- found the hard way: this
   wedged VT switching badly enough that even Ctrl+Alt+Fn couldn't
   recover it, needing a hard reboot, while an earlier version of
   ghost-ptyserv was dying near-instantly on every restart attempt.
   Doubling backoff (capped) makes a persistently-broken child restart
   slower and slower rather than spinning tightly forever; surviving
   RESTART_STABLE_SECS resets it, since that's long enough to trust the
   restart actually worked. */
#define RESTART_STABLE_SECS 10
#define RESTART_BACKOFF_BASE_SECS 1
#define RESTART_BACKOFF_MAX_SECS 30

static void
apply_restart_backoff(time_t *last_death, int *backoff, const char *what)
{
    time_t now = time(NULL);
    if (*last_death != 0 && (now - *last_death) >= RESTART_STABLE_SECS)
        *backoff = 0; /* previous instance was stable long enough; forget backoff */
    *last_death = now;

    if (*backoff > 0) {
        fprintf(stderr, "undead-head: %s died again quickly, backing off %ds before restart\n",
                what, *backoff);
        sleep((unsigned)*backoff);
    }

    *backoff = (*backoff == 0) ? RESTART_BACKOFF_BASE_SECS : *backoff * 2;
    if (*backoff > RESTART_BACKOFF_MAX_SECS)
        *backoff = RESTART_BACKOFF_MAX_SECS;
}

/* Resolves `name` relative to this executable's own directory (via
   /proc/self/exe), for finding sibling binaries without depending on
   PATH -- see the file header comment on why that can't be trusted
   here. Falls back to the bare name (PATH lookup) only if /proc/self/exe
   itself is unavailable. */
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

static volatile sig_atomic_t g_shutdown_requested = 0;

static void
term_handler(int signum)
{
    (void)signum;
    g_shutdown_requested = 1;
}

static pid_t
fork_ghost_ptyserv(const char *registry_socket, const int *vts, int n_vts)
{
    pid_t pid = fork();
    if (pid < 0) {
        perror("undead-head: fork ghost-ptyserv");
        return -1;
    }
    if (pid == 0) {
        setpgid(0, 0);

        const char *bin = getenv("GHOSTCON_PTYSERV_BIN");
        char bin_buf[4096];
        if (!bin || !*bin) {
            resolve_sibling_path(bin_buf, sizeof(bin_buf), "ghost-ptyserv");
            bin = bin_buf;
        }

        char *args[3 + MAX_VTS];
        char vt_bufs[MAX_VTS][16];
        int argi = 0;
        args[argi++] = (char *)bin;
        args[argi++] = (char *)registry_socket;
        for (int i = 0; i < n_vts; i++) {
            snprintf(vt_bufs[i], sizeof(vt_bufs[i]), "%d", vts[i]);
            args[argi++] = vt_bufs[i];
        }
        args[argi] = NULL;

        execvp(bin, args);
        perror("undead-head: execvp ghost-ptyserv");
        _exit(127);
    }
    return pid;
}

static pid_t
fork_supervisor(int vtnum, const char *registry_socket)
{
    pid_t pid = fork();
    if (pid < 0) {
        perror("undead-head: fork supervisor");
        return -1;
    }
    if (pid == 0) {
        /* supervisor calls setpgid(0,0) on itself too (PLAN.md), but
           doing it here as well is harmless (setpgid on an already-own
           leader is a no-op) and closes the race where undead-head
           might need getpgid() before the child's own call has run. */
        setpgid(0, 0);

        const char *bin = getenv("GHOSTCON_SUPERVISOR_BIN");
        char bin_buf[4096];
        if (!bin || !*bin) {
            resolve_sibling_path(bin_buf, sizeof(bin_buf), "supervisor");
            bin = bin_buf;
        }

        char vt_str[16];
        snprintf(vt_str, sizeof(vt_str), "%d", vtnum);
        /* supervisor's argv is <vtnum> [registry_socket] [drm_node] --
           registry_socket first specifically so undead-head (which has
           no opinion on drm_node, a supervisor-local hardware detail)
           can pass exactly what it has without a positional gap. */
        execlp(bin, bin, vt_str, registry_socket, (char *)NULL);
        perror("undead-head: execlp supervisor");
        _exit(127);
    }
    return pid;
}

static void
kill_group(pid_t pid)
{
    if (pid <= 0)
        return;
    /* Every group leader we spawn calls setpgid(0, 0), so its pgid
       always equals its own original pid -- kill(-pid, ...) directly,
       don't look it up via getpgid(pid) first. That lookup fails
       (ESRCH) exactly when it matters most: this is most often called
       right after waitpid() has already reaped `pid` (see the
       ghost-ptyserv-died branch below), so the process no longer
       exists to query -- but the group itself, and any orphaned
       grandchildren still in it (e.g. pty-ttyN, once its parent
       ghost-ptyserv died), are still very much alive and need this
       signal. Silently skipping the kill here left exactly those
       orphans running forever, holding the test harness's stdout pipe
       open long after the tracked process had already exited (found
       via test_undead_head.c timing out despite printing
       "ALL TESTS PASSED"). */
    kill(-pid, SIGTERM);
}

int
main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <registry_socket> <vtnum> [vtnum...]\n", argv[0]);
        return 2;
    }
    const char *registry_socket = argv[1];

    /* Load config, if any, and export it as GHOSTCON_* env vars for the
       whole tree (ghost-ptyserv/supervisor/ghostcon-core all just read
       getenv() as before -- see config.h's doc comment for why this is
       the one and only place that needs to know a config file exists).
       An already-set env var always wins, so this must run before
       anything else touches the environment. */
    ghostcon_config_t cfg;
    const char *config_path = getenv("GHOSTCON_CONFIG_PATH");
    if (!config_path)
        config_path = "/etc/ghostcon/ghostcon.toml";
    if (!ghostcon_config_load(config_path, &cfg))
        fprintf(stderr, "undead-head: %s exists but failed to parse -- using defaults\n", config_path);
    ghostcon_config_export_env(&cfg);

    int vts[MAX_VTS];
    int n_vts = 0;
    for (int i = 2; i < argc && n_vts < MAX_VTS; i++)
        vts[n_vts++] = atoi(argv[i]);

    /* Deliberately NOT SA_RESTART: the reaper loop below spends most of
       its time blocked in waitpid(-1, ...), and the shutdown flag this
       handler sets is only ever checked at the top of that loop. With
       SA_RESTART, the kernel transparently resumes the interrupted
       waitpid() call itself instead of returning EINTR to us -- control
       never comes back to the loop to notice the flag, so SIGTERM would
       be silently swallowed with the process just continuing to block
       (found via test_undead_head.c hanging on teardown). Without it,
       waitpid() returns -1/EINTR, our `if (errno == EINTR) continue`
       sends control back to the loop top, and the flag gets seen. */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = term_handler;
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    pid_t ghost_ptyserv_pid;
    pid_t supervisor_pid[MAX_VTS];

    time_t ptyserv_last_death = 0;
    int ptyserv_backoff = 0;
    time_t supervisor_last_death[MAX_VTS] = {0};
    int supervisor_backoff[MAX_VTS] = {0};

    ghost_ptyserv_pid = fork_ghost_ptyserv(registry_socket, vts, n_vts);
    for (int i = 0; i < n_vts; i++)
        supervisor_pid[i] = fork_supervisor(vts[i], registry_socket);

    for (;;) {
        if (g_shutdown_requested) {
            kill_group(ghost_ptyserv_pid);
            for (int i = 0; i < n_vts; i++)
                kill_group(supervisor_pid[i]);
            /* Reap everything we can without blocking indefinitely on a
               child that's ignoring SIGTERM -- systemd will SIGKILL the
               whole cgroup on unit stop timeout regardless. */
            while (waitpid(-1, NULL, WNOHANG) > 0)
                ;
            return 0;
        }

        int status;
        pid_t pid = waitpid(-1, &status, 0);
        if (pid < 0) {
            if (errno == EINTR)
                continue;
            if (errno == ECHILD) {
                /* Nothing left to wait on -- shouldn't normally happen
                   outside shutdown, but don't spin if it does. */
                sleep(1);
                continue;
            }
            break;
        }

        if (pid == ghost_ptyserv_pid) {
            /* Does NOT touch any supervisor. An already-connected
               renderer has no ongoing dependency on ghost-ptyserv --
               ghostcon_transport_connect() (core/transport.c) only uses
               the registry socket for a one-shot lookup at startup, then
               talks directly to its own pty-tty<N>.sock from then on.
               kill_group() here only cleans up ghost-ptyserv's own
               process -- with pty_child.c now calling its own
               setpgid(0,0), this no longer takes any live pty-ttyN (and
               the shell/login session inside it) down as collateral
               damage the way it used to. A restarted ghost-ptyserv
               recognizes and reuses those already-running pty-ttyN
               children instead of spawning duplicates (see
               ptyserv/main.c's spawn_pty_child() pidfile-liveness
               check) -- found live: one component dying used to mean
               losing every VT's session, not just the affected one. */
            fprintf(stderr, "undead-head: ghost-ptyserv died, restarting it (supervisors unaffected)\n");
            kill_group(ghost_ptyserv_pid);
            while (waitpid(-1, NULL, WNOHANG) > 0)
                ; /* non-blocking: supervisors are still running, must not wait on them here */
            apply_restart_backoff(&ptyserv_last_death, &ptyserv_backoff, "ghost-ptyserv");
            ghost_ptyserv_pid = fork_ghost_ptyserv(registry_socket, vts, n_vts);
            continue;
        }

        for (int i = 0; i < n_vts; i++) {
            if (pid == supervisor_pid[i]) {
                fprintf(stderr, "undead-head: supervisor for vt %d died, restarting it\n", vts[i]);
                kill_group(supervisor_pid[i]);
                apply_restart_backoff(&supervisor_last_death[i], &supervisor_backoff[i], "supervisor");
                supervisor_pid[i] = fork_supervisor(vts[i], registry_socket);
                break;
            }
        }
    }

    return 1;
}
