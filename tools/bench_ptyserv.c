/*
 * bench_ptyserv -- measures throughput of the REAL ghost-ptyserv +
 * pty-ttyN IPC path (pty master -> pty_child.c's forwarding loop ->
 * Unix domain socket -> ghostcon_transport_read(), the exact chain
 * core/main.c's main loop reads pty output through), isolated from VT
 * parsing and rendering.
 *
 * Spawns real ghost-ptyserv/pty-ttyN binaries (same GHOSTCON_PTYSERV_BIN/
 * GHOSTCON_PTY_CHILD_BIN env-var convention tests/test_ptyserv.c and
 * tests/test_undead_head.c already use), connects as a renderer would,
 * and has the real shell inside produce a large burst of output via
 * `dd if=/dev/zero`, timing how fast all of it arrives on the client
 * side. This is the one leg of the pipeline bench_render/bench_parse
 * don't touch at all -- see PLAN.md's benchmarking notes for why all
 * three exist as separate, isolated tools rather than one combined
 * live trace.
 *
 * Usage: bench_ptyserv [megabytes]  (default 64)
 *   Requires GHOSTCON_PTYSERV_BIN and GHOSTCON_PTY_CHILD_BIN env vars
 *   pointing at the built binaries (same as the meson test harness
 *   sets for test_ptyserv/test_undead_head).
 */

#define _DEFAULT_SOURCE

#include "ghostcon/ptyserv/protocol.h"

#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define TEST_VT 98

