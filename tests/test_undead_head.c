/*
 * test_undead_head — verifies undead-head's own specific job: forking
 * ghost-ptyserv + one supervisor per VT, and restarting whichever one
 * dies (whole-tree restart on ghost-ptyserv death, single-supervisor
 * restart otherwise). Uses the real ghost-ptyserv/supervisor binaries
 * with tests/fake_renderer.c standing in for ghostcon-core, so no real
 * DRM/KMS/VT hardware is needed. supervisor's own state machine is
 * already covered in detail by test_supervisor.c; this test only
 * exercises undead-head's restart-on-crash behavior.
 */

#define _DEFAULT_SOURCE

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define TEST_VT 92

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

static bool
registry_reachable(const char *registry_sock, int timeout_ms)
{
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (;;) {
        int fd = connect_unix(registry_sock);
        if (fd >= 0) {
            close(fd);
            return true;
        }
        clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed_ms = (now.tv_sec - start.tv_sec) * 1000 +
                           (now.tv_nsec - start.tv_nsec) / 1000000L;
        if (elapsed_ms >= timeout_ms)
            return false;
        usleep(50000);
    }
}

/* Single pgrep -f call (no internal retry loop -- see pgrep_wait_ne,
   which is where retry timing belongs; nesting timed retry loops inside
   each other just makes the outer timeout unpredictable). Returns the
   first (lowest-pid, i.e. oldest) matching pid, or -1.
   exec's pgrep directly (no `sh -c "...pattern..."` wrapper) -- found
   the hard way, via this exact function returning a real but WRONG pid
   for a not-yet-started process instead of correctly waiting/retrying:
   pgrep only excludes its own pid from results, not a parent shell's,
   and a `popen("sh -c \"pgrep -f -- 'PATTERN' ...\"", ...)` wrapper's
   own cmdline necessarily contains PATTERN as a literal substring too
   (it's right there in the shell command string) -- so that wrapper
   shell was itself a guaranteed spurious match for ANY pattern, one
   that only lost the "first/lowest pid" race when a real, already-
   running, older target happened to exist by query time. A target
   checked before it's actually started (this file's new
   supervisor_pid_before check, added for the ghost-ptyserv-restart-
   scoping fix, is younger than every existing check in this file and
   was the first to actually expose it) got the wrapper shell's pid
   back immediately, on the very first attempt, instead of retrying
   until the real process appeared. */
static pid_t
pgrep_once(const char *pattern)
{
    int pipefd[2];
    if (pipe(pipefd) != 0)
        return -1;

    pid_t child = fork();
    if (child < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }
    if (child == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0)
            dup2(devnull, STDERR_FILENO);
        execlp("pgrep", "pgrep", "-f", "--", pattern, (char *)NULL);
        _exit(127);
    }
    close(pipefd[1]);

    char buf[4096];
    ssize_t total = 0;
    ssize_t r;
    while (total < (ssize_t)sizeof(buf) - 1 &&
           (r = read(pipefd[0], buf + total, sizeof(buf) - 1 - (size_t)total)) > 0)
        total += r;
    buf[total > 0 ? total : 0] = '\0';
    close(pipefd[0]);
    waitpid(child, NULL, 0);

    /* pgrep's output is newline-separated pids in ascending order --
       atoi() naturally stops at the first newline, giving the first
       (lowest/oldest) match. */
    pid_t pid = (pid_t)atoi(buf);
    return pid > 0 ? pid : -1;
}

/* Polls pgrep_once until it returns a pid different from `exclude` (or
   times out). Used to detect "the old process died and a new one with
   a different pid took its place". */
static pid_t
pgrep_wait_ne(const char *pattern, pid_t exclude, int timeout_ms)
{
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (;;) {
        pid_t pid = pgrep_once(pattern);
        if (pid > 0 && pid != exclude)
            return pid;

        clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed_ms = (now.tv_sec - start.tv_sec) * 1000 +
                           (now.tv_nsec - start.tv_nsec) / 1000000L;
        if (elapsed_ms >= timeout_ms)
            return -1;
        usleep(100000);
    }
}

/* Finds the PID of a running process whose full argv matches `pattern`
   (via pgrep -f), waiting up to timeout_ms for it to appear. */
static pid_t
pgrep_wait(const char *pattern, int timeout_ms)
{
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (;;) {
        pid_t pid = pgrep_once(pattern);
        if (pid > 0)
            return pid;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed_ms = (now.tv_sec - start.tv_sec) * 1000 +
                           (now.tv_nsec - start.tv_nsec) / 1000000L;
        if (elapsed_ms >= timeout_ms)
            return -1;
        usleep(50000);
    }
}

