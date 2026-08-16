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
 * Ignores argv entirely (vtnum/drm_node/registry_socket) -- it never
 * touches real hardware or ghost-ptyserv.
 */

#define _DEFAULT_SOURCE

#include <stdint.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#define HEARTBEAT_INTERVAL_MS 100

static long
now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

int
main(void)
{
    const char *fd_str = getenv("GHOSTCON_CANARY_FD");
    int canary_fd = fd_str ? atoi(fd_str) : -1;

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