static double
now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static int
connect_unix(const char *path)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int
main(int argc, char **argv)
{
    int megabytes = argc > 1 ? atoi(argv[1]) : 64;
    if (megabytes <= 0) {
        fprintf(stderr, "usage: %s [megabytes]\n", argv[0]);
        return 1;
    }

    const char *ptyserv_bin = getenv("GHOSTCON_PTYSERV_BIN");
    if (!ptyserv_bin) {
        fprintf(stderr, "FAIL: set GHOSTCON_PTYSERV_BIN to the built ghost-ptyserv binary\n"
                        "  (same env var meson's own test harness sets -- see build-release/meson-logs)\n");
        return 1;
    }
    setenv("GHOSTCON_PTY_SKIP_LOGIN", "1", 1); /* straight to a shell, no agetty banner noise */

    char registry_sock[] = "/tmp/ghostcon-bench-registry-XXXXXX";
    int tmp_fd = mkstemp(registry_sock);
    if (tmp_fd < 0) { fprintf(stderr, "FAIL: mkstemp\n"); return 1; }
    close(tmp_fd);
    unlink(registry_sock);

    signal(SIGPIPE, SIG_IGN);

    pid_t ptyserv_pid = fork();
    if (ptyserv_pid < 0) { fprintf(stderr, "FAIL: fork ghost-ptyserv\n"); return 1; }
    if (ptyserv_pid == 0) {
        char vt_str[16];
        snprintf(vt_str, sizeof(vt_str), "%d", TEST_VT);
        execl(ptyserv_bin, ptyserv_bin, registry_sock, vt_str, (char *)NULL);
        perror("execl ghost-ptyserv");
        _exit(127);
    }

    int registry_fd = -1;
    for (int i = 0; i < 50 && registry_fd < 0; i++) {
        registry_fd = connect_unix(registry_sock);
        if (registry_fd < 0)
            usleep(20000);
    }
    if (registry_fd < 0) {
        fprintf(stderr, "FAIL: could not connect to registry socket\n");
        kill(ptyserv_pid, SIGKILL);
        return 1;
    }

    char req[GHOSTCON_PTYSERV_LINE_MAX];
    size_t req_len = ghostcon_ptyserv_format_get(req, sizeof(req), TEST_VT);
    write(registry_fd, req, req_len);

    char resp[GHOSTCON_PTYSERV_LINE_MAX];
    ssize_t r = read(registry_fd, resp, sizeof(resp) - 1);
    close(registry_fd);
    if (r <= 0) {
        fprintf(stderr, "FAIL: no response from registry\n");
        kill(ptyserv_pid, SIGKILL);
        return 1;
    }
    resp[r] = '\0';

    int pty_child_pid;
    char pty_socket[GHOSTCON_PTYSERV_LINE_MAX];
    if (!ghostcon_ptyserv_parse_ok(resp, &pty_child_pid, pty_socket)) {
        fprintf(stderr, "FAIL: bad registry response: %s\n", resp);
        kill(ptyserv_pid, SIGKILL);
        return 1;
    }

    int renderer_fd = -1;
    for (int i = 0; i < 50 && renderer_fd < 0; i++) {
        renderer_fd = connect_unix(pty_socket);
        if (renderer_fd < 0)
            usleep(20000);
    }
    if (renderer_fd < 0) {
        fprintf(stderr, "FAIL: could not connect to pty child socket\n");
        kill(ptyserv_pid, SIGKILL);
        kill(pty_child_pid, SIGKILL);
        return 1;
    }

    /* Drain whatever's already buffered (shell prompt, motd if any)
       before starting the timed run -- a short quiet-period drain,
       not waiting for a specific string, since a bare shell's prompt
       content isn't predictable across systems/shells. */
    {
        struct pollfd pfd = { .fd = renderer_fd, .events = POLLIN };
        char junk[4096];
        while (poll(&pfd, 1, 200) > 0)
            if (read(renderer_fd, junk, sizeof(junk)) <= 0) break;
    }

    printf("bench_ptyserv: requesting a %d MB burst through the real "
           "pty master -> pty_child.c -> socket -> read() chain\n", megabytes);

    const char *marker = "BENCH_PTYSERV_DONE";
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "dd if=/dev/zero bs=65536 count=%d 2>/dev/null; echo %s\n",
             megabytes * 16 /* 65536 * 16 = 1MB per count-unit... */, marker);
    /* bs=65536 * count -> total bytes = 65536*count; want count such
       that total == megabytes*1024*1024, i.e. count = megabytes*16. */

    write(renderer_fd, cmd, strlen(cmd));

    /* The pty's line discipline echoes typed input back byte-for-byte
       before the shell even runs it -- since `cmd` itself contains the
       literal marker text ("echo BENCH_PTYSERV_DONE" is part of what
       we just typed), searching for the marker without accounting for
       this would match the ECHO instantly, not dd's actual output
       (found live: a 64MB request "completing" in under a
       millisecond). Drain exactly the echoed byte count first --
       local echo mirrors the write 1:1 -- before starting the timed
       measurement on dd's real output only. */
    {
        size_t echoed = 0;
        size_t want = strlen(cmd);
        char junk[256];
        while (echoed < want) {
            struct pollfd pfd = { .fd = renderer_fd, .events = POLLIN };
            if (poll(&pfd, 1, 2000) <= 0) {
                fprintf(stderr, "FAIL: timed out draining echoed input\n");
                close(renderer_fd);
                kill(pty_child_pid, SIGKILL);
                kill(ptyserv_pid, SIGKILL);
                return 1;
            }
            size_t chunk = want - echoed < sizeof(junk) ? want - echoed : sizeof(junk);
            ssize_t n = read(renderer_fd, junk, chunk);
            if (n <= 0) break;
            echoed += (size_t)n;
        }
    }

    double t0 = now_ms();
    size_t total_read = 0;
    char buf[65536];
    char tail[128] = {0};
    size_t tail_len = 0;
    size_t marker_len = strlen(marker);
    bool done = false;
    while (!done) {
        struct pollfd pfd = { .fd = renderer_fd, .events = POLLIN };
        int rv = poll(&pfd, 1, 5000);
        if (rv <= 0) {
            fprintf(stderr, "FAIL: timed out waiting for completion marker\n");
            break;
        }
        ssize_t n = read(renderer_fd, buf, sizeof(buf));
        if (n <= 0) break;
        total_read += (size_t)n;

        /* Cheap rolling-window marker check, without keeping the
           whole (potentially large) transfer in memory: concatenate
           the last `tail_len` bytes seen with this new chunk, check
           for the marker in that window, then keep only the final
           sizeof(tail) bytes as the new tail. */
        char window[sizeof(tail) + sizeof(buf)];
        memcpy(window, tail, tail_len);
        memcpy(window + tail_len, buf, (size_t)n);
        size_t window_len = tail_len + (size_t)n;

        if (window_len >= marker_len && memmem(window, window_len, marker, marker_len))
            done = true;

        tail_len = window_len < sizeof(tail) ? window_len : sizeof(tail);
        memcpy(tail, window + (window_len - tail_len), tail_len);
    }
    double elapsed_ms = now_ms() - t0;

    if (done) {
        double mb = (double)total_read / (1024.0 * 1024.0);
        printf("\n--- results ---\n");
        printf("bytes received: %zu (%.2f MB)\n", total_read, mb);
        printf("elapsed:        %.2f ms\n", elapsed_ms);
        printf("throughput:     %.2f MB/s\n", mb / (elapsed_ms / 1000.0));
    }

    close(renderer_fd);
    kill(pty_child_pid, SIGKILL);
    kill(ptyserv_pid, SIGKILL);
    waitpid(ptyserv_pid, NULL, 0);
    waitpid(pty_child_pid, NULL, 0);
    unlink(registry_sock);

    return done ? 0 : 1;
}
