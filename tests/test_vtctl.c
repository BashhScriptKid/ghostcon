/* test_vtctl
 *
 * unit tests for core/vtctl.c's pure VT_PROCESS transition step
 * (ghostcon_vtctl_transition), with no /dev/ttyN, DRM, or signal dependency.
 *
 * See vtctl.h's op-enum comment for why the release ordering this pins down
 * (pre_ack before the VT_RELDISP ack) is the highest-risk part of the handshake:
 * it is what lets the caller's drmDropMaster() run before the kernel is told the
 * VT is safe to hand off.
 */

#include "ghostcon/core/vtctl.h"

#include <stdio.h>

static int failures = 0;

static void check_transition(char event, ghostcon_vt_state_t start, const ghostcon_vt_op_t *expected, int expected_n, ghostcon_vt_state_t expected_state, const char *name)
{
    ghostcon_vt_op_t ops[4];
    ghostcon_vt_state_t state = start;
    int n = ghostcon_vtctl_transition(&state, event, ops, 4);
    int ok = (n == expected_n && state == expected_state);
    for (int i = 0; ok && i < n; i++)
        if (ops[i] != expected[i])
            ok = 0;
    if (!ok)
    {
        fprintf(stderr, "FAIL: %s: got %d ops, state %d (want %d ops, state %d)\n",
                name, n, (int)state, expected_n, (int)expected_state);
        failures++;
        return;
    }
    printf("PASS: %s\n", name);
}

/* Mirrors ghostcon_vtctl_process_pending()'s per-byte loop without the syscalls,
 * so a coalesced sequence can be checked as a whole (the code deliberately
 * processes every drained byte, not only the last).
 */
static int drain(const char *bytes, ghostcon_vt_state_t start, ghostcon_vt_op_t *trace, int max, ghostcon_vt_state_t *end)
{
    ghostcon_vt_state_t state = start;
    int t = 0;
    for (const char *p = bytes; *p; p++)
    {
        ghostcon_vt_op_t ops[2];
        int n = ghostcon_vtctl_transition(&state, *p, ops, 2);
        for (int i = 0; i < n && t < max; i++)
            trace[t++] = ops[i];
    }
    *end = state;
    return t;
}

static void check_sequence(const char *bytes, const ghostcon_vt_op_t *expected, int expected_n, ghostcon_vt_state_t expected_state, const char *name)
{
    ghostcon_vt_op_t trace[8];
    ghostcon_vt_state_t end;
    int n = drain(bytes, GC_VT_STATE_ACTIVE, trace, 8, &end);
    int ok = (n == expected_n && end == expected_state);
    for (int i = 0; ok && i < n; i++)
        if (trace[i] != expected[i])
            ok = 0;
    if (!ok)
    {
        fprintf(stderr, "FAIL: %s: got %d ops, end state %d\n", name, n, (int)end);
        failures++;
        return;
    }
    printf("PASS: %s\n", name);
}

int main(void)
{
    static const ghostcon_vt_op_t rel[] = {
        GC_VT_OP_PRE_ACK_RELEASE, GC_VT_OP_ACK_RELEASE,
    };
    static const ghostcon_vt_op_t acq[] = {
        GC_VT_OP_PRE_ACK_ACQUIRE, GC_VT_OP_ACK_ACQUIRE,
    };

    check_transition('R', GC_VT_STATE_ACTIVE, rel, 2, GC_VT_STATE_INACTIVE,
                     "release: drop master before the VT_RELDISP ack");
    check_transition('A', GC_VT_STATE_INACTIVE, acq, 2, GC_VT_STATE_ACTIVE,
                     "acquire: pre_ack then VT_ACKACQ");

    /* The resulting state depends on the event, not the prior state. */
    check_transition('R', GC_VT_STATE_INACTIVE, rel, 2, GC_VT_STATE_INACTIVE,
                     "release from already-inactive");
    check_transition('A', GC_VT_STATE_ACTIVE, acq, 2, GC_VT_STATE_ACTIVE,
                     "acquire from already-active");

    /* Unknown byte: nothing emitted, state untouched. */
    {
        ghostcon_vt_op_t ops[4];
        ghostcon_vt_state_t state = GC_VT_STATE_ACTIVE;
        if (ghostcon_vtctl_transition(&state, 'x', ops, 4) != -1 ||
            state != GC_VT_STATE_ACTIVE)
        {
            fprintf(stderr, "FAIL: unknown byte emitted ops or changed state\n");
            failures++;
        }
        else
            printf("PASS: unknown byte ignored\n");
    }

    /* Coalesced signals: every byte is acked in order, before/after the ping-pong
     * of state that a rapid release-then-reacquire produces.
     */
    {
        static const ghostcon_vt_op_t ra[] = {
            GC_VT_OP_PRE_ACK_RELEASE, GC_VT_OP_ACK_RELEASE,
            GC_VT_OP_PRE_ACK_ACQUIRE, GC_VT_OP_ACK_ACQUIRE,
        };
        static const ghostcon_vt_op_t ar[] = {
            GC_VT_OP_PRE_ACK_ACQUIRE, GC_VT_OP_ACK_ACQUIRE,
            GC_VT_OP_PRE_ACK_RELEASE, GC_VT_OP_ACK_RELEASE,
        };
        check_sequence("RA", ra, 4, GC_VT_STATE_ACTIVE, "coalesced release+acquire");
        check_sequence("AR", ar, 4, GC_VT_STATE_INACTIVE, "coalesced acquire+release");
    }

    if (failures > 0)
    {
        fprintf(stderr, "%d test(s) failed\n", failures);
        return 1;
    }
    printf("ALL VTCTL TESTS PASSED\n");
    return 0;
}
