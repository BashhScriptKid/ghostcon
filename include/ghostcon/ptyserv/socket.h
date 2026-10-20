#pragma once

#include <stdbool.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

/* The AF_UNIX sockets ghostcon's process tree talks over.
 *
 * Three places open one: the registry socket (ghost-ptyserv listens, the renderer
 * connects), the per-VT data socket (pty-ttyN listens, the renderer connects) and
 * the per-VT control socket (pty-ttyN listens).
 *
 * The address setup and the "open a socket, and close it again if the next step
 * fails" dance were copied between them
 *
 * this is that code once.
 *
 * Header-only, like core/parse.h, so that ptyserv/ and core/ can both use it
 * without a library dependency.
 */

/* Fills `addr` with `path` as an AF_UNIX address. */
static inline void ghostcon_unix_addr(struct sockaddr_un *addr, const char *path)
{
    memset(addr, 0, sizeof(*addr));
    addr->sun_family = AF_UNIX;
    strncpy(addr->sun_path, path, sizeof(addr->sun_path) - 1);
}

/* Creates a listening socket bound to `path`.
 *
 * A stale socket file at `path` is removed first -- the socket node
 * outlives the process that made it, so a restart would otherwise fail to
 * bind.
 *
 * `backlog` is the listen() backlog. Returns the fd, or -1 on failure
 * (the fd is closed again if a later step fails).
 */
static inline int ghostcon_unix_listen_socket(const char *path, int backlog)
{
    unlink(path);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    struct sockaddr_un addr;
    ghostcon_unix_addr(&addr, path);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        close(fd);
        return -1;
    }
    if (listen(fd, backlog) < 0)
    {
        close(fd);
        return -1;
    }
    return fd;
}

/* Connects to the socket at `path`.
 *
 * Returns the fd, or -1 on failure.
 */
static inline int ghostcon_unix_connect_socket(const char *path)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    struct sockaddr_un addr;
    ghostcon_unix_addr(&addr, path);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        close(fd);
        return -1;
    }
    return fd;
}
