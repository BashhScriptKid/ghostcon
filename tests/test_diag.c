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

/* On success, copies the matched entry's raw -o export text into
   out_buf (caller-owned) so the caller can inspect fields beyond the
   three checked here -- specifically the backtrace embedded in
   MESSAGE, which diag.c's own crash_handler() doc comment says is
   deliberately truncated to GHOSTCON_DIAG_HEAD_FRAMES (8) frames. */
static bool
journal_has_entry(pid_t pid, char *out_buf, size_t out_cap)
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
            size_t copy_len = n < out_cap - 1 ? n : out_cap - 1;
            memcpy(out_buf, buf, copy_len);
            out_buf[copy_len] = '\0';
            return true;
        }
        usleep(100000);
    }
    return false;
}

/* diag.c's crash_handler() truncates the journald-embedded backtrace
   to the first GHOSTCON_DIAG_HEAD_FRAMES (8) frames -- a bare abort()
   only produces a 2-3 frame real stack (main -> abort -> libc
   internals), which is too shallow to ever exceed that cap, so a test
   using it could only prove symbolization works at all, not that
   truncation actually engages (the assertion below would pass
   vacuously either way). noinline + a volatile asm barrier after the
   recursive call keep the compiler from collapsing this into a loop
   or tail call, so the real call stack at crash time genuinely has
   RECURSE_DEPTH+ frames for backtrace() to capture. */
#define RECURSE_DEPTH 20

/* GCC's -Winfinite-recursion is a false positive here -- its static
   analysis doesn't track that `depth` strictly decreases toward the
   depth<=0 base case below, which does terminate every real call. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winfinite-recursion"
/* NOT static -- -rdynamic (see this binary's meson.build entry) only
   exports symbols that are already GLOBAL to the dynamic symbol
   table; a `static` function has internal linkage regardless of
   -rdynamic, so backtrace_symbols() (which resolves names via
   dladdr(), .dynsym only) would still show this as a bare offset. */
void __attribute__((noinline))
recurse_and_abort(int depth)
{
    if (depth <= 0) {
        abort();
        return;
    }
    recurse_and_abort(depth - 1);
    __asm__ volatile("");
}
#pragma GCC diagnostic pop

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
        recurse_and_abort(RECURSE_DEPTH);
        _exit(1); /* unreachable */
    }

    int status;
    waitpid(child, &status, 0);
    if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGABRT) {
        fprintf(stderr, "FAIL: child did not die of SIGABRT (status=%d)\n", status);
        return 1;
    }
    printf("PASS: child crashed with SIGABRT as expected (diag.c re-raised correctly)\n");

    char journal_buf[8192];
    if (!journal_has_entry(child, journal_buf, sizeof(journal_buf))) {
        fprintf(stderr, "FAIL: no matching journald entry found for pid %d\n", (int)child);
        return 1;
    }
    printf("PASS: journald entry found with GHOSTCON_COMPONENT/VT/SIGNAL fields\n");

    /* Isolate just the MESSAGE=... line -- journalctl -o export emits
       one FIELD=value per line for plain-text values (ours always is,
       since backtrace_symbols() entries and " | " joins never contain
       raw newlines), so bounding on the next '\n' keeps later fields
       (GHOSTCON_COMPONENT=, etc.) from being swept into the backtrace
       checks below. */
    const char *msg_start = strstr(journal_buf, "MESSAGE=");
    if (!msg_start) {
        fprintf(stderr, "FAIL: journald entry has no MESSAGE field\n");
        return 1;
    }
    const char *msg_end = strchr(msg_start, '\n');
    size_t msg_len = msg_end ? (size_t)(msg_end - msg_start) : strlen(msg_start);
    char msg_line[2200];
    size_t copy_len = msg_len < sizeof(msg_line) - 1 ? msg_len : sizeof(msg_line) - 1;
    memcpy(msg_line, msg_start, copy_len);
    msg_line[copy_len] = '\0';

    const char *head = strstr(msg_line, "backtrace head:");
    if (!head) {
        fprintf(stderr, "FAIL: journald MESSAGE missing \"backtrace head:\": %s\n", msg_line);
        return 1;
    }
    head += strlen("backtrace head:");
    while (*head == ' ')
        head++;
    if (*head == '\0') {
        fprintf(stderr, "FAIL: backtrace head is empty: %s\n", msg_line);
        return 1;
    }
    /* recurse_and_abort's real call stack has well over 8 frames at
       crash time (RECURSE_DEPTH=20 self-recursive calls alone, plus
       main/__libc_start_main/etc below them) -- if truncation weren't
       actually engaging, this specific check would be the one to
       catch it, unlike a bare-abort() version of this test. */
    if (!strstr(head, "recurse_and_abort")) {
        fprintf(stderr, "FAIL: backtrace head doesn't even reach into the recursive frames -- backtrace() itself may be broken: %s\n", head);
        return 1;
    }
    printf("PASS: journald MESSAGE contains a real, symbolized backtrace\n");

    /* Truncation: diag.c caps the embedded head at
       GHOSTCON_DIAG_HEAD_FRAMES=8 frames joined by " | ", so at most 7
       separators should appear regardless of how deep the real stack
       is (RECURSE_DEPTH=20 guarantees the real stack is deeper than
       that cap, so this is a genuine truncation check, not a vacuous
       one). */
    int seps = 0;
    for (const char *p = head; (p = strstr(p, " | ")); p += 3)
        seps++;
    if (seps > 7) {
        fprintf(stderr, "FAIL: backtrace head has %d separators (>7) for a %d-deep real stack -- GHOSTCON_DIAG_HEAD_FRAMES truncation not enforced: %s\n",
                seps, RECURSE_DEPTH, head);
        return 1;
    }
    printf("PASS: backtrace head correctly truncated (%d frame(s) shown, <=8 as documented, despite a %d-deep real stack)\n",
           seps + 1, RECURSE_DEPTH);

    printf("ALL TESTS PASSED\n");
    return 0;
}