int
main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);

    const char *undead_head_bin = getenv("UNDEAD_HEAD_BIN");
    const char *ptyserv_bin = getenv("GHOSTCON_PTYSERV_BIN");
    const char *supervisor_bin = getenv("GHOSTCON_SUPERVISOR_BIN");
    if (!undead_head_bin || !ptyserv_bin || !supervisor_bin) {
        fprintf(stderr, "FAIL: UNDEAD_HEAD_BIN / GHOSTCON_PTYSERV_BIN / GHOSTCON_SUPERVISOR_BIN not set\n");
        return 1;
    }

    char registry_sock[] = "/tmp/ghostcon-test-undead-registry-XXXXXX";
    int tmp_fd = mkstemp(registry_sock);
    if (tmp_fd < 0) {
        fprintf(stderr, "FAIL: mkstemp\n");
        return 1;
    }
    close(tmp_fd);
    unlink(registry_sock);

    signal(SIGPIPE, SIG_IGN);

    pid_t undead_pid = fork();
    if (undead_pid < 0) {
        fprintf(stderr, "FAIL: fork undead-head\n");
        return 1;
    }
    if (undead_pid == 0) {
        char vt_str[16];
        snprintf(vt_str, sizeof(vt_str), "%d", TEST_VT);
        execl(undead_head_bin, undead_head_bin, registry_sock, vt_str, (char *)NULL);
        perror("execl undead-head");
        _exit(127);
    }

    int result = 1;

    if (!registry_reachable(registry_sock, 3000)) {
        fprintf(stderr, "FAIL: registry never became reachable\n");
        goto teardown;
    }
    printf("PASS: undead-head brought up ghost-ptyserv (registry reachable)\n");

    pid_t ptyserv_pid1 = pgrep_wait(ptyserv_bin, 2000);
    if (ptyserv_pid1 < 0) {
        fprintf(stderr, "FAIL: could not find ghost-ptyserv pid\n");
        goto teardown;
    }

    pid_t supervisor_pid_before = pgrep_wait(supervisor_bin, 2000);
    if (supervisor_pid_before < 0) {
        fprintf(stderr, "FAIL: could not find supervisor pid\n");
        goto teardown;
    }

    /* Kill ghost-ptyserv -- undead-head restarts ONLY ghost-ptyserv, not
       the whole tree (an already-connected renderer has no ongoing
       dependency on it; see undead_head/main.c's ghost_ptyserv_pid
       branch and its doc comment). The supervisor must NOT be touched
       -- if it were, this pid would change too. */
    kill(ptyserv_pid1, SIGKILL);

    pid_t ptyserv_pid2 = pgrep_wait_ne(ptyserv_bin, ptyserv_pid1, 10000);
    if (ptyserv_pid2 < 0) {
        fprintf(stderr, "FAIL: ghost-ptyserv was not restarted after being killed\n");
        goto teardown;
    }
    printf("PASS: undead-head restarted ghost-ptyserv after it was killed (pid %d -> %d)\n",
           (int)ptyserv_pid1, (int)ptyserv_pid2);

    if (!registry_reachable(registry_sock, 3000)) {
        fprintf(stderr, "FAIL: registry not reachable again after ghost-ptyserv restart\n");
        goto teardown;
    }
    printf("PASS: registry reachable again after restart\n");

    pid_t supervisor_pid_after = pgrep_wait(supervisor_bin, 2000);
    if (supervisor_pid_after != supervisor_pid_before) {
        fprintf(stderr,
                "FAIL: supervisor pid changed (%d -> %d) -- ghost-ptyserv's death "
                "should not have touched it\n",
                (int)supervisor_pid_before, (int)supervisor_pid_after);
        goto teardown;
    }
    printf("PASS: supervisor pid unchanged across ghost-ptyserv restart (%d)\n",
           (int)supervisor_pid_before);

    result = 0;

teardown:
    if (undead_pid > 0) {
        kill(undead_pid, SIGTERM);
        waitpid(undead_pid, NULL, 0);
    }
    /* undead-head's own SIGTERM handler kills its whole process tree
       (see undead_head/main.c) -- best-effort extra cleanup in case
       that path itself is what's broken. */
    system("pkill -9 -f 'test_fake_renderer' 2>/dev/null; "
           "pkill -9 -f 'pty-ttyN' 2>/dev/null");
    unlink(registry_sock);

    printf(result == 0 ? "ALL TESTS PASSED\n" : "FAILED\n");
    return result;
}
