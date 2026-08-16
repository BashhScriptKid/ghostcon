#include "ghostcon/ptyserv/protocol.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

size_t
ghostcon_ptyserv_format_get(char *buf, size_t buf_len, int vtnum)
{
    int n = snprintf(buf, buf_len, "GET %d\n", vtnum);
    if (n < 0 || (size_t)n >= buf_len)
        return 0;
    return (size_t)n;
}

bool
ghostcon_ptyserv_parse_get(const char *line, int *vtnum)
{
    int v;
    if (sscanf(line, "GET %d", &v) != 1)
        return false;
    *vtnum = v;
    return true;
}

size_t
ghostcon_ptyserv_format_ok(char *buf, size_t buf_len,
                            int pid, const char *socket_path)
{
    int n = snprintf(buf, buf_len, "OK pid=%d socket=%s\n", pid, socket_path);
    if (n < 0 || (size_t)n >= buf_len)
        return 0;
    return (size_t)n;
}

bool
ghostcon_ptyserv_parse_ok(const char *line, int *pid, char *socket_path)
{
    int p;
    char path[GHOSTCON_PTYSERV_LINE_MAX];
    if (sscanf(line, "OK pid=%d socket=%255s", &p, path) != 2)
        return false;
    *pid = p;
    strncpy(socket_path, path, GHOSTCON_PTYSERV_LINE_MAX - 1);
    socket_path[GHOSTCON_PTYSERV_LINE_MAX - 1] = '\0';
    return true;
}

size_t
ghostcon_ptyserv_format_resize(char *buf, size_t buf_len, int rows, int cols)
{
    int n = snprintf(buf, buf_len, "RESIZE %d %d\n", rows, cols);
    if (n < 0 || (size_t)n >= buf_len)
        return 0;
    return (size_t)n;
}

bool
ghostcon_ptyserv_parse_resize(const char *line, int *rows, int *cols)
{
    int r, c;
    if (sscanf(line, "RESIZE %d %d", &r, &c) != 2)
        return false;
    *rows = r;
    *cols = c;
    return true;
}

size_t
ghostcon_ptyserv_format_clear(char *buf, size_t buf_len)
{
    int n = snprintf(buf, buf_len, "CLEAR\n");
    if (n < 0 || (size_t)n >= buf_len)
        return 0;
    return (size_t)n;
}

bool
ghostcon_ptyserv_parse_clear(const char *line)
{
    return strncmp(line, "CLEAR", 5) == 0;
}
