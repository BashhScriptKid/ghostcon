/*
 * ghost-ptyserv — registry + pty child spawner.
 *
 * Spawns one pty-ttyN child per configured VT, tracks child PID/socket
 * path, and answers renderer registry queries ("GET <vtnum>" ->
 * "OK pid=<pid> socket=<path>"). Never touches PTY data itself — pure
 * coordinator. See PLAN.md "Architecture overview" / "ghost-ptyserv".
 *
 * Phase 1 scope: no TOML config parsing yet (that's a separate, later
 * item per PLAN.md's own Phase 1 step list) — VT list and registry
 * socket path come from argv so this is independently testable now.
 *
 * Usage: ghost-ptyserv <registry_socket_path> <vtnum> [vtnum...]
 */

#define _DEFAULT_SOURCE

#include "ghostcon/ptyserv/protocol.h"

#include <errno.h>
#include <libgen.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_VTS 64

typedef struct {
    int vtnum;
    pid_t pid;
    char socket_path[GHOSTCON_PTYSERV_LINE_MAX];
} vt_entry_t;

static vt_entry_t g_vts[MAX_VTS];
static int g_nvts;
static const char *g_pty_child_path = "pty-ttyN"; /* overridden via env or resolved below */
static char g_pty_child_path_buf[4096];
static char g_pty_child_path_canonical[4096]; /* g_pty_child_path, realpath()-resolved --
    /proc/<pid>/exe is always absolute and symlink-resolved, so the
    pidfile-liveness cross-check (read_live_pid_file) needs this form
    regardless of whether g_pty_child_path came from resolve_sibling_path
    (already absolute-ish, but not guaranteed symlink-free) or
    GHOSTCON_PTY_CHILD_BIN (often a relative test-harness path). */
static char g_registry_dir[GHOSTCON_PTYSERV_LINE_MAX];

/* Resolves `name` relative to this executable's own directory (via
   /proc/self/exe) instead of trusting PATH -- this normally runs under
   pkexec, which resets the environment (including PATH) for security,
   so a bare name silently fails to exec. Same fix as
   undead_head/main.c and supervisor/main.c; found the same way, one
   binary later (pty-ttyN wasn't covered by that earlier fix). Falls
   back to the bare name only if /proc/self/exe itself is unavailable. */
static void
resolve_sibling_path(char *out, size_t out_len, const char *name)
{
    char self_path[4096];
    ssize_t n = readlink("/proc/self/exe", self_path, sizeof(self_path) - 1);
    if (n <= 0) {
        snprintf(out, out_len, "%s", name);
        return;
    }
    self_path[n] = '\0';

    char *slash = strrchr(self_path, '/');
    if (!slash) {
        snprintf(out, out_len, "%s", name);
        return;
    }
    *slash = '\0';
    snprintf(out, out_len, "%s/%s", self_path, name);
}

/* Sidecar pidfile path for a VT's pty-ttyN, alongside its socket
   (<dir>/pty-tty<N>.pid next to <dir>/pty-tty<N>.sock). Lets a
   restarted ghost-ptyserv recognize an already-running, still-live
   pty-ttyN (see spawn_pty_child()'s reuse path below) without needing
   to touch its renderer-facing socket at all -- connecting to that
   socket just to test liveness was considered and rejected: pty_child.c
   treats any new connection as "the renderer reconnected" and replaces
   (closes) whatever was already connected, so a bare liveness probe
   would kick out a real, working renderer connection. */
static void
pid_file_path(char *out, size_t out_len, int vtnum)
{
    snprintf(out, out_len, "%s/pty-tty%d.pid", g_registry_dir, vtnum);
}

/* Returns the still-alive pid recorded in `path`, or -1 if the file is
   missing, unreadable, names a dead pid, or names a pid that's no
   longer actually pty-ttyN (the /proc/<pid>/exe cross-check closes the
   pid-reuse edge case: a long-dead entry's pid could have since been
   recycled by an unrelated process). */
static pid_t
read_live_pid_file(const char *path)
{
    FILE *fp = fopen(path, "r");
    if (!fp)
        return -1;
    long pid_val = 0;
    int ok = fscanf(fp, "%ld", &pid_val);
    fclose(fp);
    if (ok != 1 || pid_val <= 0)
        return -1;

    pid_t pid = (pid_t)pid_val;
    if (kill(pid, 0) != 0)
        return -1; /* dead, or not ours to signal */

    if (g_pty_child_path_canonical[0] == '\0')
        return -1; /* couldn't resolve our own target at startup -- don't trust anything */

    char exe_link[64];
    snprintf(exe_link, sizeof(exe_link), "/proc/%ld/exe", pid_val);
    char exe_target[4096];
    ssize_t n = readlink(exe_link, exe_target, sizeof(exe_target) - 1);
    if (n <= 0)
        return -1;
    exe_target[n] = '\0';
    if (strcmp(exe_target, g_pty_child_path_canonical) != 0)
        return -1; /* alive, but not pty-ttyN -- pid was recycled */

    return pid;
}

/* Writes `pid` to `path` via a temp-file-then-rename, so a reader never
   observes a partially-written pid. */
static void
write_pid_file(const char *path, pid_t pid)
{
    char tmp_path[GHOSTCON_PTYSERV_LINE_MAX + 40]; /* headroom over the
        largest `path` a caller passes (see spawn_pty_child's pid_path) */
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);

    FILE *fp = fopen(tmp_path, "w");
    if (!fp)
        return;
    fprintf(fp, "%ld\n", (long)pid);
    fclose(fp);
    rename(tmp_path, path);
}

