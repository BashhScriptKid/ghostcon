/*
 * test_diag — verifies core/diag.c actually reaches journald.
 *
 * Forks a child that installs the crash handler and deliberately
 * aborts, then queries journald (matched by the child's PID, a
 * structured field journald adds to every entry automatically) for the
 * GHOSTCON_COMPONENT/GHOSTCON_VT/GHOSTCON_SIGNAL fields diag.c sends.
 * This is the exact check PLAN.md's testing strategy calls for:
 * "confirm the diag.c/panic-handler [...] wiring" against a real
 * triggered signal, not just a code read.
 */

#define _DEFAULT_SOURCE

#include "ghostcon/core/diag.h"

#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define TEST_VT 42
#define TEST_COMPONENT "ghostcon-test-diag"

static bool
journal_has_entry(pid_t pid)
{
    /* _PID= alone isn't enough to disambiguate -- PIDs get reused
       across boots, and a bare journalctl query isn't scoped to the
       current boot by default, so it can match an unrelated historical
       entry. -b (current boot only) + _COMM= (this binary's name)
       together with _PID= make the match actually unique. */
    char cmd[256];
    snprintf(cmd, sizeof(cmd),
             "journalctl -b _PID=%d _COMM=test_diag --no-pager -o export 2>/dev/null",
             (int)pid);

    for (int attempt = 0; attempt < 20; attempt++) {
        FILE *p = popen(cmd, "r");
        if (!p)
            return false;

        char buf[8192];
        size_t n = fread(buf, 1, sizeof(buf) - 1, p);
        pclose(p);
        buf[n] = '\0';

        if (strstr(buf, "GHOSTCON_COMPONENT=" TEST_COMPONENT) &&
            strstr(buf, "GHOSTCON_VT=42") &&
            strstr(buf, "GHOSTCON_SIGNAL=6" /* SIGABRT */)) {
            return true;
        }
        usleep(100000);
    }
    return false;
}

int
main(void)
{
    pid_t child = fork();
    if (child < 0) {
        fprintf(stderr, "FAIL: fork\n");
        return 1;
    }

    if (child == 0) {
        /* Suppress the default core-dump-message noise on stderr for
           this deliberate, expected crash; diag.c's own handler still
           runs and does its job regardless. */
        ghostcon_diag_init(TEST_COMPONENT, TEST_VT);
        abort();
        _exit(1); /* unreachable */
    }

    int status;
    waitpid(child, &status, 0);
    if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGABRT) {
        fprintf(stderr, "FAIL: child did not die of SIGABRT (status=%d)\n", status);
        return 1;
    }
    printf("PASS: child crashed with SIGABRT as expected (diag.c re-raised correctly)\n");

    if (!journal_has_entry(child)) {
        fprintf(stderr, "FAIL: no matching journald entry found for pid %d\n", (int)child);
        return 1;
    }
    printf("PASS: journald entry found with GHOSTCON_COMPONENT/VT/SIGNAL fields\n");

    printf("ALL TESTS PASSED\n");
    return 0;
}
