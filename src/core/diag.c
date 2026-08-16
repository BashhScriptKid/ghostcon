#define _DEFAULT_SOURCE /* strsignal() under -std=c11 */

#include "ghostcon/core/diag.h"

#include <execinfo.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include <systemd/sd-journal.h>

#define GHOSTCON_DIAG_MAX_FRAMES 32
#define GHOSTCON_DIAG_HEAD_FRAMES 8

static const char *g_component = "ghostcon";
static int g_vt_num = -1;

static const int FATAL_SIGNALS[] = { SIGSEGV, SIGABRT, SIGFPE, SIGILL, SIGBUS };

/* backtrace()/backtrace_symbols() call malloc, which is not
   async-signal-safe -- a real risk here since the fault that got us
   into this handler may itself have been heap corruption. This is a
   deliberate, pragmatic tradeoff shared by most real-world crash
   reporters (e.g. Breakpad): best-effort diagnostics on the way down
   beat none, and if this itself deadlocks or re-faults, the process
   was going to die anyway. backtrace_symbols_fd() below is the
   async-signal-safer variant (no malloc, writes raw fd) and is what
   still gets the full trace out even if the journald send above it
   fails or backtrace_symbols() itself faults. */
static void
crash_handler(int signum)
{
    void *frames[GHOSTCON_DIAG_MAX_FRAMES];
    int n = backtrace(frames, GHOSTCON_DIAG_MAX_FRAMES);

    char **symbols = backtrace_symbols(frames, n);

    char head[2048];
    size_t head_len = 0;
    int head_frames = n < GHOSTCON_DIAG_HEAD_FRAMES ? n : GHOSTCON_DIAG_HEAD_FRAMES;
    for (int i = 0; i < head_frames && head_len < sizeof(head); i++) {
        int written = snprintf(head + head_len, sizeof(head) - head_len, "%s%s",
                                i == 0 ? "" : " | ",
                                symbols ? symbols[i] : "?");
        if (written > 0)
            head_len += (size_t)written;
    }

    sd_journal_send(
        "MESSAGE=%s crashed on signal %d (%s), backtrace head: %s",
        g_component, signum, strsignal(signum), head,
        "PRIORITY=%d", LOG_CRIT,
        "GHOSTCON_COMPONENT=%s", g_component,
        "GHOSTCON_VT=%d", g_vt_num,
        "GHOSTCON_SIGNAL=%d", signum,
        NULL);

    /* Full, untruncated trace to stderr — journald captures this via
       its own stdio forwarding for services; for an interactive run
       it lands on the terminal. */
    backtrace_symbols_fd(frames, n, STDERR_FILENO);

    /* SA_RESETHAND (see ghostcon_diag_init) already reset this signal's
       disposition to default on entry; re-raising here makes the
       process actually die with the correct signal/exit status and,
       if enabled, a real core dump — this handler's job is reporting,
       not survival. */
    raise(signum);
}

void
ghostcon_diag_init(const char *component, int vt_num)
{
    g_component = component;
    g_vt_num = vt_num;

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = crash_handler;
    /* One-shot: a fault recurring inside the handler itself hits the
       (now-default) disposition directly instead of recursing. */
    sa.sa_flags = SA_RESETHAND;
    sigemptyset(&sa.sa_mask);

    for (size_t i = 0; i < sizeof(FATAL_SIGNALS) / sizeof(FATAL_SIGNALS[0]); i++)
        sigaction(FATAL_SIGNALS[i], &sa, NULL);
}

void
ghostcon_diag_log_warning(const char *fmt, ...)
{
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    sd_journal_send(
        "MESSAGE=%s", msg,
        "PRIORITY=%d", LOG_WARNING,
        "GHOSTCON_COMPONENT=%s", g_component,
        "GHOSTCON_VT=%d", g_vt_num,
        NULL);
}
