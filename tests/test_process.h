#pragma once

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

/* Process helpers for tests.
 *
 * Tests occasionally need to run a real helper binary (pkill, rm,
 * journalctl).
 *
 * Using system()/popen() routes through a shell, which clang-tidy flags
 * (bugprone-command-processor) and which makes argument quoting fragile.
 * These helpers exec the program directly with an argv array and no shell.
 */

/* Runs argv to completion, discarding stdout/stderr. Returns the
 * waitpid status, or -1 if fork/exec failed. */
static inline int
gc_test_run_command(char *const argv[])
{
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0)
    {
        int nullfd = open("/dev/null", O_WRONLY);
        if (nullfd >= 0)
        {
            dup2(nullfd, STDOUT_FILENO);
            dup2(nullfd, STDERR_FILENO);
            close(nullfd);
        }
        execvp(argv[0], argv);
        _exit(127);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) != pid)
        return -1;
    return status;
}

/* Runs argv, capturing its stdout into buf (NUL-terminated, truncated to
 * cap-1 bytes) and discarding stderr. Returns the byte count read, or -1
 * on fork/pipe failure. */
static inline ssize_t gc_test_capture_command(char *const argv[], char *buf, size_t cap)
{
    if (cap == 0)
        return -1;
    buf[0] = '\0';

    int fds[2];
    if (pipe(fds) != 0)
        return -1;

    pid_t pid = fork();
    if (pid < 0)
    {
        close(fds[0]);
        close(fds[1]);
        return -1;
    }
    if (pid == 0)
    {
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        close(fds[1]);
        int nullfd = open("/dev/null", O_WRONLY);
        if (nullfd >= 0)
        {
            dup2(nullfd, STDERR_FILENO);
            close(nullfd);
        }
        execvp(argv[0], argv);
        _exit(127);
    }

    close(fds[1]);
    size_t total = 0;
    while (total + 1 < cap)
    {
        ssize_t n = read(fds[0], buf + total, cap - 1 - total);
        if (n > 0)
        {
            total += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        break;
    }
    close(fds[0]);
    buf[total] = '\0';

    int status = 0;
    if (waitpid(pid, &status, 0) != pid)
        return -1;
    return (ssize_t)total;
}
