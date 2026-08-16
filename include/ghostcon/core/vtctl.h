#pragma once

#include <stdbool.h>
#include <stdint.h>

/* ------------------------------------------------------------------ */
/* VT lifecycle — VT_PROCESS acquire/release handshake                 */
/*                                                                     */
/* UNTESTED AT RUNTIME as of this writing (see PLAN.md's Phase 1 item  */
/* 3 note): /dev/ttyN is root-only on this machine, and exercising the */
/* acquire/release signals for real requires an actual chvt away from  */
/* and back to the live desktop session — both deferred to a joint     */
/* session with core/kms.c, since this module's whole job is telling   */
/* kms.c when to grab/drop DRM master.                                 */
/*                                                                     */
/* Follows kmscon's proven kernel-level pattern (reference: its        */
/* src/uterm/vt_real.c) — PLAN.md's own reading-guide pointer:          */
/*   1. Open /dev/ttyN with O_RDWR | O_NOCTTY.                         */
/*   2. ioctl(KDSETMODE, KD_GRAPHICS).                                 */
/*   3. ioctl(VT_SETMODE, { VT_PROCESS, acqsig=SIGUSR1, relsig=SIGUSR2 }).*/
/*   4. On SIGUSR2 (release): caller must drmDropMaster(), then this   */
/*      module acks via ioctl(VT_RELDISP, 1).                          */
/*   5. On SIGUSR1 (acquire): this module acks via                     */
/*      ioctl(VT_RELDISP, VT_ACKACQ), then caller must drmSetMaster(). */
/*                                                                     */
/* PLAN.md flags this as the highest-risk module: a wedged event loop  */
/* that never acks VT_RELDISP is exactly kmscon's "switching doesn't   */
/* work" failure mode, and the kernel has no way to force it. The fix  */
/* isn't "make the handshake code perfect" — it's "make sure the       */
/* supervisor's canary can detect and recover from this specific stall"*/
/* (PLAN.md's supervision layer, a separate module). This module's own */
/* job is just: never do unsafe work in the signal handler itself.     */
/* Signal handlers can't safely call ioctl()/drmSetMaster() (not       */
/* async-signal-safe), so the handler only writes a byte to a self-pipe*/
/* — the actual ioctl/drmSetMaster/drmDropMaster work happens in       */
/* ghostcon_vtctl_process_pending(), called from the main loop after   */
/* polling ghostcon_vtctl_signal_fd() readable.                        */
/*                                                                     */
/* SIGUSR1/SIGUSR2 are reserved for this handshake — no other ghostcon */
/* component should install handlers for them. Only one                */
/* ghostcon_vtctl_t may be open per process (one process = one VT,     */
/* per PLAN.md's architecture; signal handlers have no way to carry    */
/* per-instance context, so this module keeps one static instance      */
/* pointer internally and ghostcon_vtctl_open() fails if called twice).*/
/* ------------------------------------------------------------------ */

typedef enum {
    GC_VT_STATE_INACTIVE, /* released — do not touch DRM master */
    GC_VT_STATE_ACTIVE,   /* acquired — safe to hold DRM master */
} ghostcon_vt_state_t;

typedef struct ghostcon_vtctl ghostcon_vtctl_t;

/* Opens /dev/tty<vt_num> and installs the VT_PROCESS handshake. The VT
   starts INACTIVE if it's not currently the foreground VT (the normal
   case at startup — the supervisor/undead-head decides when to chvt),
   or ACTIVE if it already is. Returns NULL on failure (see stderr). */
ghostcon_vtctl_t *ghostcon_vtctl_open(int vt_num);

/* Restores KD_TEXT + VT_AUTO and closes the tty fd. */
void ghostcon_vtctl_close(ghostcon_vtctl_t *vt);

ghostcon_vt_state_t ghostcon_vtctl_state(const ghostcon_vtctl_t *vt);

/* Self-pipe read fd — poll() this for POLLIN in the main event loop.
   Readable means an acquire or release signal arrived and
   ghostcon_vtctl_process_pending() needs to run. */
int ghostcon_vtctl_signal_fd(const ghostcon_vtctl_t *vt);

/* Called once per coalesced signal, immediately BEFORE that signal's
   ioctl ack — not after processing the whole batch. This is what makes
   it possible to satisfy process_pending()'s drmDropMaster()-before-
   VT_RELDISP contract correctly even when a release and a later
   reacquire have coalesced into the same self-pipe drain (see
   ghostcon_vtctl_process_pending()'s own doc comment): the caller drops
   master here, synchronously, for the specific 'R' this callback fires
   for, before that exact ack goes out — not once, ahead of time, for
   the whole batch. `event` is 'R' (about to ack a release) or 'A'
   (about to ack an acquire). */
typedef void (*ghostcon_vtctl_pre_ack_fn)(char event, void *userdata);

/* Drains the self-pipe and updates state, invoking `pre_ack` (if
   non-NULL) immediately before each individual signal's ack. On
   release (GC_VT_STATE_ACTIVE -> INACTIVE): `pre_ack('R', ...)` must
   call drmDropMaster() on the caller's DRM fd before returning, since
   the VT_RELDISP ack that follows tells the kernel it's safe to hand
   the VT to whoever is switching in — holding master past that point
   races the next VT's mode-set. On acquire (INACTIVE -> ACTIVE): this
   function acks first (VT_RELDISP/VT_ACKACQ), then the caller must
   call drmSetMaster() to reclaim it (`pre_ack('A', ...)` fires before
   that ack too, but reclaiming master is still done by the caller
   after process_pending() returns, matching the existing acquire-side
   contract — only the release side has the ordering hazard). */
void ghostcon_vtctl_process_pending(ghostcon_vtctl_t *vt,
                                    ghostcon_vtctl_pre_ack_fn pre_ack,
                                    void *userdata);
