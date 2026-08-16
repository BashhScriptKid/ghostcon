/*
 * test_ptyserv — Phase 1 acceptance test for ghost-ptyserv + pty-ttyN.
 *
 * Starts ghost-ptyserv with a test config requesting one VT, connects a
 * fake renderer client to the pty child it spawns, and verifies
 * bidirectional byte flow and ring-buffer replay on reconnect. This is
 * the exact acceptance test PLAN.md specifies for Phase 1 item 1.
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
#include <unistd.h>

#define TEST_VT 97

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

/* Reads from fd until `needle` appears in the accumulated buffer or the
   deadline (in milliseconds from now) expires. Returns true on match. */
static bool
wait_for_substring(int fd, const char *needle, int timeout_ms)
{
    char acc[8192];
    size_t acc_len = 0;

    for (;;) {
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int rv = poll(&pfd, 1, timeout_ms);
        if (rv <= 0)
            return false;
        ssize_t r = read(fd, acc + acc_len, sizeof(acc) - acc_len - 1);
        if (r <= 0)
            return false;
        acc_len += (size_t)r;
        acc[acc_len] = '\0';
        if (strstr(acc, needle))
            return true;
        if (acc_len >= sizeof(acc) - 1)
            return false;
    }
}

int
main(void)
{
    const char *ptyserv_bin = getenv("GHOSTCON_PTYSERV_BIN");
    if (!ptyserv_bin) {
        fprintf(stderr, "FAIL: GHOSTCON_PTYSERV_BIN not set\n");
        return 1;
    }

    char registry_sock[] = "/tmp/ghostcon-test-registry-XXXXXX";
    int tmp_fd = mkstemp(registry_sock);
    if (tmp_fd < 0) {
        fprintf(stderr, "FAIL: mkstemp\n");
        return 1;
    }
    close(tmp_fd);
    unlink(registry_sock); /* ghost-ptyserv creates the socket itself */

    signal(SIGPIPE, SIG_IGN);

    pid_t ptyserv_pid = fork();
    if (ptyserv_pid < 0) {
        fprintf(stderr, "FAIL: fork ghost-ptyserv\n");
        return 1;
    }
    if (ptyserv_pid == 0) {
        char vt_str[16];
        snprintf(vt_str, sizeof(vt_str), "%d", TEST_VT);
        execl(ptyserv_bin, ptyserv_bin, registry_sock, vt_str, (char *)NULL);
        perror("execl ghost-ptyserv");
        _exit(127);
    }

    /* Give ghost-ptyserv + its pty child a moment to bind their sockets. */
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
    printf("PASS: registry GET %d -> pid=%d socket=%s\n",
           TEST_VT, pty_child_pid, pty_socket);

    /* Connect a fake renderer, wait for the pty child socket to appear
       (it binds slightly after the fork). */
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

    /* Bidirectional byte flow: send a shell command, expect its output. */
    const char *cmd = "echo HELLO_GHOSTCON\n";
    write(renderer_fd, cmd, strlen(cmd));
    if (!wait_for_substring(renderer_fd, "HELLO_GHOSTCON", 3000)) {
        fprintf(stderr, "FAIL: did not see command output\n");
        kill(ptyserv_pid, SIGKILL);
        kill(pty_child_pid, SIGKILL);
        return 1;
    }
    printf("PASS: bidirectional byte flow\n");

    /* Disconnect (simulating a killed/hung renderer), then reconnect and
       verify the ring buffer replays prior output. */
    close(renderer_fd);
    usleep(50000);

    int renderer_fd2 = connect_unix(pty_socket);
    if (renderer_fd2 < 0) {
        fprintf(stderr, "FAIL: could not reconnect to pty child socket\n");
        kill(ptyserv_pid, SIGKILL);
        kill(pty_child_pid, SIGKILL);
        return 1;
    }
    if (!wait_for_substring(renderer_fd2, "HELLO_GHOSTCON", 3000)) {
        fprintf(stderr, "FAIL: ring buffer did not replay prior output\n");
        kill(ptyserv_pid, SIGKILL);
        kill(pty_child_pid, SIGKILL);
        return 1;
    }
    printf("PASS: ring buffer replay on reconnect\n");

    /* Reliability fix verification: kill ghost-ptyserv itself directly
       (not the pty child), start a fresh instance pointed at the SAME
       registry socket, and confirm it recognizes and reuses the
       already-running pty-ttyN instead of spawning a duplicate -- the
       live session (renderer_fd2, still connected this whole time)
       must keep working straight through, uninterrupted. See
       ptyserv/main.c's spawn_pty_child() pidfile-liveness check and
       pty_child.c's own setpgid(0,0) (which is what lets pty-ttyN
       survive ghost-ptyserv's process-group kill in the first place). */
    kill(ptyserv_pid, SIGKILL);
    waitpid(ptyserv_pid, NULL, 0);

    pid_t ptyserv_pid2 = fork();
    if (ptyserv_pid2 < 0) {
        fprintf(stderr, "FAIL: fork second ghost-ptyserv\n");
        kill(pty_child_pid, SIGKILL);
        return 1;
    }
    if (ptyserv_pid2 == 0) {
        char vt_str[16];
        snprintf(vt_str, sizeof(vt_str), "%d", TEST_VT);
        execl(ptyserv_bin, ptyserv_bin, registry_sock, vt_str, (char *)NULL);
        perror("execl ghost-ptyserv (second instance)");
        _exit(127);
    }

    int registry_fd2 = -1;
    for (int i = 0; i < 50 && registry_fd2 < 0; i++) {
        registry_fd2 = connect_unix(registry_sock);
        if (registry_fd2 < 0)
            usleep(20000);
    }
    if (registry_fd2 < 0) {
        fprintf(stderr, "FAIL: could not connect to registry socket after ghost-ptyserv restart\n");
        kill(ptyserv_pid2, SIGKILL);
        kill(pty_child_pid, SIGKILL);
        return 1;
    }

    write(registry_fd2, req, req_len); /* same GET request as before */
    char resp2[GHOSTCON_PTYSERV_LINE_MAX];
    ssize_t r2 = read(registry_fd2, resp2, sizeof(resp2) - 1);
    close(registry_fd2);
    if (r2 <= 0) {
        fprintf(stderr, "FAIL: no response from restarted registry\n");
        kill(ptyserv_pid2, SIGKILL);
        kill(pty_child_pid, SIGKILL);
        return 1;
    }
    resp2[r2] = '\0';

    int pty_child_pid2;
    char pty_socket2[GHOSTCON_PTYSERV_LINE_MAX];
    if (!ghostcon_ptyserv_parse_ok(resp2, &pty_child_pid2, pty_socket2)) {
        fprintf(stderr, "FAIL: bad registry response after restart: %s\n", resp2);
        kill(ptyserv_pid2, SIGKILL);
        kill(pty_child_pid, SIGKILL);
        return 1;
    }
    if (pty_child_pid2 != pty_child_pid || strcmp(pty_socket2, pty_socket) != 0) {
        fprintf(stderr,
                "FAIL: restarted ghost-ptyserv did not reuse the existing pty-ttyN "
                "(pid %d -> %d, socket %s -> %s) -- spawned a duplicate instead\n",
                pty_child_pid, pty_child_pid2, pty_socket, pty_socket2);
        kill(ptyserv_pid2, SIGKILL);
        kill(pty_child_pid, SIGKILL);
        return 1;
    }
    printf("PASS: restarted ghost-ptyserv reused the existing pty-ttyN (pid %d, socket %s)\n",
           pty_child_pid2, pty_socket2);

    /* The live session survived the whole thing -- renderer_fd2 has
       been connected since before ghost-ptyserv was ever killed. */
    const char *cmd2 = "echo STILL_ALIVE_GHOSTCON\n";
    write(renderer_fd2, cmd2, strlen(cmd2));
    if (!wait_for_substring(renderer_fd2, "STILL_ALIVE_GHOSTCON", 3000)) {
        fprintf(stderr, "FAIL: live session did not survive ghost-ptyserv restart\n");
        kill(ptyserv_pid2, SIGKILL);
        kill(pty_child_pid, SIGKILL);
        return 1;
    }
    printf("PASS: live session survived ghost-ptyserv's restart uninterrupted\n");

    close(renderer_fd2);
    kill(ptyserv_pid2, SIGTERM);
    waitpid(ptyserv_pid2, NULL, 0);
    kill(pty_child_pid, SIGTERM);
    unlink(registry_sock); /* ghost-ptyserv's own cleanup doesn't run under SIGTERM */

    printf("ALL TESTS PASSED\n");
    return 0;
}