static vt_entry_t *
find_vt(int vtnum)
{
    for (int i = 0; i < g_nvts; i++) {
        if (g_vts[i].vtnum == vtnum)
            return &g_vts[i];
    }
    return NULL;
}

static bool
spawn_pty_child(int vtnum)
{
    vt_entry_t *e = &g_vts[g_nvts];
    e->vtnum = vtnum;
    snprintf(e->socket_path, sizeof(e->socket_path),
             GHOSTCON_PTY_CHILD_SOCKET_FMT, g_registry_dir, vtnum);

    char pid_path[GHOSTCON_PTYSERV_LINE_MAX + 32]; /* +32: headroom over
        GHOSTCON_PTYSERV_LINE_MAX for "/pty-tty<N>.pid" so the compiler's
        (theoretical worst-case-int) truncation warning doesn't fire */
    pid_file_path(pid_path, sizeof(pid_path), vtnum);

    /* If a pty-ttyN for this VT is already alive (this ghost-ptyserv
       instance itself was restarted -- e.g. after a crash -- while a
       previous one's children kept running independently: see
       pty_child.c's own setpgid(0,0)), reuse it instead of spawning a
       duplicate. A duplicate would orphan the real, possibly-mid-use
       shell session behind a socket the registry no longer points to. */
    pid_t existing = read_live_pid_file(pid_path);
    if (existing > 0) {
        e->pid = existing;
        g_nvts++;
        return true;
    }
    unlink(pid_path); /* stale (dead pid, or pid recycled) -- clear it before spawning fresh */

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return false;
    }
    if (pid == 0) {
        char vtnum_str[16];
        snprintf(vtnum_str, sizeof(vtnum_str), "%d", vtnum);
        execlp(g_pty_child_path, g_pty_child_path, vtnum_str, e->socket_path,
               (char *)NULL);
        perror("execlp pty-ttyN");
        _exit(127);
    }

    write_pid_file(pid_path, pid);
    e->pid = pid;
    g_nvts++;
    return true;
}

static int
listen_unix(const char *path)
{
    unlink(path);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    if (listen(fd, 16) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void
handle_client(int client_fd)
{
    char line[GHOSTCON_PTYSERV_LINE_MAX];
    ssize_t r = read(client_fd, line, sizeof(line) - 1);
    if (r <= 0) {
        close(client_fd);
        return;
    }
    line[r] = '\0';

    int vtnum;
    char reply[GHOSTCON_PTYSERV_LINE_MAX];
    size_t reply_len;

    if (!ghostcon_ptyserv_parse_get(line, &vtnum)) {
        reply_len = (size_t)snprintf(reply, sizeof(reply), "ERR bad request\n");
    } else {
        vt_entry_t *e = find_vt(vtnum);
        if (!e) {
            reply_len = (size_t)snprintf(reply, sizeof(reply),
                                          "ERR unknown vt %d\n", vtnum);
        } else {
            reply_len = ghostcon_ptyserv_format_ok(reply, sizeof(reply),
                                                    (int)e->pid, e->socket_path);
        }
    }

    if (reply_len > 0)
        write(client_fd, reply, reply_len);
    close(client_fd);
}

int
main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <registry_socket_path> <vtnum> [vtnum...]\n",
                argv[0]);
        return 2;
    }

    const char *registry_socket = argv[1];

    char dir_buf[GHOSTCON_PTYSERV_LINE_MAX];
    strncpy(dir_buf, registry_socket, sizeof(dir_buf) - 1);
    dir_buf[sizeof(dir_buf) - 1] = '\0';
    strncpy(g_registry_dir, dirname(dir_buf), sizeof(g_registry_dir) - 1);

    const char *env_pty_child = getenv("GHOSTCON_PTY_CHILD_BIN");
    if (env_pty_child && *env_pty_child) {
        g_pty_child_path = env_pty_child;
    } else {
        resolve_sibling_path(g_pty_child_path_buf, sizeof(g_pty_child_path_buf), "pty-ttyN");
        g_pty_child_path = g_pty_child_path_buf;
    }
    if (!realpath(g_pty_child_path, g_pty_child_path_canonical))
        g_pty_child_path_canonical[0] = '\0'; /* pidfile reuse just won't
            trust any pidfile if this fails -- falls back to always
            spawning fresh, same as today's behavior */

    /* Best-effort: the registry socket's directory (typically
       /run/ghostcon, whose real deployment would get this from a
       systemd tmpfiles.d rule -- no such infra here yet) may not exist.
       bind() below fails with ENOENT if it doesn't. */
    mkdir(g_registry_dir, 0755);

    signal(SIGPIPE, SIG_IGN);

    for (int i = 2; i < argc && g_nvts < MAX_VTS; i++) {
        int vtnum = atoi(argv[i]);
        if (!spawn_pty_child(vtnum)) {
            fprintf(stderr, "ghost-ptyserv: failed to spawn pty child for vt %d\n",
                    vtnum);
            return 1;
        }
    }

    int listen_fd = listen_unix(registry_socket);
    if (listen_fd < 0) {
        fprintf(stderr, "ghost-ptyserv: failed to listen on %s: %s\n",
                registry_socket, strerror(errno));
        return 1;
    }

    for (;;) {
        int client_fd = accept(listen_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        handle_client(client_fd);
    }

    close(listen_fd);
    unlink(registry_socket);
    return 0;
}
