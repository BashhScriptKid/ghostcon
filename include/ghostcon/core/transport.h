#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* ------------------------------------------------------------------ */
/* pty child socket transport                                          */
/*                                                                     */
/* ghostcon-core's replacement for a local pty.c (PLAN.md: "pty.c —    */
/* Moved to pty-ttyN child processes under ghost-ptyserv"). Queries    */
/* ghost-ptyserv's registry for the pty child socket serving a given   */
/* VT (see ptyserv/protocol.h), connects to it directly, and from then */
/* on it's a raw bidirectional byte pipe — the pty child is "byte-     */
/* stupid" by design, no framing on this side either.                  */
/* ------------------------------------------------------------------ */

typedef struct {
    int fd; /* connected to the pty child's socket, or -1 if unconnected */
    int ctl_fd; /* connected to the pty child's resize control socket
                    (see ptyserv/protocol.h's GHOSTCON_PTY_CTL_SOCKET_FMT),
                    or -1 if unconnected/unavailable -- not fatal if this
                    one fails, resizes just silently won't propagate */
    int pty_child_pid; /* from the registry response, for callers that
                           need it (e.g. test cleanup) -- ghostcon-core
                           itself has no reason to touch pty-ttyN's
                           lifecycle directly, that's ghost-ptyserv's job */
} ghostcon_transport_t;

/* Connects to registry_socket_path (typically
   GHOSTCON_PTYSERV_SOCKET_PATH), asks for vtnum's pty child, then
   connects to that child's socket. On success, the pty child
   immediately starts streaming its ring buffer replay followed by live
   output — the first ghostcon_transport_read() calls will see that. */
bool ghostcon_transport_connect(ghostcon_transport_t *t,
                                 const char *registry_socket_path,
                                 int vtnum);

void ghostcon_transport_close(ghostcon_transport_t *t);

/* fd suitable for poll()'ing in the main event loop. */
int ghostcon_transport_fd(const ghostcon_transport_t *t);

/* Thin wrappers over read()/write() on the transport fd — no framing,
   no retry-on-partial-write loop here, since the caller's event loop
   already needs its own poll()-driven read/write handling regardless
   (matching core/ptyserv's own pty_child.c pattern). Returns the same
   semantics as read(2)/write(2): byte count, 0 on EOF (pty child
   process exited), -1 on error (errno set). */
ssize_t ghostcon_transport_read(ghostcon_transport_t *t, uint8_t *buf, size_t len);
ssize_t ghostcon_transport_write(ghostcon_transport_t *t, const uint8_t *buf, size_t len);

/* Sends a resize notification over the control socket connected
   alongside the main data socket in ghostcon_transport_connect(). A
   no-op (returns true) if the control connection isn't available --
   see ctl_fd's own doc comment on why that's not fatal. */
bool ghostcon_transport_resize(ghostcon_transport_t *t, int rows, int cols);

/* Reads and parses one message off the control socket (see ctl_fd's own
   doc comment) — call this when ctl_fd is POLLIN/POLLHUP/POLLERR-ready
   in the caller's event loop. Sets *out_clear = true if a CLEAR message
   (pty_child.c's session-death notification) was received. Closes and
   invalidates ctl_fd on EOF/error (subsequent calls become no-ops,
   matching ctl_fd's existing "not fatal if unavailable" contract).
   Returns false only if ctl_fd was already < 0 when called. */
bool ghostcon_transport_read_ctl(ghostcon_transport_t *t, bool *out_clear);
