/*
 * test_transport — Phase 1 acceptance test for core/transport.c.
 *
 * Starts a real ghost-ptyserv + pty-ttyN pair (same setup as
 * test_ptyserv.c) and connects to it via ghostcon_transport_connect
 * instead of raw sockets, verifying the registry query + pty child
 * connect + bidirectional byte flow all work through the transport API
 * a real ghostcon-core renderer would use.
 */

#define _DEFAULT_SOURCE

#include "ghostcon/core/transport.h"

#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define TEST_VT 96

static bool
wait_for_substring(ghostcon_transport_t *t, const char *needle, int timeout_ms)
{
    char acc[8192];
    size_t acc_len = 0;

    for (;;) {
        struct pollfd pfd = { .fd = ghostcon_transport_fd(t), .events = POLLIN };
        int rv = poll(&pfd, 1, timeout_ms);
        if (rv <= 0)
            return false;
        ssize_t r = ghostcon_transport_read(t, (uint8_t *)acc + acc_len,
                                             sizeof(acc) - acc_len - 1);
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

    char registry_sock[] = "/tmp/ghostcon-test-transport-registry-XXXXXX";
    int tmp_fd = mkstemp(registry_sock);
    if (tmp_fd < 0) {
        fprintf(stderr, "FAIL: mkstemp\n");
        return 1;
    }
    close(tmp_fd);
    unlink(registry_sock);

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

    ghostcon_transport_t t;
    bool connected = false;
    for (int i = 0; i < 50 && !connected; i++) {
        connected = ghostcon_transport_connect(&t, registry_sock, TEST_VT);
        if (!connected)
            usleep(20000);
    }
    if (!connected) {
        fprintf(stderr, "FAIL: could not connect transport\n");
        kill(ptyserv_pid, SIGKILL);
        unlink(registry_sock);
        return 1;
    }
    printf("PASS: transport connected (registry query + pty child connect)\n");

    const char *cmd = "echo TRANSPORT_OK\n";
    ghostcon_transport_write(&t, (const uint8_t *)cmd, strlen(cmd));
    if (!wait_for_substring(&t, "TRANSPORT_OK", 3000)) {
        fprintf(stderr, "FAIL: did not see command output over transport\n");
        ghostcon_transport_close(&t);
        kill(t.pty_child_pid, SIGKILL);
        kill(ptyserv_pid, SIGKILL);
        unlink(registry_sock);
        return 1;
    }
    printf("PASS: bidirectional byte flow over transport\n");

    ghostcon_transport_close(&t);
    /* pty-ttyN is ghost-ptyserv's child, not ours -- SIGTERM on
       ptyserv_pid doesn't touch it, and it holds a dup'd stdout fd
       from fork() that would otherwise keep the test harness waiting
       on this process's output pipe forever after we exit. */
    kill(t.pty_child_pid, SIGTERM);
    kill(ptyserv_pid, SIGTERM);
    waitpid(ptyserv_pid, NULL, 0);
    unlink(registry_sock);

    printf("ALL TESTS PASSED\n");
    return 0;
}
