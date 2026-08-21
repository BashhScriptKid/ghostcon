/*
 * fake_renderer — stand-in for ghostcon-core, used only by
 * test_supervisor.c/test_undead_head.c. Speaks just enough of the
 * canary protocol (a byte per "frame" on GHOSTCON_CANARY_FD) to drive
 * supervisor's state machine through each scenario, without needing
 * real DRM/KMS/VT hardware.
 *
 * Behavior selected via env vars (all optional):
 *   FAKE_RENDERER_NEVER_HEARTBEAT - never write anything; just sleep.
 *     Drives the startup-timeout -> fallback path.
 *   FAKE_RENDERER_HANG_AFTER_MS=N - heartbeat normally, then go silent
 *     (sleep forever) after N ms. Drives the ACTIVE -> hang -> respawn
 *     path.
 *   FAKE_RENDERER_EXIT_AFTER_MS=N - heartbeat normally, then exit(0)
 *     cleanly after N ms. Drives the ACTIVE -> clean-exit -> respawn
 *     path.
 *   (none set) - heartbeat forever, simulating a healthy renderer.
 *
 *   FAKE_RENDERER_REAL_PTY=1 - also actually connect to ghost-ptyserv
 *     via core/transport.c, the same code path real ghostcon-core
 *     uses (argv[1]=vtnum, argv[3]=registry_socket, matching
 *     supervisor's own spawn_renderer() argv order -- argv[2]=drm_node
 *     is accepted but ignored). Needed to test anything beyond the
 *     canary protocol itself: a hang/respawn cycle only actually
 *     exercises pty-child survival, recovery-file pid correctness, and
 *     ring-buffer replay if SOMETHING real is on the other end of that
 *     connection -- without this, there is no pty child at all, so
 *     those parts of the induced-hang checklist (PLAN.md's Testing
 *     strategy section) are unobservable by construction, not just
 *     untested. Without FAKE_RENDERER_REAL_PTY, behavior is unchanged
 *     from before this existed.
 *   FAKE_RENDERER_WRITE_MARKER="text" - once connected, write this
 *     string over the transport (i.e. as if a user typed it into the
 *     live shell) before entering the heartbeat loop. Used to plant a
 *     known, later-greppable marker in the pty child's ring buffer.
 *   FAKE_RENDERER_CHECK_MARKER="text" - once connected, read whatever
 *     arrives within a 1s window (the ring buffer replay burst a
 *     reconnect immediately receives -- see transport.h's own doc
 *     comment on ghostcon_transport_connect()) and report via stderr
 *     whether `text` appeared in it: "REPLAY: marker found" or
 *     "REPLAY: marker MISSING". This is how a second fake_renderer
 *     instance (the one supervisor spawns automatically after
 *     detecting a hang) proves the replacement renderer actually
 *     recovered the prior session's output, not just that a new pty
 *     child happened to exist.
 *   On a successful FAKE_RENDERER_REAL_PTY connect, also prints
 *   "PTY_CHILD_PID: <n>" to stderr (from ghostcon_transport_t's own
 *   pty_child_pid field) so a test can compare it against what the
 *   recovery file recorded.
 */

#define _DEFAULT_SOURCE

#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "ghostcon/core/transport.h"

#define HEARTBEAT_INTERVAL_MS 100

static long
now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

int
main(int argc, char **argv)
{
    const char *fd_str = getenv("GHOSTCON_CANARY_FD");
    int canary_fd = fd_str ? atoi(fd_str) : -1;

    if (getenv("FAKE_RENDERER_REAL_PTY")) {
        int vtnum = argc > 1 ? atoi(argv[1]) : 0;
        const char *registry_socket = argc > 3 ? argv[3] : NULL;
        ghostcon_transport_t transport = {0};
        if (registry_socket && ghostcon_transport_connect(&transport, registry_socket, vtnum)) {
            fprintf(stderr, "PTY_CHILD_PID: %d\n", transport.pty_child_pid);

            /* Check BEFORE write, deliberately: this only drains
               whatever ghost-ptyserv's ring-buffer replay hands over
               immediately on connect (see transport.h's own doc
               comment on ghostcon_transport_connect()) -- i.e. output
               from a PRIOR instance's session, not an echo of this
               instance's own write below. That ordering is what makes
               a "found" here actual proof of session continuity
               across a hang/respawn cycle, not just proof that some
               shell somewhere responds to input. */
            const char *check = getenv("FAKE_RENDERER_CHECK_MARKER");
            if (check) {
                char buf[8192];
                size_t total = 0;
                bool found = false;
                long deadline = now_ms() + 1000;
                while (now_ms() < deadline && total < sizeof(buf) - 1) {
                    struct pollfd pfd = { .fd = transport.fd, .events = POLLIN };
                    int rv = poll(&pfd, 1, 100);
                    if (rv <= 0)
                        continue;
                    if (!(pfd.revents & POLLIN))
                        break;
                    ssize_t n = ghostcon_transport_read(&transport, (uint8_t *)buf + total, sizeof(buf) - 1 - total);
                    if (n <= 0)
                        break;
                    total += (size_t)n;
                    buf[total] = '\0';
                    if (strstr(buf, check)) {
                        found = true;
                        break;
                    }
                }
                fprintf(stderr, found ? "REPLAY: marker found\n" : "REPLAY: marker MISSING\n");
            }

            const char *marker = getenv("FAKE_RENDERER_WRITE_MARKER");
            if (marker) {
                size_t len = strlen(marker);
                ssize_t ignored = ghostcon_transport_write(&transport, (const uint8_t *)marker, len);
                (void)ignored;
            }
        } else {
            fprintf(stderr, "REAL_PTY: connect failed\n");
        }
    }

    if (getenv("FAKE_RENDERER_NEVER_HEARTBEAT")) {
        for (;;)
            pause();
    }

    long hang_after = -1, exit_after = -1;
    if (getenv("FAKE_RENDERER_HANG_AFTER_MS"))
        hang_after = atol(getenv("FAKE_RENDERER_HANG_AFTER_MS"));
    if (getenv("FAKE_RENDERER_EXIT_AFTER_MS"))
        exit_after = atol(getenv("FAKE_RENDERER_EXIT_AFTER_MS"));

    long start = now_ms();
    for (;;) {
        long elapsed = now_ms() - start;

        if (exit_after >= 0 && elapsed >= exit_after)
            return 0;
        if (hang_after >= 0 && elapsed >= hang_after) {
            for (;;)
                pause();
        }

        if (canary_fd >= 0) {
            uint8_t byte = 1;
            ssize_t ignored = write(canary_fd, &byte, 1);
            (void)ignored;
        }
        usleep(HEARTBEAT_INTERVAL_MS * 1000);
    }
}
