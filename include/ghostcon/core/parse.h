#pragma once

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>

/* Checked decimal-string conversion helpers.
 *
 * atoi()/atol() cannot report errors: invalid input silently becomes 0
 * and trailing garbage is ignored. These helpers accept the same normal
 * decimal forms (optional sign, leading whitespace), but require the
 * whole string to be consumed and the value to fit the target type.
 * On failure *out is left untouched, so callers keep their existing
 * default/fallback and only overwrite it on a valid parse.
 *
 * Header-only so src/, tools/, and tests/ can all use them without a
 * library dependency.
 */

static inline bool
ghostcon_parse_trailing_ok(const char *end)
{
    while (*end != '\0' && isspace((unsigned char)*end))
        end++;
    return *end == '\0';
}

static inline bool
ghostcon_parse_int(const char *s, int *out)
{
    if (!s || *s == '\0')
        return false;
    errno = 0;
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (end == s || errno == ERANGE || !ghostcon_parse_trailing_ok(end))
        return false;
    if (v < INT_MIN || v > INT_MAX)
        return false;
    *out = (int)v;
    return true;
}

static inline bool
ghostcon_parse_long(const char *s, long *out)
{
    if (!s || *s == '\0')
        return false;
    errno = 0;
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (end == s || errno == ERANGE || !ghostcon_parse_trailing_ok(end))
        return false;
    *out = v;
    return true;
}

static inline bool
ghostcon_parse_uint(const char *s, unsigned *out)
{
    if (!s || *s == '\0' || *s == '-')
        return false;
    errno = 0;
    char *end = NULL;
    unsigned long v = strtoul(s, &end, 10);
    if (end == s || errno == ERANGE || !ghostcon_parse_trailing_ok(end))
        return false;
    if (v > UINT_MAX)
        return false;
    *out = (unsigned)v;
    return true;
}

static inline bool
ghostcon_parse_ulong(const char *s, unsigned long *out)
{
    if (!s || *s == '\0' || *s == '-')
        return false;
    errno = 0;
    char *end = NULL;
    unsigned long v = strtoul(s, &end, 10);
    if (end == s || errno == ERANGE || !ghostcon_parse_trailing_ok(end))
        return false;
    *out = v;
    return true;
}
