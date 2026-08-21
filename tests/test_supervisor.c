/*
 * test_supervisor — drives supervisor[ttyN]'s state machine through each
 * scenario in PLAN.md's supervision layer, using tests/fake_renderer.c
 * as a stand-in for ghostcon-core (no real DRM/KMS/VT hardware needed).
 * Observes supervisor's behavior via its stderr log lines, since it has
 * no other externally observable state (no status/query mechanism, by
 * design -- PLAN.md's canary is deliberately dumb).
 */

#define _DEFAULT_SOURCE

#include "ghostcon/ptyserv/protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define TEST_VT 90

static pid_t
spawn_supervisor_with_registry(int *out_stderr_fd, const char *registry_socket)
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
        execlp(bin, bin, vt_str, registry_socket, (char *)NULL);
        _exit(127);
    }

    close(pipefd[1]);
    fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
    *out_stderr_fd = pipefd[0];
    return pid;
}

/* Most scenarios don't touch ghost-ptyserv at all (fake_renderer.c's
   default behavior ignores argv entirely -- see its own doc comment),
   so this placeholder path is a deliberate no-op destination, not a
   real socket anything connects to. */
static pid_t
spawn_supervisor(int *out_stderr_fd)
{
    return spawn_supervisor_with_registry(out_stderr_fd, "/tmp/ghostcon-test-supervisor-unused.sock");
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

/* Same idea as wait_for_log, but reads into a CALLER-OWNED, persistent
   accumulator (buf/len) and only searches from *search_from onward --
   needed for scenario_hang_preserves_session below, which chains
   several sequential waits against one fast-moving log stream.
   wait_for_log's own fresh-local-buffer-per-call design silently
   drops any bytes read past a match in the same read() syscall (fine
   for the existing scenarios, which only ever wait for one thing
   before tearing the supervisor down; not sturdy enough here, where a
   burst -- e.g. "renderer hung" immediately followed by the next
   instance's own startup lines -- landing in a single read() would
   otherwise vanish before a later wait_for_log_seq call could see it).
   On success, *search_from advances to just past the match, so the
   caller can inspect buf[old *search_from .. new *search_from] for
   anything captured alongside the needle (e.g. a trailing pid via
   sscanf), and the next call won't re-match the same occurrence. */
static bool
wait_for_log_seq(int fd, char *buf, size_t buf_cap, size_t *len,
                 size_t *search_from, const char *needle, int timeout_ms)
{
    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += timeout_ms / 1000;
    deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;

    for (;;) {
        char *hit = strstr(buf + *search_from, needle);
        if (hit) {
            *search_from = (size_t)(hit - buf) + strlen(needle);
            return true;
        }

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long remaining_ms = (deadline.tv_sec - now.tv_sec) * 1000 +
                             (deadline.tv_nsec - now.tv_nsec) / 1000000L;
        if (remaining_ms <= 0 || *len >= buf_cap - 1)
            return false;

        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int rv = poll(&pfd, 1, (int)remaining_ms);
        if (rv <= 0)
            return false;
        ssize_t r = read(fd, buf + *len, buf_cap - 1 - *len);
        if (r <= 0)
            return false;
        *len += (size_t)r;
        buf[*len] = '\0';
    }
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

/* Spawns a REAL ghost-ptyserv (same fork/execl pattern as
   test_ptyserv.c) rather than fake_renderer.c's usual no-hardware
   stand-in -- needed for scenario_hang_preserves_session below, which
   has to observe a REAL pty child surviving a hang, not just the
   canary protocol. */
static pid_t
spawn_ghost_ptyserv(const char *registry_socket, int vtnum)
{
    const char *bin = getenv("GHOSTCON_PTYSERV_BIN");
    if (!bin)
        return -1;

    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        char vt_str[16];
        snprintf(vt_str, sizeof(vt_str), "%d", vtnum);
        execl(bin, bin, registry_socket, vt_str, (char *)NULL);
        _exit(127);
    }

    for (int i = 0; i < 50; i++) {
        int fd = connect_unix(registry_socket);
        if (fd >= 0) {
            close(fd);
            return pid;
        }
        usleep(20000);
    }
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    return -1;
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

/* Deepens scenario_hang_and_respawn to the full induced-hang checklist
   from PLAN.md's Testing strategy section, points 2-5 specifically
   (point 1, deadline timing, and point 6, wall rate-limiting, are
   covered elsewhere): with a REAL ghost-ptyserv + pty child + shell in
   the loop (fake_renderer's FAKE_RENDERER_REAL_PTY mode, see that
   file's own doc comment), confirms the pty child survives the hung
   renderer's SIGKILL, the recovery file records that pty child's REAL
   pid (not the -1 placeholder every recovery file used to contain --
   see query_pty_child_pid() in supervisor/main.c), and the respawned
   renderer's ring-buffer replay actually contains output from BEFORE
   the hang -- proving session continuity, not just "a shell appeared." */
static void
scenario_hang_preserves_session(void)
{
    int pid1 = -1; /* real pty child pid, set once the first instance
                       reports it -- declared up here (not at its
                       first real use below) so it's always -1, not
                       indeterminate, at every cleanup label this
                       function can jump to via goto, including ones
                       reached before it would otherwise be assigned. */

    char run_dir[] = "/tmp/ghostcon-test-supervisor-rundir2-XXXXXX";
    if (!mkdtemp(run_dir)) {
        fprintf(stderr, "FAIL: session: mkdtemp\n");
        failures++;
        return;
    }

    char registry_sock[] = "/tmp/ghostcon-test-supervisor-registry-XXXXXX";
    int tmp_fd = mkstemp(registry_sock);
    if (tmp_fd < 0) {
        fprintf(stderr, "FAIL: session: mkstemp\n");
        failures++;
        goto cleanup_rundir;
    }
    close(tmp_fd);
    unlink(registry_sock); /* ghost-ptyserv creates the socket itself */

    pid_t ptyserv_pid = spawn_ghost_ptyserv(registry_sock, TEST_VT);
    if (ptyserv_pid < 0) {
        fprintf(stderr, "FAIL: session: could not start ghost-ptyserv\n");
        failures++;
        goto cleanup_rundir;
    }

    /* Unlike the other scenarios' 300ms, this needs real headroom:
       fake_renderer's REAL_PTY block (connect + a check-marker read
       loop that can legitimately run for its own full 1000ms window
       on the FIRST instance, which has nothing to find yet) all
       happens BEFORE the first heartbeat byte, and that whole block
       counts against the startup race timer -- the same deadline
       value as the ongoing hang check (see supervisor/main.c's file
       comment: "the startup race timer and the ongoing canary are the
       SAME mechanism"). 300ms was tight enough to make the FIRST
       instance look like a startup timeout instead of reaching ACTIVE
       (found live). FAKE_RENDERER_HANG_AFTER_MS is unaffected by this
       -- it's measured from AFTER the REAL_PTY block completes, not
       from process start. */
    setenv("GHOSTCON_CANARY_DEADLINE_MS", "2000", 1);
    setenv("FAKE_RENDERER_HANG_AFTER_MS", "600", 1);
    unsetenv("FAKE_RENDERER_EXIT_AFTER_MS");
    unsetenv("FAKE_RENDERER_NEVER_HEARTBEAT");
    setenv("GHOSTCON_DISABLE_WALL", "1", 1);
    setenv("GHOSTCON_RUN_DIR", run_dir, 1);
    setenv("FAKE_RENDERER_REAL_PTY", "1", 1);
    setenv("FAKE_RENDERER_WRITE_MARKER", "echo GHOSTCON_HANG_MARKER_98765\n", 1);
    setenv("FAKE_RENDERER_CHECK_MARKER", "GHOSTCON_HANG_MARKER_98765", 1);

    int stderr_fd;
    pid_t sup_pid = spawn_supervisor_with_registry(&stderr_fd, registry_sock);
    if (sup_pid < 0) {
        fprintf(stderr, "FAIL: session: could not spawn supervisor\n");
        failures++;
        goto cleanup_ptyserv;
    }

    char log[16384];
    size_t log_len = 0, pos = 0;
    log[0] = '\0';

    bool ok = wait_for_log_seq(stderr_fd, log, sizeof(log), &log_len, &pos,
                               "PTY_CHILD_PID: ", 3000);
    if (ok)
        sscanf(log + pos, "%d", &pid1);
    ok = ok && wait_for_log_seq(stderr_fd, log, sizeof(log), &log_len, &pos,
                                "claimed VT, ACTIVE", 3000);
    if (!ok) {
        fprintf(stderr, "FAIL: session: first instance never reached ACTIVE with a real pty child\n");
        failures++;
        goto cleanup_supervisor;
    }
    if (pid1 <= 0) {
        fprintf(stderr, "FAIL: session: first instance never reported a real PTY_CHILD_PID\n");
        failures++;
    } else {
        printf("PASS: real pty child spawned (pid %d)\n", pid1);
    }

    /* FAKE_RENDERER_HANG_AFTER_MS (600) + GHOSTCON_CANARY_DEADLINE_MS
       (2000, see its own comment above on why this scenario needs it
       wider than the other scenarios' 300) is the real floor here --
       needs comfortable margin past that, not the 2000ms this used to
       be (found live: same class of mistake as the ACTIVE-wait
       timeout above, just for the hang side of the same widened
       deadline). */
    if (!wait_for_log_seq(stderr_fd, log, sizeof(log), &log_len, &pos,
                          "renderer hung", 4000)) {
        fprintf(stderr, "FAIL: session: hang was never detected\n");
        failures++;
        goto cleanup_supervisor;
    }
    printf("PASS: hang detected\n");

    /* pty child survival: still alive right after the kill, using the
       real pid captured above, not a fresh lookup that could paper
       over the process having actually died and something else
       reusing the pid. */
    if (pid1 > 0) {
        if (kill(pid1, 0) == 0) {
            printf("PASS: pty child (pid %d) survived the hung renderer's kill\n", pid1);
        } else {
            fprintf(stderr, "FAIL: session: pty child (pid %d) did not survive: %s\n", pid1, strerror(errno));
            failures++;
        }
    }

    /* Recovery file: correct reason AND a real (matching) pid, not
       the -1 placeholder every recovery file used to contain before
       supervisor/main.c's query_pty_child_pid(). */
    char recovery_path[512];
    snprintf(recovery_path, sizeof(recovery_path), "%s/recovery-tty%d.json", run_dir, TEST_VT);
    FILE *rf = NULL;
    for (int i = 0; i < 20 && !rf; i++) {
        rf = fopen(recovery_path, "r");
        if (!rf)
            usleep(100000);
    }
    if (!rf) {
        fprintf(stderr, "FAIL: session: recovery file %s not written\n", recovery_path);
        failures++;
    } else {
        char buf[512];
        size_t n = fread(buf, 1, sizeof(buf) - 1, rf);
        buf[n] = '\0';
        fclose(rf);
        char want[64];
        snprintf(want, sizeof(want), "\"pty_child_pid\":%d", pid1);
        if (!strstr(buf, "\"reason\":\"hang\"")) {
            fprintf(stderr, "FAIL: session: recovery file missing hang reason: %s\n", buf);
            failures++;
        } else if (pid1 > 0 && !strstr(buf, want)) {
            fprintf(stderr, "FAIL: session: recovery file has wrong/missing pty_child_pid (want %s): %s\n", want, buf);
            failures++;
        } else {
            printf("PASS: recovery file has correct reason and real pty_child_pid\n");
        }
    }

    /* Ring-buffer replay carrying prior session output across the
       respawn -- the actual "session continuity" proof, not just "a
       new shell appeared." See fake_renderer.c's own doc comment on
       why FAKE_RENDERER_CHECK_MARKER is checked BEFORE
       FAKE_RENDERER_WRITE_MARKER: this must be the SECOND instance
       finding the FIRST instance's marker, not an instance finding
       its own echo. */
    if (!wait_for_log_seq(stderr_fd, log, sizeof(log), &log_len, &pos,
                          "REPLAY: marker found", 3000)) {
        fprintf(stderr, "FAIL: session: replacement renderer's ring-buffer replay did not contain prior session output\n");
        failures++;
        goto cleanup_supervisor;
    }
    printf("PASS: replacement renderer recovered prior session output via ring-buffer replay\n");

    if (!wait_for_log_seq(stderr_fd, log, sizeof(log), &log_len, &pos,
                          "claimed VT, ACTIVE", 2000)) {
        fprintf(stderr, "FAIL: session: replacement renderer never reached ACTIVE\n");
        failures++;
    } else {
        printf("PASS: replacement renderer reached ACTIVE\n");
    }

cleanup_supervisor:
    kill_supervisor_tree(sup_pid);
    close(stderr_fd);
cleanup_ptyserv:
    /* Kill the pty child too, not just ghost-ptyserv -- it's a
       SEPARATE process (ghost-ptyserv forks one per VT on first
       registry GET), so killing ghost-ptyserv alone leaves it
       orphaned and still running, still holding open the copies of
       this test binary's stdout/stderr it inherited across the
       fork/exec chain. Found live: under meson's test harness
       (which reads those fds to EOF to capture output) that orphan
       kept the pipe open long after this function itself returned,
       making the whole test process look hung for the remainder of
       meson's timeout even though every scenario had already finished
       and printed its result -- a plain terminal run of the same
       binary never showed this, since nothing there was blocked
       reading those fds to EOF. */
    if (pid1 > 0)
        kill(pid1, SIGKILL);
    kill(ptyserv_pid, SIGKILL);
    waitpid(ptyserv_pid, NULL, 0);
    unsetenv("GHOSTCON_RUN_DIR");
    unsetenv("FAKE_RENDERER_REAL_PTY");
    unsetenv("FAKE_RENDERER_WRITE_MARKER");
    unsetenv("FAKE_RENDERER_CHECK_MARKER");
    unsetenv("FAKE_RENDERER_HANG_AFTER_MS");
cleanup_rundir:
    {
        char rm_cmd2[600];
        snprintf(rm_cmd2, sizeof(rm_cmd2), "rm -rf '%s'", run_dir);
        system(rm_cmd2);
    }
}

/* Point 6 of PLAN.md's induced-hang checklist: repeated triggers
   within one rate-limit window should only broadcast once. Doesn't
   need a real pty child (unlike scenario_hang_preserves_session
   above) -- just the supervisor cycling through several hang/respawn
   rounds and GHOSTCON_WALL_DEBUG_LOG counting how many broadcasts it
   would have made (see wall_broadcast()'s own comment on why this,
   not GHOSTCON_DISABLE_WALL, is what a rate-limit test needs -- the
   disable path skips the rate-limit bookkeeping entirely, so it can
   never be used to test the limiter itself). */
static void
scenario_wall_rate_limit(void)
{
    setenv("GHOSTCON_CANARY_DEADLINE_MS", "200", 1);
    setenv("FAKE_RENDERER_HANG_AFTER_MS", "100", 1);
    unsetenv("FAKE_RENDERER_EXIT_AFTER_MS");
    unsetenv("FAKE_RENDERER_NEVER_HEARTBEAT");
    unsetenv("GHOSTCON_DISABLE_WALL"); /* must NOT be set -- see above */
    setenv("GHOSTCON_WALL_DEBUG_LOG", "1", 1);
    /* Wider than this scenario's whole observation window below, so
       every hang after the first one falls inside the same window and
       must be suppressed -- the real 60s default would make this test
       either flaky (window too short relative to it) or slow (window
       long enough to be safe). */
    setenv("GHOSTCON_WALL_RATE_LIMIT_SECS", "5", 1);

    int stderr_fd;
    pid_t pid = spawn_supervisor(&stderr_fd);
    if (pid < 0) {
        fprintf(stderr, "FAIL: wall_rate_limit: could not spawn supervisor\n");
        failures++;
        goto cleanup;
    }

    /* Drain stderr for a fixed window (long enough for several
       ~(100ms hang-after + 200ms canary-deadline + respawn grace)
       cycles) counting both "renderer hung" and "would wall-
       broadcast" occurrences, rather than waiting for any single
       needle -- this scenario's assertion is about a COUNT across the
       whole window, not a single event's presence. */
    char acc[16384];
    size_t acc_len = 0;
    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += 3;
    for (;;) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long remaining_ms = (deadline.tv_sec - now.tv_sec) * 1000 +
                             (deadline.tv_nsec - now.tv_nsec) / 1000000L;
        if (remaining_ms <= 0 || acc_len >= sizeof(acc) - 1)
            break;
        struct pollfd pfd = { .fd = stderr_fd, .events = POLLIN };
        int rv = poll(&pfd, 1, (int)remaining_ms);
        if (rv <= 0)
            continue;
        ssize_t r = read(stderr_fd, acc + acc_len, sizeof(acc) - 1 - acc_len);
        if (r <= 0)
            break;
        acc_len += (size_t)r;
        acc[acc_len] = '\0';
    }

    int hang_count = 0, broadcast_count = 0;
    for (const char *p = acc; (p = strstr(p, "renderer hung")); p++)
        hang_count++;
    for (const char *p = acc; (p = strstr(p, "would wall-broadcast")); p++)
        broadcast_count++;

    if (hang_count < 2) {
        fprintf(stderr, "FAIL: wall_rate_limit: only saw %d hang(s) in the observation window -- test didn't actually exercise repeated hangs\n", hang_count);
        failures++;
    } else if (broadcast_count != 1) {
        fprintf(stderr, "FAIL: wall_rate_limit: %d hang(s) but %d broadcast(s) -- expected exactly 1 (rate limit not enforced)\n", hang_count, broadcast_count);
        failures++;
    } else {
        printf("PASS: %d hangs within the rate-limit window produced exactly 1 wall broadcast\n", hang_count);
    }

    kill_supervisor_tree(pid);
    close(stderr_fd);

cleanup:
    unsetenv("FAKE_RENDERER_HANG_AFTER_MS");
    unsetenv("GHOSTCON_WALL_DEBUG_LOG");
    unsetenv("GHOSTCON_WALL_RATE_LIMIT_SECS");
    setenv("GHOSTCON_DISABLE_WALL", "1", 1); /* restore the default every other scenario expects */
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
    scenario_hang_preserves_session();
    scenario_wall_rate_limit();

    if (failures > 0) {
        fprintf(stderr, "%d scenario(s) failed\n", failures);
        return 1;
    }
    printf("ALL TESTS PASSED\n");
    return 0;
}
