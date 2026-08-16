/*
 * pty-ttyN — PTY child process.
 *
 * Holds one PTY master fd, spawns a login session, maintains a ring
 * buffer of output, and serves at most one connected renderer over a
 * Unix socket: on connect, replays the ring buffer, then forwards
 * bytes bidirectionally until the renderer disconnects. The session
 * never notices a renderer coming or going — output forwarding just
 * pauses, ring buffer keeps filling. See PLAN.md "ghost-ptyserv" /
 * IMPLEMENTATION continuation.
 *
 * The session is `agetty` (which itself hands off to `login(1)` for
 * the username/password prompt), not a raw shell -- matching how real
 * VTs work and how kmscon's own systemd unit does it ("--login --
 * /sbin/agetty ... - $TERM", `-` meaning "use the already-attached
 * line"). ghost-ptyserv/pty-ttyN run as root (the whole stack does,
 * under pkexec, until a real privilege-separated deployment exists),
 * so without this, whoever connects lands directly in a root shell
 * with no authentication at all -- found live, not caught by review.
 * When the session exits (logout), a fresh one is respawned
 * automatically (with backoff if it keeps failing immediately -- same
 * lesson as undead-head's own restart loop, PLAN.md Phase 1 item 7),
 * instead of this process just exiting and leaving the VT with nothing
 * attached to its PTY at all -- also found live ("exiting bails to
 * blank kernel state").
 *
 * GHOSTCON_PTY_SKIP_LOGIN=1 bypasses agetty and execs the raw shell
 * directly instead -- tests need this, since they assume immediate
 * shell access with no login prompt in the way.
 *
 * Usage: pty-ttyN <vtnum> <socket_path> [shell]
 */

#define _DEFAULT_SOURCE /* forkpty() is a glibc extension, hidden under -std=c11 without this */

#include "ghostcon/ptyserv/protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* Ring buffer — fixed size, overwrite-oldest                          */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t *data;
    size_t cap;
    size_t head; /* next write position */
    size_t len;  /* valid bytes, <= cap */
} ringbuf_t;

static void
ringbuf_init(ringbuf_t *rb, size_t cap)
{
    rb->data = malloc(cap);
    rb->cap = cap;
    rb->head = 0;
    rb->len = 0;
}

static void
ringbuf_append(ringbuf_t *rb, const uint8_t *buf, size_t n)
{
    if (n >= rb->cap) {
        buf += n - rb->cap;
        memcpy(rb->data, buf, rb->cap);
        rb->head = 0;
        rb->len = rb->cap;
        return;
    }
    size_t first = rb->cap - rb->head;
    if (first > n)
        first = n;
    memcpy(rb->data + rb->head, buf, first);
    if (n > first)
        memcpy(rb->data, buf + first, n - first);
    rb->head = (rb->head + n) % rb->cap;
    rb->len += n;
    if (rb->len > rb->cap)
        rb->len = rb->cap;
}

/* ------------------------------------------------------------------ */
/* I/O helpers                                                         */
/* ------------------------------------------------------------------ */

static bool
write_all(int fd, const uint8_t *buf, size_t n)
{
    while (n > 0) {
        ssize_t w = write(fd, buf, n);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        buf += w;
        n -= (size_t)w;
    }
    return true;
}

