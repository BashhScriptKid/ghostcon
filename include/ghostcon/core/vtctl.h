#pragma once

#include <stdbool.h>
#include <stdint.h>

/* VT lifecycle: the VT_PROCESS acquire/release handshake.
 *
 * Status: untested at runtime as of this writing (see PLAN.md's Phase 1 item 3
 * note). /dev/ttyN is root-only here, and exercising the signals requires a real
 * chvt away from and back to the desktop.
 *
 * That was deferred to a joint session with core/kms.c, since this module's job
 * is telling kms.c when to grab or drop DRM master.
 *
 * The sequence follows kmscon's pattern (src/uterm/vt_real.c):
 *
 * 1. Open /dev/ttyN with O_RDWR | O_NOCTTY.
 *
 * 2. ioctl(KDSETMODE, KD_GRAPHICS).
 *
 *    If the inherited keyboard mode would stop the kernel from handling
 *    Ctrl+Alt+Fn (K_OFF, K_RAW, K_MEDIUMRAW), first override it to
 *    K_UNICODE. Otherwise the kernel's switch handling, which this
 *    handshake relies on, never runs; see kbd_mode_blocks_vt_switch().
 *
 * 3. ioctl(VT_SETMODE, { VT_PROCESS, acqsig=SIGUSR1, relsig=SIGUSR2 }).
 * 4. On SIGUSR2 (release): the caller drops DRM master; this module then
 *    acks with ioctl(VT_RELDISP, 1).
 * 5. On SIGUSR1 (acquire): this module acks with
 *    ioctl(VT_RELDISP, VT_ACKACQ); the caller then takes DRM master.
 *
 * PLAN.md calls this the highest-risk module. A wedged event loop that never acks
 * VT_RELDISP reproduces kmscon's "VT switching does not work" failure mode, and
 * the kernel cannot force the ack. The answer is not a perfect handshake
 *
 * it is the supervisor canary detecting and recovering from that stall (a
 * separate module). This module's own responsibility is to never do dangerous
 * work inside the signal handler.
 *
 * ioctl() and drmSetMaster() are not async-signal-safe, so the handler only
 * writes a byte to a self-pipe.
 *
 * The real work happens in ghostcon_vtctl_process_pending(), which the main loop
 * calls after polling ghostcon_vtctl_signal_fd() readable.
 *
 * SIGUSR1 and SIGUSR2 are reserved for this handshake. No other ghostcon
 * component may install handlers for them.
 *
 * Only one instance may be open per process (one process per VT). Signal handlers
 * cannot carry instance context, so the module keeps a single static pointer and
 * ghostcon_vtctl_open() fails on a second call.
 */

typedef enum
{
    GC_VT_STATE_INACTIVE, /* released — do not touch DRM master */
    GC_VT_STATE_ACTIVE,   /* acquired — safe to hold DRM master */
} ghostcon_vt_state_t;

typedef struct ghostcon_vtctl ghostcon_vtctl_t;

/* Opens /dev/tty<vt_num> and installs the VT_PROCESS handshake.
 *
 * The VT starts INACTIVE if it's not currently the foreground VT (the normal case
 * at startup
 *
 * the supervisor/undead-head decides when to chvt), or ACTIVE if it already is.
 * Returns NULL on failure (see stderr).
 */
extern ghostcon_vtctl_t *ghostcon_vtctl_open(int vt_num);

/* Restores KD_TEXT + VT_AUTO (and the pre-existing keyboard mode, unless that
 * mode was a switching-blocking one
 *
 * see vtctl.c) and closes the tty fd.
 */
extern void ghostcon_vtctl_close(ghostcon_vtctl_t *vt);

extern ghostcon_vt_state_t ghostcon_vtctl_state(const ghostcon_vtctl_t *vt);

/* Self-pipe read fd
 *
 * poll() this for POLLIN in the main event loop. Readable means an acquire or
 * release signal arrived and ghostcon_vtctl_process_pending() needs to run.
 */
extern int ghostcon_vtctl_signal_fd(const ghostcon_vtctl_t *vt);

/* Called once per coalesced signal, immediately BEFORE that signal's ioctl ack
 *
 * not after processing the whole batch.
 *
 * This is what makes it possible to satisfy process_pending()'s
 * drmDropMaster()-before-VT_RELDISP contract correctly even when a release and a
 * later reacquire have coalesced into the same self-pipe drain (see
 * ghostcon_vtctl_process_pending()'s own doc comment): the caller drops master
 * here, synchronously, for the specific 'R' this callback fires for, before that
 * exact ack goes out
 *
 * not once, ahead of time, for the whole batch.
 *
 * `event` is 'R' (about to ack a release) or 'A' (about to ack an acquire).
 */
typedef void (*ghostcon_vtctl_pre_ack_fn)(char event, void *userdata);

/* Ordered operations a single drained signal byte implies.
 *
 * Exposed so a unit test can pin the order down without a real VT.
 *
 * The release path must list GC_VT_OP_PRE_ACK_RELEASE before
 * GC_VT_OP_ACK_RELEASE: that is what makes the caller's drmDropMaster() (in the
 * pre_ack callback) run before the VT_RELDISP ack reaches the kernel.
 *
 * Getting it backwards is the live hang documented on
 * ghostcon_vtctl_process_pending().
 */
typedef enum
{
    GC_VT_OP_PRE_ACK_RELEASE, /* pre_ack('R', ...) — caller drops DRM master */
    GC_VT_OP_ACK_RELEASE,     /* ioctl(VT_RELDISP, 1) */
    GC_VT_OP_PRE_ACK_ACQUIRE, /* pre_ack('A', ...) */
    GC_VT_OP_ACK_ACQUIRE,     /* ioctl(VT_RELDISP, VT_ACKACQ) */
} ghostcon_vt_op_t;

/* Pure transition for one drained byte: 'R' (release) or 'A' (acquire).
 *
 * Writes the ordered ops for `event` into ops[0..max), sets *state to the
 * resulting VT state, and returns the op count. An unrecognized byte emits
 * nothing, leaves *state unchanged, and returns -1.
 *
 * No ioctl, no callback, no /dev/ttyN — this is the piece of the handshake a
 * unit test can pin down; ghostcon_vtctl_process_pending() is the thin executor.
 */
extern int ghostcon_vtctl_transition(ghostcon_vt_state_t *state, char event,
                                     ghostcon_vt_op_t *ops, int max);

/* Drains the self-pipe and updates state, invoking `pre_ack` (if non-NULL)
 * immediately before each individual signal's ack.
 *
 * On release (GC_VT_STATE_ACTIVE -> INACTIVE): `pre_ack('R', ...)` must call
 * drmDropMaster() on the caller's DRM fd before returning, since the VT_RELDISP
 * ack that follows tells the kernel it's safe to hand the VT to whoever is
 * switching in
 *
 * holding master past that point races the next VT's mode-set.
 *
 * On acquire (INACTIVE -> ACTIVE): this function acks first
 * (VT_RELDISP/VT_ACKACQ), then the caller must call drmSetMaster() to reclaim it
 * (`pre_ack('A', ...)` fires before that ack too, but reclaiming master is still
 * done by the caller after process_pending() returns, matching the existing
 * acquire-side contract
 *
 * only the release side has the ordering hazard).
 */
extern void ghostcon_vtctl_process_pending(ghostcon_vtctl_t *vt,
                                           ghostcon_vtctl_pre_ack_fn pre_ack,
                                           void *userdata);
