#define _DEFAULT_SOURCE /* usleep() under -std=c11 without this */

#include "ghostcon/core/transport.h"
#include "ghostcon/ptyserv/protocol.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static int
connect_unix(const char *path)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static bool
write_all(int fd, const char *buf, size_t n)
{
    while (n > 0) {
        ssize_t w = write(fd, buf, n);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        buf += w;
        n -= (size_t)w;
    }
    return true;
}

static void
dirname_inplace(char *path)
{
    char *slash = strrchr(path, '/');
    if (slash)
        *slash = '\0';
    else
        path[0] = '\0';
}

bool
ghostcon_transport_connect(ghostcon_transport_t *t,
                            const char *registry_socket_path, int vtnum)
{
    t->fd = -1;
    t->ctl_fd = -1;

    int reg_fd = connect_unix(registry_socket_path);
    if (reg_fd < 0) {
        fprintf(stderr, "transport: connect %s: %s\n", registry_socket_path, strerror(errno));
        return false;
    }

    char req[GHOSTCON_PTYSERV_LINE_MAX];
    size_t req_len = ghostcon_ptyserv_format_get(req, sizeof(req), vtnum);
    if (req_len == 0 || !write_all(reg_fd, req, req_len)) {
        fprintf(stderr, "transport: failed to send registry request\n");
        close(reg_fd);
        return false;
    }

    char resp[GHOSTCON_PTYSERV_LINE_MAX];
    ssize_t r = read(reg_fd, resp, sizeof(resp) - 1);
    close(reg_fd);
    if (r <= 0) {
        fprintf(stderr, "transport: no response from registry\n");
        return false;
    }
    resp[r] = '\0';

    int pid;
    char pty_socket[GHOSTCON_PTYSERV_LINE_MAX];
    if (!ghostcon_ptyserv_parse_ok(resp, &pid, pty_socket)) {
        fprintf(stderr, "transport: registry error for vt %d: %s", vtnum, resp);
        return false;
    }
    t->pty_child_pid = pid;

    t->fd = connect_unix(pty_socket);
    if (t->fd < 0) {
        fprintf(stderr, "transport: connect %s: %s\n", pty_socket, strerror(errno));
        return false;
    }

    /* Best-effort: the control socket connection failing doesn't fail
       the whole connect -- see ctl_fd's own doc comment. Derived from
       pty_socket's own directory (not registry_socket_path's) since
       that's the value ghost-ptyserv itself actually reported, the
       more authoritative source if the two ever diverged.

       Retried, NOT a single attempt -- found live: a single-shot
       connect_unix() here raced pty_child.c's own ctl-socket bind at
       early-boot startup (both sockets bind around the same moment,
       but the data socket above already gets 50 retries via its own
       caller loop, while this one previously had none). Losing that
       race left ctl_fd permanently -1 for the rest of this process's
       lifetime -- every ghostcon_transport_resize() call for the
       entire session (including from a later Ctrl+=/Ctrl+Minus zoom)
       then silently no-ops forever (see that function's own "best-
       effort, not an error" early return), with no error logged
       anywhere to explain why a resize that clearly ran produced no
       visible effect. Same retry budget as the data socket connect
       above, so both sockets get an equally fair chance to catch up
       with pty_child.c's startup. */
    char ctl_dir[GHOSTCON_PTYSERV_LINE_MAX];
    snprintf(ctl_dir, sizeof(ctl_dir), "%s", pty_socket);
    dirname_inplace(ctl_dir);
    char ctl_path[GHOSTCON_PTYSERV_LINE_MAX];
    snprintf(ctl_path, sizeof(ctl_path), GHOSTCON_PTY_CTL_SOCKET_FMT, ctl_dir, vtnum);
    for (int i = 0; i < 50 && t->ctl_fd < 0; i++) {
        t->ctl_fd = connect_unix(ctl_path);
        if (t->ctl_fd < 0)
            usleep(20000);
    }
    if (t->ctl_fd < 0) {
        /* Still best-effort -- don't fail the whole connect over this,
           the data path is what actually matters most -- but no longer
           SILENT: this is exactly the failure mode that made resize
           permanently (and mysteriously) stop working before. */
        fprintf(stderr, "transport: connect %s failed after retries -- "
                        "resize requests will silently no-op for this session\n",
                ctl_path);
    }

    return true;
}

void
ghostcon_transport_close(ghostcon_transport_t *t)
{
    if (t->fd >= 0) {
        close(t->fd);
        t->fd = -1;
    }
    if (t->ctl_fd >= 0) {
        close(t->ctl_fd);
        t->ctl_fd = -1;
    }
}

int
ghostcon_transport_fd(const ghostcon_transport_t *t)
{
    return t->fd;
}

ssize_t
ghostcon_transport_read(ghostcon_transport_t *t, uint8_t *buf, size_t len)
{
    return read(t->fd, buf, len);
}

ssize_t
ghostcon_transport_write(ghostcon_transport_t *t, const uint8_t *buf, size_t len)
{
    return write(t->fd, buf, len);
}

bool
ghostcon_transport_resize(ghostcon_transport_t *t, int rows, int cols)
{
    if (t->ctl_fd < 0)
        return true; /* no control connection -- best-effort, not an error */

    char msg[GHOSTCON_PTYSERV_LINE_MAX];
    size_t msg_len = ghostcon_ptyserv_format_resize(msg, sizeof(msg), rows, cols);
    if (msg_len == 0)
        return false;
    bool ok = write_all(t->ctl_fd, msg, msg_len);
    return ok;
}

bool
ghostcon_transport_request_dump(ghostcon_transport_t *t)
{
    if (t->ctl_fd < 0)
        return true; /* no control connection -- best-effort, not an error */

    char msg[GHOSTCON_PTYSERV_LINE_MAX];
    size_t msg_len = ghostcon_ptyserv_format_dump(msg, sizeof(msg));
    if (msg_len == 0)
        return false;
    return write_all(t->ctl_fd, msg, msg_len);
}

bool
ghostcon_transport_read_ctl(ghostcon_transport_t *t, bool *out_clear)
{
    *out_clear = false;
    if (t->ctl_fd < 0)
        return false;

    char line[GHOSTCON_PTYSERV_LINE_MAX];
    ssize_t r = read(t->ctl_fd, line, sizeof(line) - 1);
    if (r <= 0) {
        close(t->ctl_fd);
        t->ctl_fd = -1;
        return true;
    }
    line[r] = '\0';
    *out_clear = ghostcon_ptyserv_parse_clear(line);
    return true;
}
