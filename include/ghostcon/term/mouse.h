#pragma once

#include <stddef.h>
#include <stdint.h>

#include <ghostty/vt/key.h>

#include "screen.h"

/* ------------------------------------------------------------------ */
/* Mouse-reporting escape sequence encoding                            */
/*                                                                     */
/* Pure function, no libinput/hardware dependency (mirrors             */
/* core/input.c's own split between ghostcon_input_encode_key() and    */
/* the real libinput-backed dispatch loop, for the same reason:        */
/* directly unit-testable without real hardware). See PLAN.md's        */
/* "Mouse support, pass 1" section for the protocol reference this     */
/* implements against.                                                 */
/* ------------------------------------------------------------------ */

typedef enum {
    GC_MOUSE_LEFT,
    GC_MOUSE_MIDDLE,
    GC_MOUSE_RIGHT,
    GC_MOUSE_WHEEL_UP,
    GC_MOUSE_WHEEL_DOWN,
    GC_MOUSE_NONE, /* motion with no button held */
} ghostcon_mouse_button_t;

typedef enum {
    GC_MOUSE_PRESS,
    GC_MOUSE_RELEASE,
    GC_MOUSE_MOTION,
} ghostcon_mouse_action_t;

/* Encodes one mouse event as an xterm-protocol escape sequence, per
   `screen`'s currently negotiated mouse_tracking/mouse_protocol/
   mouse_sgr state (see term/stream.c's DECSET/DECRST 1000/1002/1003/
   1006 handling). `col`/`row` are 0-based grid coordinates.

   Returns the number of bytes written to `buf`, or 0 if this event
   should not be reported at all given the negotiated protocol:
     - mouse_tracking == false: always 0 (nothing requested tracking).
     - mode 1000: only PRESS/RELEASE are reported, never MOTION.
     - mode 1002: PRESS/RELEASE always; MOTION only while `button` is
       held (i.e. `button != GC_MOUSE_NONE`) -- "drag" reporting.
     - mode 1003: every MOTION is reported regardless of button state.
   Wheel events (GC_MOUSE_WHEEL_UP/DOWN) are always reported as PRESS
   when tracking is on, regardless of protocol mode (matches real
   terminals -- wheel scroll isn't gated by the motion-reporting mode).

   Framing: SGR (`ESC[<Cb;Cx;Cy` + 'M'/'m', decimal, unlimited
   coordinate range) when screen->mouse_sgr is set; otherwise legacy
   X10 (`ESC[M` + 3 raw bytes, coordinates clamped to a 223 max, no
   separate release signal -- release is always encoded as button
   code 3). buf_len should be at least 32 to guarantee the worst case
   (SGR with large coordinates) always fits. */
size_t ghostcon_mouse_encode(const ghostcon_screen_t *screen,
                              ghostcon_mouse_button_t button,
                              ghostcon_mouse_action_t action,
                              GhosttyMods mods, int col, int row,
                              char *buf, size_t buf_len);
