#pragma once

/* readlink() is POSIX, not ISO C, so the feature test macro has to be set before
 * any system header is included. The binaries that use this header already define
 * it at the top of their own source
 *
 * this keeps the header usable on its own.
 */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Locating a sibling binary next to this executable.
 *
 * ghost-ptyserv, supervisor and undead-head each need to find the other binaries
 * in the same directory, and each of them used to carry its own byte-identical
 * copy of the same resolution.
 *
 * Why not PATH: these run under pkexec, which resets the environment (including
 * PATH) for security, so a bare name silently fails to exec. undead-head found
 * this the hard way -- it spun in a fast fork/exec-fail/restart loop under pkexec
 * before the resolution existed.
 *
 * (pty-ttyN was not covered by that earlier fix and failed the same way, one
 * binary later.)
 *
 * The resolution is via /proc/self/exe, which is absolute and symlink-resolved,
 * so it does not depend on the environment at all. If /proc is unavailable, the
 * bare name is used, which restores the old PATH-lookup behaviour as a fallback.
 *
 * Header-only, like core/parse.h, so that src/, tools/ and tests/ can all use it
 * without a library dependency.
 */
static inline void ghostcon_resolve_sibling_path(char *out, size_t out_len, const char *name)
{
    if (out_len == 0)
        return;

    char self_path[4096];
    ssize_t n = readlink("/proc/self/exe", self_path, sizeof(self_path) - 1);
    if (n <= 0)
    {
        snprintf(out, out_len, "%s", name);
        return;
    }
    self_path[n] = '\0';

    char *slash = strrchr(self_path, '/');
    if (!slash)
    {
        snprintf(out, out_len, "%s", name);
        return;
    }
    *slash = '\0';

    /* "<dir>/<name>", truncated to fit `out` exactly the way snprintf()
     * would truncate it.
     *
     * Written out rather than handed to snprintf() because the compiler
     * cannot see that the result fits -- `out_len` is a runtime value and
     * `self_path` is 4 KiB -- and warns about a truncation that is
     * deliberate here.
     */
    size_t dir_len = strlen(self_path);
    size_t name_len = strlen(name);
    size_t len = dir_len + 1 + name_len;
    if (len > out_len - 1)
        len = out_len - 1;

    size_t written = dir_len < len ? dir_len : len;
    memcpy(out, self_path, written);
    if (written < len)
    {
        out[written++] = '/';
        size_t from_name = len - written;
        if (from_name > name_len)
            from_name = name_len;
        memcpy(out + written, name, from_name);
        written += from_name;
    }
    out[written] = '\0';
}
