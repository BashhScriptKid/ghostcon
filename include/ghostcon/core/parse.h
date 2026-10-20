#pragma once

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>

/* Checked decimal-string conversion helpers.
 *
 * atoi()/atol() cannot report errors: invalid input silently becomes 0 and
 * trailing garbage is ignored.
 *
 * These helpers accept the same normal decimal forms (optional sign,
 * leading whitespace), but require the whole string to be consumed and the
 * value to fit the target type.
 *
 * On failure *out is left untouched, so callers keep their existing
 * default/fallback and only overwrite it on a valid parse.
 *
 * Header-only so src/, tools/, and tests/ can all use them without a
 * library dependency.
 */

/* One body for the signed conversions, one for the unsigned ones.
 *
 * The signed body allows a leading '-'
 *
 * the unsigned one rejects it (a negative value has no meaning for an unsigned
 * type, and strtoul() would silently wrap it). Both require the whole string to
 * be consumed, and reject a value that does not fit.
 *
 * The four public conversions below are these two plus a range check for their
 * own target type.
 */
static inline bool ghostcon_parse_signed(const char *s, long *out)
{
    if (!s || *s == '\0')
        return false;
    errno = 0;
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (end == s || errno == ERANGE || (*end != '\0'))
        return false;
    *out = v;
    return true;
}

static inline bool ghostcon_parse_unsigned(const char *s, unsigned long *out)
{
    if (!s || *s == '\0' || *s == '-')
        return false;
    errno = 0;
    char *end = NULL;
    unsigned long v = strtoul(s, &end, 10);
    if (end == s || errno == ERANGE || (*end != '\0'))
        return false;
    *out = v;
    return true;
}

static inline bool ghostcon_parse_int(const char *s, int *out)
{
    long v;
    if (!ghostcon_parse_signed(s, &v) || v < INT_MIN || v > INT_MAX)
        return false;
    *out = (int)v;
    return true;
}

static inline bool ghostcon_parse_long(const char *s, long *out)
{
    return ghostcon_parse_signed(s, out);
}

static inline bool ghostcon_parse_uint(const char *s, unsigned *out)
{
    unsigned long v;
    if (!ghostcon_parse_unsigned(s, &v) || v > UINT_MAX)
        return false;
    *out = (unsigned)v;
    return true;
}

static inline bool ghostcon_parse_ulong(const char *s, unsigned long *out)
{
    return ghostcon_parse_unsigned(s, out);
}
