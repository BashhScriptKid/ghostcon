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
    int saved_kbd_mode; /* KDGKBMODE at open; -1 if not captured. See kbd_mode_blocks_vt_switch(). */
};

/* Console keyboard modes that disable the kernel's own console-switch keys.
 *
 * Ctrl+Alt+Fn is a KT_CONS key: in kbd_keycode() (drivers/tty/vt/ keyboard.c)
 * every key type except KT_SPEC/KT_SHIFT is dropped before its handler runs
 * whenever kbdmode is VC_RAW, VC_MEDIUMRAW, or VC_OFF
 *
 * so k_cons()/set_console() never executes, the kernel never initiates a switch,
 * and our VT_PROCESS handshake (SIGUSR2 -> VT_RELDISP) never fires.
 *
 * Found live: Ctrl+Alt+Fn was completely dead inside a ghostcon session with the
 * active tty2 reporting kb_mode=K_OFF.
 *
 * The mode is kernel state that survives a renderer restart, so taking over a VT
 * a previous owner (kmscon sets K_OFF and does not always restore it on an
 * unclean exit) left in one of these silently breaks VT switching for ghostcon's
 * entire lifetime
 *
 * ghostcon relies on the kernel's own switch handling rather than implementing it
 * itself (unlike kmscon's --no-switchvt), so it needs the switch key to keep
 * working.
 */
static bool kbd_mode_blocks_vt_switch(int mode)
{
    return mode == K_OFF || mode == K_RAW || mode == K_MEDIUMRAW;
}

/* Signal handlers can't take arbitrary context, and PLAN.md's architecture is one
 * process per VT, so one static instance is the whole story
 *
 * see the header's "only one per process" note.
 */
static ghostcon_vtctl_t *g_instance;

static void signal_handler(int signum)
{
    if (!g_instance)
        return;
    char c = (signum == SIGUSR1) ? 'A' : 'R';
    /* write() is async-signal-safe; errors here are unrecoverable from a signal
     * handler anyway (the self-pipe should never be full
     *
     * at most one byte per VT switch is ever pending).
     */
    ssize_t unused = write(g_instance->pipe_fds[1], &c, 1);
    (void)unused;
}

