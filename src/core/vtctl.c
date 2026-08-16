/* See include/ghostcon/core/vtctl.h for the "untested at runtime" note. */

#define _DEFAULT_SOURCE /* O_NOCTTY under -std=c11 */

#include "ghostcon/core/vtctl.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/kd.h>
#include <linux/vt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

struct ghostcon_vtctl {
    int fd;
    int vt_num;
    ghostcon_vt_state_t state;
    int pipe_fds[2]; /* [0]=read (public, poll()'d), [1]=write (signal handler only) */
};

/* Signal handlers can't take arbitrary context, and PLAN.md's
   architecture is one process per VT, so one static instance is the
   whole story — see the header's "only one per process" note. */
static ghostcon_vtctl_t *g_instance;

static void
signal_handler(int signum)
{
    if (!g_instance)
        return;
    char c = (signum == SIGUSR1) ? 'A' : 'R';
    /* write() is async-signal-safe; errors here are unrecoverable from
       a signal handler anyway (the self-pipe should never be full —
       at most one byte per VT switch is ever pending). */
    ssize_t unused = write(g_instance->pipe_fds[1], &c, 1);
    (void)unused;
}

static bool
currently_active(int fd, int vt_num, bool *out_active)
{
    struct vt_stat st;
    if (ioctl(fd, VT_GETSTATE, &st) != 0)
        return false;
    *out_active = (st.v_active == vt_num);
    return true;
}

ghostcon_vtctl_t *
ghostcon_vtctl_open(int vt_num)
{
    if (g_instance) {
        fprintf(stderr, "vtctl: only one ghostcon_vtctl_t per process (VT %d already open)\n",
                g_instance->vt_num);
        return NULL;
    }

    ghostcon_vtctl_t *vt = calloc(1, sizeof(*vt));
    if (!vt)
        return NULL;
    vt->vt_num = vt_num;
    vt->pipe_fds[0] = vt->pipe_fds[1] = -1;

    char path[32];
    snprintf(path, sizeof(path), "/dev/tty%d", vt_num);
    vt->fd = open(path, O_RDWR | O_NOCTTY);
    if (vt->fd < 0) {
        fprintf(stderr, "vtctl: open %s: %s\n", path, strerror(errno));
        goto fail;
    }

    if (ioctl(vt->fd, KDSETMODE, KD_GRAPHICS) != 0) {
        fprintf(stderr, "vtctl: KDSETMODE(KD_GRAPHICS) on %s: %s\n", path, strerror(errno));
        goto fail_fd;
    }

    if (pipe(vt->pipe_fds) != 0) {
        fprintf(stderr, "vtctl: pipe: %s\n", strerror(errno));
        goto fail_graphics;
    }
    /* Non-blocking write end so a pathological burst of signals can
       never block inside the handler; O_NONBLOCK read end so
       ghostcon_vtctl_process_pending() can drain without blocking. */
    fcntl(vt->pipe_fds[0], F_SETFL, O_NONBLOCK);
    fcntl(vt->pipe_fds[1], F_SETFL, O_NONBLOCK);

    g_instance = vt; /* must be set before installing handlers */

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGUSR1, &sa, NULL) != 0 || sigaction(SIGUSR2, &sa, NULL) != 0) {
        fprintf(stderr, "vtctl: sigaction: %s\n", strerror(errno));
        g_instance = NULL;
        goto fail_pipe;
    }

    struct vt_mode vtm;
    memset(&vtm, 0, sizeof(vtm));
    vtm.mode = VT_PROCESS;
    vtm.acqsig = SIGUSR1;
    vtm.relsig = SIGUSR2;
    if (ioctl(vt->fd, VT_SETMODE, &vtm) != 0) {
        fprintf(stderr, "vtctl: VT_SETMODE(VT_PROCESS) on %s: %s\n", path, strerror(errno));
        g_instance = NULL;
        goto fail_pipe;
    }

    bool active = false;
    if (!currently_active(vt->fd, vt_num, &active)) {
        fprintf(stderr, "vtctl: VT_GETSTATE on %s: %s\n", path, strerror(errno));
        /* Not fatal — default to INACTIVE, the safe assumption (never
           claim DRM master we weren't told we hold). */
    }
    vt->state = active ? GC_VT_STATE_ACTIVE : GC_VT_STATE_INACTIVE;

    return vt;

fail_pipe:
    close(vt->pipe_fds[0]);
    close(vt->pipe_fds[1]);
fail_graphics:
    ioctl(vt->fd, KDSETMODE, KD_TEXT);
fail_fd:
    close(vt->fd);
fail:
    free(vt);
    return NULL;
}

void
ghostcon_vtctl_close(ghostcon_vtctl_t *vt)
{
    if (!vt)
        return;

    struct vt_mode vtm;
    memset(&vtm, 0, sizeof(vtm));
    vtm.mode = VT_AUTO;
    ioctl(vt->fd, VT_SETMODE, &vtm);
    ioctl(vt->fd, KDSETMODE, KD_TEXT);

    signal(SIGUSR1, SIG_DFL);
    signal(SIGUSR2, SIG_DFL);
    if (g_instance == vt)
        g_instance = NULL;

    if (vt->pipe_fds[0] >= 0)
        close(vt->pipe_fds[0]);
    if (vt->pipe_fds[1] >= 0)
        close(vt->pipe_fds[1]);
    close(vt->fd);
    free(vt);
}

ghostcon_vt_state_t
ghostcon_vtctl_state(const ghostcon_vtctl_t *vt)
{
    return vt->state;
}

int
ghostcon_vtctl_signal_fd(const ghostcon_vtctl_t *vt)
{
    return vt->pipe_fds[0];
}

void
ghostcon_vtctl_process_pending(ghostcon_vtctl_t *vt,
                               ghostcon_vtctl_pre_ack_fn pre_ack,
                               void *userdata)
{
    char buf[64];
    ssize_t n;
    char last = 0;

    /* Multiple signals may have coalesced in the pipe (e.g. a rapid
       release-then-reacquire); only the final one reflects reality, but
       we still ack every VT_RELDISP the kernel is waiting on — draining
       and acking on the LAST byte's meaning is wrong if there were both
       an 'R' and a later 'A' pending, since each needs its own specific
       ack call (VT_RELDISP with different arguments). So process each
       byte as it's drained, not just the last one. */
    while ((n = read(vt->pipe_fds[0], buf, sizeof(buf))) > 0) {
        for (ssize_t i = 0; i < n; i++) {
            last = buf[i];
            if (last == 'R') {
                /* pre_ack('R', ...) must call drmDropMaster() before
                   returning — see this function's own header doc
                   comment for why the ordering matters here specifically
                   (found live: paging back to the desktop hung on a
                   black screen until forced through an unrelated VT,
                   because this ack used to go out before master was
                   actually dropped). */
                if (pre_ack)
                    pre_ack('R', userdata);
                ioctl(vt->fd, VT_RELDISP, 1);
                vt->state = GC_VT_STATE_INACTIVE;
            } else if (last == 'A') {
                if (pre_ack)
                    pre_ack('A', userdata);
                ioctl(vt->fd, VT_RELDISP, VT_ACKACQ);
                vt->state = GC_VT_STATE_ACTIVE;
                /* Caller must now call drmSetMaster() — see header. */
            }
        }
    }
    (void)last;
}
