#pragma once

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Domain-tagged debug logging (PLAN.md refactor 3.1).
 *
 * Header-only and dependency-free.
 *
 * Output goes to stderr, which journald captures for ghostcon.service through
 * stdio forwarding, the same channel diag.c uses for backtraces. term/ can
 * include this without linking libsystemd, unlike ghostcon_diag_log_warning(),
 * which would pull it into ghostcon_dump and the test binaries.
 *
 * GHOSTCON_LOG is a comma-separated domain list; "all" is a wildcard. When it is
 * unset or empty, every GC_LOG site is silent, so normal operation logs nothing
 * new.
 *
 * Examples:
 *
 *   GHOSTCON_LOG=input,selection ghostcon-core 4
 *   GHOSTCON_LOG=vt,drm              ghostcon-core 4
 *   GHOSTCON_LOG=stream              ghostcon-core 4
 *   GHOSTCON_LOG=all                 ghostcon-core 4
 *
 * The domain becomes the tag verbatim:
 *
 *   GC_LOG(drm, "master retry %d", i)  ->  gc(drm): master retry 2
 *
 * Always-on warnings still go through ghostcon_diag_log_warning().
 *
 * Domains in use:
 *
 *   vt         VT acquire/release lifecycle (core/main.c)
 *   drm        DRM-master retry loop, KMS failures (core/display.c)
 *   cursor     hardware-cursor sprite/state commits (core/display.c)
 *   input      libinput open/close, dispatch errors (core/main.c)
 *   selection  click-drag selection transitions (core/ui.c)
 *   config     TOML hot-reload decisions (core/main.c)
 *   pty        transport connect/read lifecycle (core/main.c)
 *   stream     unhandled/overridden escape sequences (term/)
 */

/* Pure token matcher: does `spec` (comma-separated, NULL/empty = no)
 * contain the token `domain`, or the wildcard "all"?
 *
 * Exposed (not hidden inside the macro) so tests/unit code can exercise the
 * parsing rules without the env-var static state.
 */
static inline int
gc_log_domain_matches(const char *spec, const char *domain)
{
    if (!spec || !spec[0] || !domain || !domain[0])
        return 0;
    size_t dlen = strlen(domain);
    const char *p = spec;
    for (;;)
    {
        const char *tok = p;
        while (*p && *p != ',')
            p++;
        size_t tlen = (size_t)(p - tok);
        bool eq_all = (tlen == 3 && tok[0] == 'a' && tok[1] == 'l' && tok[2] == 'l');
        bool eq_dom = (tlen == dlen && strncmp(tok, domain, dlen) == 0);
        if (eq_all || eq_dom)
            return 1;
        if (*p != ',') /* '\0': end of spec */
            return 0;
        p++; /* skip comma, next token */
    }
}

/* Note the GNU ##__VA_ARGS__ tail-comma elision: with no varargs, e.g.
 * GC_LOG(vt, "switching away"), expands to plain fprintf. Works under
 * -std=c11 (warning_level=2, no -pedantic) with GCC/Clang.
 */
#define GC_LOG(domain, fmt, ...)                                         \
    do {                                                                 \
        /* Cached per-domain verdict (-1 = uninitialized): GHOSTCON_LOG   \
           is static after startup, so paying getenv() once per site      \
           and then a single byte-test per call is cheap enough to        \
           keep enabled sites in hot paths. */                            \
        static signed char gc_log_state = -1;                            \
        if (gc_log_state < 0)                                            \
            gc_log_state = (signed char)gc_log_domain_matches(            \
                getenv("GHOSTCON_LOG"), #domain);                         \
        if (gc_log_state)                                                \
            fprintf(stderr, "gc(" #domain "):" fmt "\n",                  \
                    ##__VA_ARGS__);                                       \
    } while (0)