static bool currently_active(int fd, int vt_num, bool *out_active)
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
    if (g_instance)
    {
        fprintf(stderr, "vtctl: only one ghostcon_vtctl_t per process (VT %d already open)\n",
                g_instance->vt_num);
        return NULL;
    }

    ghostcon_vtctl_t *vt = calloc(1, sizeof(*vt));
    if (!vt)
        return NULL;
    vt->vt_num = vt_num;
    vt->pipe_fds[0] = vt->pipe_fds[1] = -1;
    vt->saved_kbd_mode = -1; /* calloc() would make this 0 == K_RAW, a real (if bogus) mode */

    char path[32];
    snprintf(path, sizeof(path), "/dev/tty%d", vt_num);
    vt->fd = open(path, O_RDWR | O_NOCTTY);
    if (vt->fd < 0)
    {
        fprintf(stderr, "vtctl: open %s: %s\n", path, strerror(errno));
        goto fail;
    }

    if (ioctl(vt->fd, KDSETMODE, KD_GRAPHICS) != 0)
    {
        fprintf(stderr, "vtctl: KDSETMODE(KD_GRAPHICS) on %s: %s\n", path, strerror(errno));
        goto fail_fd;
    }

    if (pipe(vt->pipe_fds) != 0)
    {
        fprintf(stderr, "vtctl: pipe: %s\n", strerror(errno));
        goto fail_graphics;
    }
    /* Non-blocking write end so a pathological burst of signals can never
     * block inside the handler; O_NONBLOCK read end so
     * ghostcon_vtctl_process_pending() can drain without blocking.
     */
    fcntl(vt->pipe_fds[0], F_SETFL, O_NONBLOCK);
    fcntl(vt->pipe_fds[1], F_SETFL, O_NONBLOCK);

    g_instance = vt; /* must be set before installing handlers */

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGUSR1, &sa, NULL) != 0 || sigaction(SIGUSR2, &sa, NULL) != 0)
    {
        fprintf(stderr, "vtctl: sigaction: %s\n", strerror(errno));
        g_instance = NULL;
        goto fail_pipe;
    }

    struct vt_mode vtm;
    memset(&vtm, 0, sizeof(vtm));
    vtm.mode = VT_PROCESS;
    vtm.acqsig = SIGUSR1;
    vtm.relsig = SIGUSR2;
    if (ioctl(vt->fd, VT_SETMODE, &vtm) != 0)
    {
        fprintf(stderr, "vtctl: VT_SETMODE(VT_PROCESS) on %s: %s\n", path, strerror(errno));
        g_instance = NULL;
        goto fail_pipe;
    }

    /* KDSETMODE(KD_GRAPHICS) helps the console but does NOT stop the kernel from
     * dropping the console-switch keys; a stale K_OFF/raw mode inherited from a
     * previous owner does, and that silently kills the very VT-switch keys this
     * handshake depends on (see kbd_mode_blocks_vt_switch()'s own comment).
     *
     * Override to a switching-capable mode when needed, remembering the old value
     * so a clean shutdown can put it back.
     *
     * Best-effort: a failed KDGKBMODE/ KDSKBMODE is logged, not fatal
     *
     * worst case VT switching stays as broken as it already was, never worse.
     *
     * Placed after VT_SETMODE so no earlier failure path has to unwind it.
     */
    int kbm = -1;
    if (ioctl(vt->fd, KDGKBMODE, &kbm) != 0)
    {
        fprintf(stderr, "vtctl: KDGKBMODE on %s: %s\n", path, strerror(errno));
    }
    else
    {
        vt->saved_kbd_mode = kbm;
        if (kbd_mode_blocks_vt_switch(kbm) &&
            ioctl(vt->fd, KDSKBMODE, K_UNICODE) != 0)
            fprintf(stderr, "vtctl: KDSKBMODE(K_UNICODE) on %s: %s\n", path, strerror(errno));
    }

    bool active = false;
    if (!currently_active(vt->fd, vt_num, &active))
    {
        fprintf(stderr, "vtctl: VT_GETSTATE on %s: %s\n", path, strerror(errno));
        /* Not fatal
         *
         * default to INACTIVE, the safe assumption (never claim DRM master we
         * weren't told we hold).
         */
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

void ghostcon_vtctl_close(ghostcon_vtctl_t *vt)
{
    if (!vt)
        return;

    struct vt_mode vtm;
    memset(&vtm, 0, sizeof(vtm));
    vtm.mode = VT_AUTO;
    ioctl(vt->fd, VT_SETMODE, &vtm);
    ioctl(vt->fd, KDSETMODE, KD_TEXT);

    /* Restore the keyboard mode only if it wasn't itself a switching-blocking
     * one.
     *
     * Putting K_OFF/K_RAW/K_MEDIUMRAW back would hand the VT to whatever runs
     * next (a getty, another renderer) in the exact state
     * kbd_mode_blocks_vt_switch() exists to repair, so a mode this module had to
     * override is deliberately left as the usable K_UNICODE instead of being
     * faithfully (and uselessly) restored.
     */
    if (vt->saved_kbd_mode >= 0 && !kbd_mode_blocks_vt_switch(vt->saved_kbd_mode))
        ioctl(vt->fd, KDSKBMODE, vt->saved_kbd_mode);

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

int ghostcon_vtctl_signal_fd(const ghostcon_vtctl_t *vt)
{
    return vt->pipe_fds[0];
}

int ghostcon_vtctl_transition(ghostcon_vt_state_t *state, char event,
                              ghostcon_vt_op_t *ops, int max)
{
    if (max < 2)
        return -1;
    if (event == 'R')
    {
        /* pre_ack('R', ...) must call drmDropMaster() before returning, so it is
         * emitted before the VT_RELDISP ack — see vtctl.h's op-enum comment for
         * the live hang this ordering fixes.
         */
        ops[0] = GC_VT_OP_PRE_ACK_RELEASE;
        ops[1] = GC_VT_OP_ACK_RELEASE;
        *state = GC_VT_STATE_INACTIVE;
        return 2;
    }
    if (event == 'A')
    {
        ops[0] = GC_VT_OP_PRE_ACK_ACQUIRE;
        ops[1] = GC_VT_OP_ACK_ACQUIRE;
        *state = GC_VT_STATE_ACTIVE;
        return 2;
    }
    return -1;
}

void ghostcon_vtctl_process_pending(ghostcon_vtctl_t *vt,
                                    ghostcon_vtctl_pre_ack_fn pre_ack,
                                    void *userdata)
{
    char buf[64];
    ssize_t n;

    /* Multiple signals may have coalesced in the pipe (e.g. a rapid
     * release-then-reacquire); only the final one reflects reality, but we still
     * ack every VT_RELDISP the kernel is waiting on
     *
     * draining and acking on the LAST byte's meaning is wrong if there were both
     * an 'R' and a later 'A' pending, since each needs its own specific ack call
     * (VT_RELDISP with different arguments).
     *
     * So process each byte as it's drained, not just the last one.
     */
    while ((n = read(vt->pipe_fds[0], buf, sizeof(buf))) > 0)
    {
        for (ssize_t i = 0; i < n; i++)
        {
            ghostcon_vt_op_t ops[2];
            int nops = ghostcon_vtctl_transition(&vt->state, buf[i], ops, 2);
            for (int j = 0; j < nops; j++)
            {
                switch (ops[j])
                {
                case GC_VT_OP_PRE_ACK_RELEASE:
                    if (pre_ack)
                        pre_ack('R', userdata);
                    break;
                case GC_VT_OP_ACK_RELEASE:
                    ioctl(vt->fd, VT_RELDISP, 1);
                    break;
                case GC_VT_OP_PRE_ACK_ACQUIRE:
                    if (pre_ack)
                        pre_ack('A', userdata);
                    break;
                case GC_VT_OP_ACK_ACQUIRE:
                    ioctl(vt->fd, VT_RELDISP, VT_ACKACQ);
                    /* Caller must now call drmSetMaster() — see header. */
                    break;
                }
            }
        }
    }
}
