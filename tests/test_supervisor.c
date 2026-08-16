/*
 * test_supervisor — drives supervisor[ttyN]'s state machine through each
 * scenario in PLAN.md's supervision layer, using tests/fake_renderer.c
 * as a stand-in for ghostcon-core (no real DRM/KMS/VT hardware needed).
 * Observes supervisor's behavior via its stderr log lines, since it has
 * no other externally observable state (no status/query mechanism, by
 * design -- PLAN.md's canary is deliberately dumb).
 */

#define _DEFAULT_SOURCE

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define TEST_VT 90

static pid_t
spawn_supervisor(int *out_stderr_fd)
{
    int pipefd[2];
    if (pipe(pipefd) != 0)
        return -1;

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }
    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);

        const char *bin = getenv("SUPERVISOR_BIN");
        char vt_str[16];
        snprintf(vt_str, sizeof(vt_str), "%d", TEST_VT);
        execlp(bin, bin, vt_str, "/tmp/ghostcon-test-supervisor-unused.sock", (char *)NULL);
        _exit(127);
    }

    close(pipefd[1]);
    fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
    *out_stderr_fd = pipefd[0];
    return pid;
}

/* Reads supervisor's stderr until `needle` appears or the deadline
   expires. Returns true on match. */
static bool
wait_for_log(int fd, const char *needle, int timeout_ms)
{
    char acc[8192];
    size_t acc_len = 0;

    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += timeout_ms / 1000;
    deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;

    for (;;) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long remaining_ms = (deadline.tv_sec - now.tv_sec) * 1000 +
                             (deadline.tv_nsec - now.tv_nsec) / 1000000L;
        if (remaining_ms <= 0)
            return false;

        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int rv = poll(&pfd, 1, (int)remaining_ms);
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

static void
kill_supervisor_tree(pid_t pid)
{
    if (pid <= 0)
        return;
    pid_t pgid = getpgid(pid);
    if (pgid > 0)
        kill(-pgid, SIGKILL);
    waitpid(pid, NULL, 0);
}

static int failures = 0;

static void
scenario_healthy(void)
{
    setenv("GHOSTCON_CANARY_DEADLINE_MS", "300", 1);
    unsetenv("FAKE_RENDERER_HANG_AFTER_MS");
    unsetenv("FAKE_RENDERER_EXIT_AFTER_MS");
    unsetenv("FAKE_RENDERER_NEVER_HEARTBEAT");

    int stderr_fd;
    pid_t pid = spawn_supervisor(&stderr_fd);
    if (pid < 0) {
        fprintf(stderr, "FAIL: healthy: could not spawn supervisor\n");
        failures++;
        return;
    }

    if (!wait_for_log(stderr_fd, "claimed VT, ACTIVE", 2000)) {
        fprintf(stderr, "FAIL: healthy: supervisor never reached ACTIVE\n");
        failures++;
    } else {
        printf("PASS: healthy renderer reaches ACTIVE\n");
    }

    kill_supervisor_tree(pid);
    close(stderr_fd);
}

static void
scenario_hang_and_respawn(void)
{
    setenv("GHOSTCON_CANARY_DEADLINE_MS", "300", 1);
    setenv("FAKE_RENDERER_HANG_AFTER_MS", "150", 1);
    unsetenv("FAKE_RENDERER_EXIT_AFTER_MS");
    unsetenv("FAKE_RENDERER_NEVER_HEARTBEAT");
    /* This scenario hits supervisor's wall_broadcast(vtnum, "hang") path
       for real -- without this, every meson test run genuinely paged
       whoever's logged in with a live "renderer hang, recovering"
       message. Found live: the user saw one appear in their own
       terminal mid-session. scenario_startup_timeout_fallback already
       disables this for its own wall_broadcast() call; this scenario
       was just missing the same line. */
    setenv("GHOSTCON_DISABLE_WALL", "1", 1);

    int stderr_fd;
    pid_t pid = spawn_supervisor(&stderr_fd);
    if (pid < 0) {
        fprintf(stderr, "FAIL: hang: could not spawn supervisor\n");
        failures++;
        return;
    }

    bool ok = wait_for_log(stderr_fd, "claimed VT, ACTIVE", 2000) &&
              wait_for_log(stderr_fd, "renderer hung", 2000) &&
              wait_for_log(stderr_fd, "claimed VT, ACTIVE", 2000);
    if (!ok) {
        fprintf(stderr, "FAIL: hang: did not see ACTIVE -> hung -> ACTIVE cycle\n");
        failures++;
    } else {
        printf("PASS: hung renderer detected and respawned\n");
    }

    kill_supervisor_tree(pid);
    close(stderr_fd);
}

static void
scenario_clean_exit_respawn(void)
{
    setenv("GHOSTCON_CANARY_DEADLINE_MS", "300", 1);
    unsetenv("FAKE_RENDERER_HANG_AFTER_MS");
    setenv("FAKE_RENDERER_EXIT_AFTER_MS", "150", 1);
    unsetenv("FAKE_RENDERER_NEVER_HEARTBEAT");

    int stderr_fd;
    pid_t pid = spawn_supervisor(&stderr_fd);
    if (pid < 0) {
        fprintf(stderr, "FAIL: clean-exit: could not spawn supervisor\n");
        failures++;
        return;
    }

    bool ok = wait_for_log(stderr_fd, "claimed VT, ACTIVE", 2000) &&
              wait_for_log(stderr_fd, "renderer exited, respawning", 2000) &&
              wait_for_log(stderr_fd, "claimed VT, ACTIVE", 2000);
    if (!ok) {
        fprintf(stderr, "FAIL: clean-exit: did not see ACTIVE -> exit -> ACTIVE cycle\n");
        failures++;
    } else {
        printf("PASS: clean renderer exit detected and respawned\n");
    }

    kill_supervisor_tree(pid);
    close(stderr_fd);
}

static void
scenario_startup_timeout_fallback(void)
{
    char run_dir[] = "/tmp/ghostcon-test-supervisor-rundir-XXXXXX";
    if (!mkdtemp(run_dir)) {
        fprintf(stderr, "FAIL: fallback: mkdtemp\n");
        failures++;
        return;
    }

    setenv("GHOSTCON_CANARY_DEADLINE_MS", "300", 1);
    unsetenv("FAKE_RENDERER_HANG_AFTER_MS");
    unsetenv("FAKE_RENDERER_EXIT_AFTER_MS");
    setenv("FAKE_RENDERER_NEVER_HEARTBEAT", "1", 1);
    setenv("GHOSTCON_DISABLE_KMSCON_FALLBACK", "1", 1);
    setenv("GHOSTCON_DISABLE_WALL", "1", 1);
    setenv("GHOSTCON_RUN_DIR", run_dir, 1);

    int stderr_fd;
    pid_t pid = spawn_supervisor(&stderr_fd);
    if (pid < 0) {
        fprintf(stderr, "FAIL: fallback: could not spawn supervisor\n");
        failures++;
        goto cleanup;
    }

    if (!wait_for_log(stderr_fd, "falling back", 2000)) {
        fprintf(stderr, "FAIL: fallback: supervisor never reported falling back\n");
        failures++;
        kill_supervisor_tree(pid);
        close(stderr_fd);
        goto cleanup;
    }
    printf("PASS: startup timeout triggers fallback\n");

    char recovery_path[512];
    snprintf(recovery_path, sizeof(recovery_path), "%s/recovery-tty%d.json", run_dir, TEST_VT);
    /* write_recovery_file() runs AFTER kill_and_reap()'s grace-period
       wait for the timed-out renderer (up to ~500ms), which itself
       runs after the "falling back" log line we just waited for --
       so the file may not exist yet the instant that line appears. */
    FILE *f = NULL;
    for (int i = 0; i < 20 && !f; i++) {
        f = fopen(recovery_path, "r");
        if (!f)
            usleep(100000);
    }
    if (!f) {
        fprintf(stderr, "FAIL: fallback: recovery file %s not written\n", recovery_path);
        failures++;
    } else {
        char buf[512];
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        buf[n] = '\0';
        fclose(f);
        if (!strstr(buf, "startup_timeout")) {
            fprintf(stderr, "FAIL: fallback: recovery file missing startup_timeout reason: %s\n", buf);
            failures++;
        } else {
            printf("PASS: recovery file written with correct reason\n");
        }
    }

    kill_supervisor_tree(pid);
    close(stderr_fd);

cleanup:
    unsetenv("GHOSTCON_RUN_DIR");
    unsetenv("GHOSTCON_DISABLE_KMSCON_FALLBACK");
    unsetenv("FAKE_RENDERER_NEVER_HEARTBEAT");
    char rm_cmd[600];
    snprintf(rm_cmd, sizeof(rm_cmd), "rm -rf '%s'", run_dir);
    system(rm_cmd);
}

int
main(void)
{
    const char *supervisor_bin = getenv("SUPERVISOR_BIN");
    const char *renderer_bin = getenv("GHOSTCON_CORE_BIN");
    if (!supervisor_bin || !renderer_bin) {
        fprintf(stderr, "FAIL: SUPERVISOR_BIN / GHOSTCON_CORE_BIN not set\n");
        return 1;
    }

    signal(SIGPIPE, SIG_IGN);

    scenario_healthy();
    scenario_hang_and_respawn();
    scenario_clean_exit_respawn();
    scenario_startup_timeout_fallback();

    if (failures > 0) {
        fprintf(stderr, "%d scenario(s) failed\n", failures);
        return 1;
    }
    printf("ALL TESTS PASSED\n");
    return 0;
}