static bool
ringbuf_replay(ringbuf_t *rb, int fd)
{
    if (rb->len == 0)
        return true;
    size_t start = (rb->head + rb->cap - rb->len) % rb->cap;
    size_t first = rb->cap - start;
    if (first > rb->len)
        first = rb->len;
    if (!write_all(fd, rb->data + start, first))
        return false;
    if (rb->len > first) {
        if (!write_all(fd, rb->data, rb->len - first))
            return false;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Session spawn (agetty/login, or a raw shell for tests)              */
/* ------------------------------------------------------------------ */

static pid_t
spawn_session(const char *shell, int *out_master_fd)
{
    int master_fd;
    pid_t child = forkpty(&master_fd, NULL, NULL, NULL);
    if (child < 0) {
        perror("forkpty");
        return -1;
    }
    if (child == 0) {
        const char *skip_login = getenv("GHOSTCON_PTY_SKIP_LOGIN");
        if (skip_login && *skip_login) {
            execlp(shell, shell, (char *)NULL);
        } else {
            const char *term = getenv("TERM");
            if (!term || !*term) {
                /* NOT "linux" -- that's the bare Linux console's own
                   terminfo entry (minimal color, no xterm-style mouse
                   escapes), which undersells what ghostcon's
                   libghostty-vt engine actually implements (SGR/24-bit
                   color, X10/SGR mouse reporting, OSC 8) and visibly
                   degrades apps that query terminfo to decide what to
                   use -- found live: nano showed noticeably fewer
                   colors and no mouse response under TERM=linux
                   compared to the same session under kmscon. */
                term = "xterm-256color";
                /* term = "xterm-ghostty"; -- Ghostty itself defaults to
                   this (its own real terminfo entry, already installed
                   on this machine as the ghostty-terminfo package) --
                   but that entry advertises Ghostty's FULL feature set
                   (Kitty graphics protocol, certain OSC extensions)
                   which ghostcon doesn't implement yet (see PLAN.md's
                   OSC support matrix); claiming it before ghostcon's
                   coverage is actually close enough would make apps
                   attempt features that then silently misbehave, worse
                   than xterm-256color's more conservative but honest
                   feature set. Revisit once ghostcon's OSC/CSI coverage
                   is closer to real Ghostty's. */
            }
            /* "-" = use the already-attached line (our pty slave, via
               forkpty) instead of agetty opening a tty device itself --
               same convention kmscon's own unit file uses. */
            execlp("agetty", "agetty", "-8", "--noclear", "-", term, (char *)NULL);
        }
        perror("execlp session");
        _exit(127);
    }
    *out_master_fd = master_fd;
    return child;
}

/* ------------------------------------------------------------------ */
/* Unix socket server                                                  */
/* ------------------------------------------------------------------ */

static int
listen_unix(const char *path)
{
    unlink(path);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    if (listen(fd, 1) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* ------------------------------------------------------------------ */
/* main                                                                 */
/* ------------------------------------------------------------------ */
/* SIGCHLD self-pipe — see the long comment at its poll() call site for   */
/* why POLLHUP/read()<=0 on master_fd must NOT be treated as "child       */
/* died" (matches the self-pipe pattern already used by core/vtctl.c).   */
/* ------------------------------------------------------------------ */

static int g_chld_pipe[2] = { -1, -1 };

static long
monotonic_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void
chld_signal_handler(int signum)
{
    (void)signum;
    char c = 'C';
    ssize_t unused = write(g_chld_pipe[1], &c, 1);
    (void)unused;
}

int
main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <vtnum> <socket_path> [shell]\n", argv[0]);
        return 2;
    }

    int vtnum = atoi(argv[1]);
    const char *socket_path = argv[2];
    const char *shell = argc > 3 ? argv[3] : getenv("SHELL");
    if (!shell || !*shell)
        shell = "/bin/sh";

    /* Own process group, matching every other group leader in this tree
       (supervisor/main.c, undead_head/main.c's fork_ghost_ptyserv/
       fork_supervisor) -- otherwise this inherits ghost-ptyserv's group,
       and ghost-ptyserv dying takes this (and the live shell session
       inside it) down as collateral damage via undead-head's own
       kill_group() cleanup, even though this process has no actual
       runtime dependency on ghost-ptyserv once spawned (its socket is
       used directly by the renderer from then on). */
    setpgid(0, 0);

    signal(SIGPIPE, SIG_IGN);

    /* /bin/login calls vhangup() on its controlling TTY as part of its
       own password-prompt procedure -- confirmed against kmscon's own
       src/misc/pty.c, which has an explicit comment about exactly this:
       vhangup() causes a transient hangup condition on the PTY MASTER
       side even though the child has NOT actually exited yet (it's mid-
       procedure, about to reopen its controlling terminal and continue).
       Treating that as "the child died" -- as an earlier version of this
       file did, via a blocking waitpid() fired straight off POLLHUP/
       read()<=0 on master_fd -- blocks with nothing to reap until the
       child eventually exits for some unrelated reason (e.g. login's own
       ~60s LOGIN_TIMEOUT), freezing this single-threaded loop, including
       the live renderer connection, for the whole wait. Found live as a
       "stuck at password" symptom that self-resolved after ~60s. Fixed
       by only ever reaping via SIGCHLD (kmscon's own proven approach,
       via its sig_child() callback) -- never via read()<=0 on the pty. */
    if (pipe(g_chld_pipe) != 0) {
        fprintf(stderr, "pty-tty%d: pipe: %s\n", vtnum, strerror(errno));
        return 1;
    }
    fcntl(g_chld_pipe[0], F_SETFL, O_NONBLOCK);
    fcntl(g_chld_pipe[1], F_SETFL, O_NONBLOCK);

    struct sigaction chld_sa;
    memset(&chld_sa, 0, sizeof(chld_sa));
    chld_sa.sa_handler = chld_signal_handler;
    chld_sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigemptyset(&chld_sa.sa_mask);
    if (sigaction(SIGCHLD, &chld_sa, NULL) != 0) {
        fprintf(stderr, "pty-tty%d: sigaction(SIGCHLD): %s\n", vtnum, strerror(errno));
        return 1;
    }

    int master_fd;
    pid_t child = spawn_session(shell, &master_fd);
    if (child < 0)
        return 1;

    int listen_fd = listen_unix(socket_path);
    if (listen_fd < 0) {
        fprintf(stderr, "pty-tty%d: failed to listen on %s: %s\n",
                vtnum, socket_path, strerror(errno));
        kill(child, SIGHUP);
        waitpid(child, NULL, 0);
        return 1;
    }

    /* Resize control socket -- see protocol.h's own doc comment on
       GHOSTCON_PTY_CTL_SOCKET_FMT for why this is a separate socket
       rather than an in-band command on the data socket above. Not
       fatal if this fails to bind: the session still works, just
       without live TIOCSWINSZ propagation (the pre-existing behavior,
       unchanged) -- found live: TUI programs (e.g. a full-screen editor)
       rendered into a small, wrong-sized corner because nothing ever
       called TIOCSWINSZ on this pty's master at all, not even once at
       startup, so ioctl(TIOCGWINSZ) callers got whatever forkpty()'s
       bare default happened to be. */
    char dir_buf[GHOSTCON_PTYSERV_LINE_MAX];
    snprintf(dir_buf, sizeof(dir_buf), "%s", socket_path);
    char ctl_path[GHOSTCON_PTYSERV_LINE_MAX];
    snprintf(ctl_path, sizeof(ctl_path), GHOSTCON_PTY_CTL_SOCKET_FMT, dirname(dir_buf), vtnum);
    int ctl_listen_fd = listen_unix(ctl_path);
    if (ctl_listen_fd < 0)
        fprintf(stderr, "pty-tty%d: failed to listen on %s: %s (resizes won't propagate)\n",
                vtnum, ctl_path, strerror(errno));
    int ctl_fd = -1; /* accepted control connection, if any */

    ringbuf_t rb;
    ringbuf_init(&rb, GHOSTCON_PTY_RINGBUF_DEFAULT_SIZE);

    int renderer_fd = -1;
    uint8_t buf[4096];

    /* Respawn backoff, same reasoning as undead-head's own restart loop
       (PLAN.md Phase 1 item 7): a session that keeps dying instantly
       (e.g. agetty missing, or misconfigured) must not spin tightly
       forever respawning. Non-blocking: a plain sleep() here was tried
       first and found live to be a real bug, not just an implementation
       detail -- this is a SINGLE-THREADED event loop that also services
       the renderer connection (every keystroke the user types), so a
       blocking sleep() (up to 30s, per the cap below) froze the entire
       session, not just the respawn: keystrokes typed during that
       window sat unread in the renderer socket's kernel buffer instead
       of being forwarded to the (not-yet-existing) new shell, then all
       flushed at once the moment the sleep ended, indistinguishable
       from the terminal being frozen and abruptly "catching up." Found
       via a live repro where a typed password appeared to vanish, then
       showed up in plaintext ~57s later — this pty-ttyN process had
       accumulated a non-trivial backoff value across many quick
       failed-login deaths earlier in the same debugging session, since
       it (correctly, per the ghost-ptyserv reliability fix) persists
       across everything except its own shell dying. Fixed by tracking
       a respawn deadline instead of blocking: master_fd is left out of
       the pollfd set while a respawn is pending (it's stale/at-EOF
       anyway), and poll()'s own timeout parameter -- not sleep() --
       is what makes the loop wait, so listen_fd/renderer_fd stay fully
       serviced the whole time. */
    time_t last_death = 0;
    int backoff = 0;
    bool awaiting_respawn = false;
    time_t respawn_after = 0;

    /* Cooldown for a transient hangup on master_fd (e.g. login's own
       vhangup(), see the big comment above) that is NOT an actual child
       death: poll() is level-triggered, so if we kept master_fd in the
       set while the condition persists, POLLHUP would fire again on
       every single iteration and this loop would busy-spin at 100% CPU
       (kmscon avoids this the same way, just via edge-triggered epoll
       instead -- EV_ET isn't available through plain poll()). Instead,
       briefly stop polling master_fd and retry after a short delay.
       Millisecond resolution (CLOCK_MONOTONIC, not time_t) and
       exponential (20ms doubling to a 500ms cap, reset on the next
       successful read) rather than a flat 1s: a flat whole-second
       cooldown was tried first and found live to add a noticeable,
       user-visible lag to every login even though the freeze itself
       was fixed -- most hangups are much shorter than 1s in practice
       (the vhangup()-then-reopen sequence is typically near-instant),
       so starting short and only backing off if it's still not done
       keeps the common case snappy while still bounding worst-case
       CPU spin the same way the flat version did. */
    bool master_quiet = false;
    long master_quiet_ms = 0;
    long master_quiet_until_ms = 0;

    for (;;) {
        if (awaiting_respawn && time(NULL) >= respawn_after) {
            /* The old master_fd (from the session that just died) is
               never read again past this point -- close it before it's
               overwritten below. Left open, the kernel's devpts can't
               free/reuse that pts number (it allocates the lowest
               currently-unused slot), so every respawn permanently
               leaked one pts allocation, pushing each new login to a
               higher pts number forever (found live: pts/3 -> pts/18
               across one boot's worth of logout/login cycles). */
            close(master_fd);
            child = spawn_session(shell, &master_fd);
            if (child < 0) {
                fprintf(stderr, "pty-tty%d: failed to respawn session, giving up\n", vtnum);
                break;
            }
            awaiting_respawn = false;
        }
        if (master_quiet && monotonic_ms() >= master_quiet_until_ms)
            master_quiet = false;

        struct pollfd fds[6];
        int nfds = 0;

        int master_idx = -1;
        if (!awaiting_respawn && !master_quiet) {
            master_idx = nfds;
            fds[nfds++] = (struct pollfd){ .fd = master_fd, .events = POLLIN };
        }
        int listen_idx = nfds;
        fds[nfds++] = (struct pollfd){ .fd = listen_fd, .events = POLLIN };
        int renderer_idx = -1;
        if (renderer_fd >= 0) {
            renderer_idx = nfds;
            fds[nfds++] = (struct pollfd){ .fd = renderer_fd, .events = POLLIN };
        }
        int ctl_listen_idx = -1;
        if (ctl_listen_fd >= 0) {
            ctl_listen_idx = nfds;
            fds[nfds++] = (struct pollfd){ .fd = ctl_listen_fd, .events = POLLIN };
        }
        int ctl_idx = -1;
        if (ctl_fd >= 0) {
            ctl_idx = nfds;
            fds[nfds++] = (struct pollfd){ .fd = ctl_fd, .events = POLLIN };
        }
        int chld_idx = nfds;
        fds[nfds++] = (struct pollfd){ .fd = g_chld_pipe[0], .events = POLLIN };

        int poll_timeout_ms = -1;
        if (awaiting_respawn) {
            time_t remaining = respawn_after - time(NULL);
            poll_timeout_ms = remaining > 0 ? (int)(remaining * 1000) : 0;
        } else if (master_quiet) {
            long remaining = master_quiet_until_ms - monotonic_ms();
            poll_timeout_ms = remaining > 0 ? (int)remaining : 0;
        }

        int rv = poll(fds, (nfds_t)nfds, poll_timeout_ms);
        if (rv < 0) {
            if (errno == EINTR)
                continue;
            break;
        }

        if (chld_idx >= 0 && (fds[chld_idx].revents & POLLIN)) {
            char discard[64];
            while (read(g_chld_pipe[0], discard, sizeof(discard)) > 0)
                ; /* drain -- the byte content doesn't matter, only the wakeup */

            pid_t reaped;
            int status;
            while ((reaped = waitpid(-1, &status, WNOHANG)) > 0) {
                if (reaped != child)
                    continue; /* not our session (shouldn't happen, but not our death either) */

                /* This IS a real death, confirmed by the kernel via
                   SIGCHLD/waitpid -- unlike master_fd's POLLHUP, this
                   signal cannot fire spuriously while the child is still
                   alive. Respawn a fresh session rather than exiting this
                   process and leaving the VT's PTY with nothing attached
                   at all (found live: "exiting bails it to blank kernel
                   state"). The renderer stays connected throughout;
                   it'll just see the new login prompt appear as ordinary
                   output. */
                /* Tell the renderer this session's screen owner is gone,
                   over the control socket (same connection RESIZE
                   arrives on, opposite direction) -- ghostcon-core
                   decides whether to act on it (GHOSTCON_CLEAR_ON_LOGOUT).
                   Best-effort: no ctl connection or a failed write just
                   means the old screen lingers under the new login
                   prompt, same as today's behavior. */
                if (ctl_fd >= 0) {
                    char clear_msg[16];
                    size_t clear_len = ghostcon_ptyserv_format_clear(clear_msg, sizeof(clear_msg));
                    if (clear_len > 0)
                        write_all(ctl_fd, (const uint8_t *)clear_msg, clear_len);
                }

                master_quiet = false;
                master_quiet_ms = 0;
                time_t now = time(NULL);
                if (last_death != 0 && (now - last_death) >= 10)
                    backoff = 0; /* previous session ran long enough to trust it worked */
                last_death = now;
                if (backoff > 0)
                    fprintf(stderr, "pty-tty%d: session died again quickly, backing off %ds\n",
                            vtnum, backoff);
                awaiting_respawn = true;
                respawn_after = now + backoff;

                backoff = (backoff == 0) ? 1 : backoff * 2;
                if (backoff > 30)
                    backoff = 30;
            }
        }

        if (master_idx >= 0 && (fds[master_idx].revents & (POLLIN | POLLHUP | POLLERR))) {
            ssize_t r = read(master_fd, buf, sizeof(buf));
            if (r <= 0) {
                /* NOT necessarily death -- see the vhangup() comment
                   above main(). The child's actual death is detected
                   exclusively via the SIGCHLD self-pipe handled above;
                   this branch just backs off polling master_fd briefly
                   so a persistent (but transient) hangup condition
                   doesn't busy-spin the loop, then retries. */
                master_quiet = true;
                master_quiet_ms = (master_quiet_ms == 0) ? 20 : master_quiet_ms * 2;
                if (master_quiet_ms > 500)
                    master_quiet_ms = 500;
                master_quiet_until_ms = monotonic_ms() + master_quiet_ms;
                continue;
            }
            master_quiet_ms = 0; /* master healthy again -- forget any prior backoff */
            ringbuf_append(&rb, buf, (size_t)r);
            if (renderer_fd >= 0 && !write_all(renderer_fd, buf, (size_t)r)) {
                close(renderer_fd);
                renderer_fd = -1;
            }
        }

        if (fds[listen_idx].revents & POLLIN) {
            int new_fd = accept(listen_fd, NULL, NULL);
            if (new_fd >= 0) {
                /* Only one renderer at a time — a new connection (e.g.
                   supervisor restarted a hung renderer) replaces the old. */
                if (renderer_fd >= 0)
                    close(renderer_fd);
                renderer_fd = new_fd;
                if (!ringbuf_replay(&rb, renderer_fd)) {
                    close(renderer_fd);
                    renderer_fd = -1;
                }
            }
        }

        if (renderer_idx >= 0 &&
            (fds[renderer_idx].revents & (POLLIN | POLLHUP | POLLERR))) {
            ssize_t r = read(renderer_fd, buf, sizeof(buf));
            if (r <= 0) {
                close(renderer_fd);
                renderer_fd = -1;
            } else if (!awaiting_respawn) {
                /* Nowhere to deliver this while a respawn is pending
                   (master_fd is stale, the old session already died) --
                   drop it rather than write to a dead fd. Genuine
                   improvement over the old behavior, not a regression:
                   previously this same input just sat unread in the
                   renderer socket's kernel buffer for the entire
                   blocking sleep() and got delivered late/out of
                   context anyway once the shell existed again. */
                write_all(master_fd, buf, (size_t)r);
            }
        }

        if (ctl_listen_idx >= 0 && (fds[ctl_listen_idx].revents & POLLIN)) {
            int new_fd = accept(ctl_listen_fd, NULL, NULL);
            if (new_fd >= 0) {
                /* Only one control connection at a time, same rationale
                   as the data socket's renderer_fd above. */
                if (ctl_fd >= 0)
                    close(ctl_fd);
                ctl_fd = new_fd;
            }
        }

        if (ctl_idx >= 0 && (fds[ctl_idx].revents & (POLLIN | POLLHUP | POLLERR))) {
            char line[GHOSTCON_PTYSERV_LINE_MAX];
            ssize_t r = read(ctl_fd, line, sizeof(line) - 1);
            if (r <= 0) {
                close(ctl_fd);
                ctl_fd = -1;
            } else {
                line[r] = '\0';
                int rows, cols;
                if (ghostcon_ptyserv_parse_resize(line, &rows, &cols) &&
                    rows > 0 && cols > 0) {
                    /* The kernel automatically sends SIGWINCH to this
                       pty's foreground process group when TIOCSWINSZ
                       actually changes the size -- no manual kill()
                       needed, and guessing the right process group to
                       target manually would be less correct than
                       letting the kernel's own foreground-group tracking
                       handle it (matters once job control is in play:
                       the foreground group isn't always just `child`). */
                    struct winsize ws;
                    memset(&ws, 0, sizeof(ws));
                    ws.ws_row = (unsigned short)rows;
                    ws.ws_col = (unsigned short)cols;
                    ioctl(master_fd, TIOCSWINSZ, &ws);
                }
            }
        }
    }

    if (renderer_fd >= 0)
        close(renderer_fd);
    close(listen_fd);
    unlink(socket_path);
    kill(child, SIGHUP);
    waitpid(child, NULL, 0);
    return 0;
}
