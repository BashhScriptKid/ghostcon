#pragma once

/* ------------------------------------------------------------------ */
/* Crash self-reporting                                                */
/*                                                                     */
/* No kmscon equivalent (PLAN.md). Installs handlers for the fatal      */
/* signals (SIGSEGV, SIGABRT, SIGFPE, SIGILL, SIGBUS) that capture a   */
/* backtrace and log it to journald with structured fields before      */
/* letting the process die normally (core dump, correct exit status)   */
/* — the supervisor does NOT do the catching (PLAN.md: "This must be   */
/* self-contained inside ghostcon-core/wrap ... The binary reports on  */
/* itself"). Call ghostcon_diag_init() once, as early as possible in    */
/* main(), before anything that could plausibly crash.                 */
/* ------------------------------------------------------------------ */

/* component: short identifier for MESSAGE/journald field
   (e.g. "ghostcon-core", "ghost-ptyserv"). Must outlive the process —
   pass a string literal or otherwise static storage. vt_num: the VT
   this process instance is for, or -1 if not applicable (e.g.
   ghost-ptyserv, which isn't per-VT). */
void ghostcon_diag_init(const char *component, int vt_num);

/* Logs a non-fatal warning to journald with the same structured fields
   diag's crash handler uses, without a signal/crash involved. Useful
   for "this shouldn't happen but isn't fatal" conditions elsewhere in
   the codebase. printf-style format string. */
void ghostcon_diag_log_warning(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));
