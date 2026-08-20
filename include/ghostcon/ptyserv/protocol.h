#pragma once

#include <stddef.h>
#include <stdbool.h>

/* ------------------------------------------------------------------ */
/* ghost-ptyserv registry protocol                                     */
/*                                                                     */
/* Plain-text, newline-framed request/response over a Unix socket.    */
/* Deliberately not the strict-framed/SO_PEERCRED protocol ghostcon-ipc*/
/* uses (PLAN.md Phase 2) — this is a same-host coordination handshake,*/
/* not a cross-privilege broker.                                       */
/*                                                                     */
/* Renderer -> ghost-ptyserv:  "GET <vtnum>\n"                         */
/* ghost-ptyserv -> renderer:  "OK pid=<pid> socket=<path>\n"          */
/*                          or "ERR <reason>\n"                        */
/* ------------------------------------------------------------------ */

#define GHOSTCON_PTYSERV_SOCKET_PATH "/run/ghostcon/ptyserv.sock"
#define GHOSTCON_PTYSERV_LINE_MAX 256

/* pty-ttyN child sockets live alongside the registry socket, one per VT
   ("%s" is the registry socket's directory — ghost-ptyserv derives this
   from its own argv[1] rather than hardcoding /run/ghostcon, so it works
   both under a real /run/ghostcon (production) and an arbitrary temp
   directory (tests, no root required). */
#define GHOSTCON_PTY_CHILD_SOCKET_FMT "%s/pty-tty%d.sock"

/* Resize control socket, one per VT alongside the data socket above.
   Separate connection, not an in-band command on the data socket:
   pty_child.c's data socket is a raw, binary-transparent passthrough
   (any byte sequence, including one chosen as an escape marker, can
   legitimately appear in real terminal traffic), and a same-socket
   "reconnect to signal a resize" trick was considered and rejected --
   pty_child.c's accept() loop treats any new connection on the DATA
   socket as "the renderer reconnected," immediately replacing (closing)
   whatever was already connected. A dedicated socket avoids both
   problems: plain-text framing is safe here since nothing but resize
   messages ever crosses it. */
#define GHOSTCON_PTY_CTL_SOCKET_FMT "%s/pty-tty%d.ctl.sock"

/* Fixed-size overwrite-oldest ring buffer of PTY output, replayed in
   full to every newly connected renderer before live forwarding resumes.
   Not specified in PLAN.md; this default is the ghost-ptyserv port's own
   choice, documented here since nothing else pins it down. */
#define GHOSTCON_PTY_RINGBUF_DEFAULT_SIZE (1u << 20) /* 1 MiB */

/* ------------------------------------------------------------------ */
/* Registry request/response parsing helpers                          */
/* ------------------------------------------------------------------ */

/* Formats "GET <vtnum>\n" into buf (size buf_len). Returns written
   length, or 0 if it wouldn't fit. */
size_t ghostcon_ptyserv_format_get(char *buf, size_t buf_len, int vtnum);

/* Parses a "GET <vtnum>\n" request line. Returns true and sets *vtnum
   on success. */
bool ghostcon_ptyserv_parse_get(const char *line, int *vtnum);

/* Formats "OK pid=<pid> socket=<path>\n" into buf. Returns written
   length, or 0 if it wouldn't fit. */
size_t ghostcon_ptyserv_format_ok(char *buf, size_t buf_len,
                                   int pid, const char *socket_path);

/* Parses an "OK pid=<pid> socket=<path>\n" response line. socket_path
   must be at least GHOSTCON_PTYSERV_LINE_MAX bytes. Returns true on
   success. */
bool ghostcon_ptyserv_parse_ok(const char *line, int *pid, char *socket_path);

/* Formats "RESIZE <rows> <cols> <xpixel> <ypixel>\n" into buf. xpixel/
   ypixel are the terminal's TOTAL pixel width/height (cell_w*cols,
   cell_h*rows), matching struct winsize's own ws_xpixel/ws_ypixel
   convention -- pass 0/0 if unknown, which pty_child.c treats as "no
   pixel geometry to report" rather than guessing. Returns written
   length, or 0 if it wouldn't fit. Sent over the control socket
   (GHOSTCON_PTY_CTL_SOCKET_FMT), never the data socket. */
size_t ghostcon_ptyserv_format_resize(char *buf, size_t buf_len, int rows, int cols,
                                      int xpixel, int ypixel);

/* Parses a "RESIZE <rows> <cols> <xpixel> <ypixel>\n" line. Returns
   true and sets `*rows`/`*cols`/`*xpixel`/`*ypixel` on success --
   xpixel/ypixel come back as 0 if the line only has the older
   2-field rows/cols form (kept parseable rather than rejected, since
   nothing else on this internal control socket needs strict
   versioning). */
bool ghostcon_ptyserv_parse_resize(const char *line, int *rows, int *cols,
                                   int *xpixel, int *ypixel);

/* Formats "CLEAR\n" into buf. Returns written length, or 0 if it
   wouldn't fit. Sent over the control socket (GHOSTCON_PTY_CTL_SOCKET_FMT)
   by pty_child.c the moment its session dies (before respawning),
   telling ghostcon-core the screen's previous owner is gone so it can
   optionally wipe the display + scrollback rather than leaving the old
   session's last screenful visible under the new login prompt. */
size_t ghostcon_ptyserv_format_clear(char *buf, size_t buf_len);

/* Parses a "CLEAR\n" line. Returns true if `line` is a CLEAR message. */
bool ghostcon_ptyserv_parse_clear(const char *line);

/* Formats "DUMP\n" into buf. Returns written length, or 0 if it
   wouldn't fit. Sent over the control socket by core/main.c when
   Ctrl+Alt+D is pressed (see ghostcon_screen_dump's own doc comment
   in term/dump.h for the companion live screen-state dump this pairs
   with) -- pty_child.c responds by replaying its ring buffer (the
   real, raw bytes leading up to the moment of the dump, not a
   synthetic reproduction) to a fixed diagnostic file. */
size_t ghostcon_ptyserv_format_dump(char *buf, size_t buf_len);

/* Parses a "DUMP\n" line. Returns true if `line` is a DUMP message. */
bool ghostcon_ptyserv_parse_dump(const char *line);
