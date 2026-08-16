/*
 * test_mouse -- pure-CPU unit test for term/mouse.c's
 * ghostcon_mouse_encode(). No libinput/hardware dependency, matching
 * tests/test_input.c's own split between the pure encoder and the
 * real dispatch loop.
 */

#include "ghostcon/term/mouse.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
    else { printf("PASS: %s\n", msg); } \
} while (0)

static ghostcon_screen_t
screen_with(bool tracking, uint16_t protocol, bool sgr)
{
    ghostcon_screen_t s;
    memset(&s, 0, sizeof(s));
    s.mouse_tracking = tracking;
    s.mouse_protocol = protocol;
    s.mouse_sgr = sgr;
    return s;
}

int
main(void)
{
    char buf[32];
    size_t n;

    /* Tracking disabled entirely -- always 0, regardless of event. */
    ghostcon_screen_t off = screen_with(false, 0, false);
    n = ghostcon_mouse_encode(&off, GC_MOUSE_LEFT, GC_MOUSE_PRESS, 0, 5, 3, buf, sizeof(buf));
    CHECK(n == 0, "tracking disabled: press returns 0 bytes");

    /* Mode 1000 (click-only): motion is never reported, even in a
       drag with a button held. */
    ghostcon_screen_t m1000 = screen_with(true, 1000, false);
    n = ghostcon_mouse_encode(&m1000, GC_MOUSE_LEFT, GC_MOUSE_MOTION, 0, 5, 3, buf, sizeof(buf));
    CHECK(n == 0, "mode 1000 filters out motion");
    n = ghostcon_mouse_encode(&m1000, GC_MOUSE_LEFT, GC_MOUSE_PRESS, 0, 5, 3, buf, sizeof(buf));
    CHECK(n > 0, "mode 1000 still reports press");

    /* Mode 1002 (button+drag): motion only reported while a button is
       held; bare motion (GC_MOUSE_NONE) is filtered out. */
    ghostcon_screen_t m1002 = screen_with(true, 1002, false);
    n = ghostcon_mouse_encode(&m1002, GC_MOUSE_NONE, GC_MOUSE_MOTION, 0, 5, 3, buf, sizeof(buf));
    CHECK(n == 0, "mode 1002 filters out bare motion (no button held)");
    n = ghostcon_mouse_encode(&m1002, GC_MOUSE_LEFT, GC_MOUSE_MOTION, 0, 5, 3, buf, sizeof(buf));
    CHECK(n > 0, "mode 1002 reports drag motion (button held)");

    /* Mode 1003 (any-motion): bare motion is reported. */
    ghostcon_screen_t m1003 = screen_with(true, 1003, false);
    n = ghostcon_mouse_encode(&m1003, GC_MOUSE_NONE, GC_MOUSE_MOTION, 0, 5, 3, buf, sizeof(buf));
    CHECK(n > 0, "mode 1003 reports bare motion");

    /* Wheel events always reported when tracking is on, regardless of
       protocol mode (even 1000, the most restrictive). */
    n = ghostcon_mouse_encode(&m1000, GC_MOUSE_WHEEL_UP, GC_MOUSE_PRESS, 0, 5, 3, buf, sizeof(buf));
    CHECK(n > 0, "wheel reported even in mode 1000");

    /* X10 legacy framing: "ESC[M" + 3 bytes, col/row offset by 33
       (1-based + 32), button by 32. Left press, no mods, col=5,row=3
       (0-based) -> Cb=0+32=32, Cx=6+32=38, Cy=4+32=36. */
    n = ghostcon_mouse_encode(&m1000, GC_MOUSE_LEFT, GC_MOUSE_PRESS, 0, 5, 3, buf, sizeof(buf));
    CHECK(n == 6, "X10 framing is exactly 6 bytes");
    CHECK(memcmp(buf, "\x1b[M", 3) == 0, "X10 framing starts with ESC[M");
    CHECK((unsigned char)buf[3] == 32, "X10 left-press button byte");
    CHECK((unsigned char)buf[4] == 38, "X10 column byte (col=5 0-based -> 38)");
    CHECK((unsigned char)buf[5] == 36, "X10 row byte (row=3 0-based -> 36)");

    /* SGR framing: "ESC[<Cb;Cx;CyM" (press) / "...m" (release),
       decimal, 1-based, no offset. */
    ghostcon_screen_t sgr = screen_with(true, 1000, true);
    n = ghostcon_mouse_encode(&sgr, GC_MOUSE_LEFT, GC_MOUSE_PRESS, 0, 5, 3, buf, sizeof(buf));
    buf[n] = '\0';
    CHECK(strcmp(buf, "\x1b[<0;6;4M") == 0, "SGR press framing");
    n = ghostcon_mouse_encode(&sgr, GC_MOUSE_LEFT, GC_MOUSE_RELEASE, 0, 5, 3, buf, sizeof(buf));
    buf[n] = '\0';
    CHECK(strcmp(buf, "\x1b[<0;6;4m") == 0, "SGR release framing (lowercase m, button number kept)");

    /* Modifier bits: shift=4, alt=8, ctrl=16, additive. */
    n = ghostcon_mouse_encode(&sgr, GC_MOUSE_LEFT, GC_MOUSE_PRESS,
                               GHOSTTY_MODS_SHIFT | GHOSTTY_MODS_CTRL, 0, 0, buf, sizeof(buf));
    buf[n] = '\0';
    CHECK(strcmp(buf, "\x1b[<20;1;1M") == 0, "SGR shift+ctrl modifier bits (4+16=20)");

    /* Wheel button codes: up=64, down=65. */
    n = ghostcon_mouse_encode(&sgr, GC_MOUSE_WHEEL_UP, GC_MOUSE_PRESS, 0, 0, 0, buf, sizeof(buf));
    buf[n] = '\0';
    CHECK(strcmp(buf, "\x1b[<64;1;1M") == 0, "SGR wheel-up button code");
    n = ghostcon_mouse_encode(&sgr, GC_MOUSE_WHEEL_DOWN, GC_MOUSE_PRESS, 0, 0, 0, buf, sizeof(buf));
    buf[n] = '\0';
    CHECK(strcmp(buf, "\x1b[<65;1;1M") == 0, "SGR wheel-down button code");

    if (failures > 0) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("ALL TESTS PASSED\n");
    return 0;
}
