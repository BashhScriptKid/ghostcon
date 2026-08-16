#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <ghostty/vt/key.h>

#include "transport.h"
#include "ghostcon/term/screen.h"

/* ------------------------------------------------------------------ */
/* Input pipeline — libinput capture + key encoding                    */
/*                                                                     */
/* Scope per PLAN.md's "Ordered pipeline": this module is stage 1      */
/* only (libinput capture — "cannot consume/stop an event, only        */
/* observes and forwards downstream") plus, for Phase 1 (the `keybind` */
/* Rust component and mouse-reporting/Kitty-mode-aware parts of `wrap` */
/* aren't built yet), the key encoding step too — i.e. straight from   */
/* raw evdev keys to `pty-ttyN` via transport.c, no keybind            */
/* interception yet. See PLAN.md Phase 1 item 5 and IMPLEMENTATION.md's*/
/* module table.                                                       */
/*                                                                     */
/* Split into two halves on purpose:                                   */
/*   - ghostcon_input_encode_key() is a pure function (evdev keycode + */
/*     action + mods + xkb-derived text -> encoded escape sequence)    */
/*     with no libinput/hardware dependency, so it's directly          */
/*     unit-testable (see tests/test_input.c) without real hardware.   */
/*   - ghostcon_input_t owns the actual libinput context, xkbcommon    */
/*     keymap/state, and dispatch loop, and calls the above per event. */
/* ------------------------------------------------------------------ */

/* Maps a Linux evdev keycode (linux/input-event-codes.h KEY_* values,
   as returned by libinput_event_keyboard_get_key()) to Ghostty's
   layout-independent physical key code. Returns GHOSTTY_KEY_UNIDENTIFIED
   for anything not in the W3C UI Events "code" table this mirrors. */
GhosttyKey ghostcon_input_evdev_to_ghostty_key(uint32_t evdev_code);

/* Encodes one key event given already-resolved modifier/text state
   (the caller — either real xkbcommon-backed dispatch, or a test —
   supplies utf8/unshifted_codepoint since deriving those needs a
   keymap, which this pure function deliberately doesn't own). Returns
   the number of bytes written to `out` (may be 0 for keys that
   legitimately encode to nothing, e.g. a bare modifier press), or
   SIZE_MAX on encoder error. */
size_t ghostcon_input_encode_key(GhosttyKeyEncoder encoder,
                                  GhosttyKeyAction action,
                                  uint32_t evdev_code,
                                  GhosttyMods mods,
                                  const char *utf8, size_t utf8_len,
                                  uint32_t unshifted_codepoint,
                                  char *out, size_t out_len);

typedef struct ghostcon_input ghostcon_input_t;

/* Opens a libinput context on the given seat (typically "seat0") via
   the udev backend, and an xkbcommon context/keymap/state using the
   system's default keyboard layout. Does not require root — evdev
   nodes are group-readable/writable by the "input" group on most
   distros (this machine included), and libinput's udev backend
   doesn't grab devices exclusively, so this never interferes with the
   desktop session's own input handling. */
ghostcon_input_t *ghostcon_input_open(const char *seat_id);
void ghostcon_input_close(ghostcon_input_t *input);

/* fd suitable for poll()'ing in the main event loop. */
int ghostcon_input_fd(const ghostcon_input_t *input);

/* Key auto-repeat timer fd, suitable for poll()'ing (POLLIN) alongside
   ghostcon_input_fd() above -- fires once after an initial delay, then
   at a fixed interval, for as long as a repeatable key (per the active
   XKB keymap -- modifiers are excluded automatically) is held. -1 if
   repeat is unavailable (timerfd_create() failed at ghostcon_input_open()
   time) -- not fatal, caller should just skip polling it. */
int ghostcon_input_repeat_fd(const ghostcon_input_t *input);

/* Call when ghostcon_input_repeat_fd()'s fd is POLLIN-ready: drains the
   timer and, if a key is still marked as repeating, resends the exact
   bytes its initial press produced. Returns false only on a transport
   write failure, matching ghostcon_input_dispatch()'s own convention. */
bool ghostcon_input_repeat_fire(ghostcon_input_t *input, ghostcon_transport_t *transport);

/* Mirrors the handful of terminal modes the key encoder needs to know
   about (currently just DECCKM/application-cursor) from live screen
   state into the encoder. Cheap; call before dispatch each time,
   there's no dirty-tracking for this on the screen side. */
void ghostcon_input_sync_modes(ghostcon_input_t *input, const ghostcon_screen_t *screen);

/* Drains all pending libinput events. Keyboard key events are encoded
   (see ghostcon_input_encode_key) and written directly to `transport`
   — no keybind interception yet, see the file-level comment — except
   for two small fixed sets of shortcuts:
     - scrollback (Shift+Up/Down/PageUp/PageDown, mirroring kmscon's
       grab-scroll/grab-page defaults), applied directly to `screen` via
       ghostcon_screen_scroll_view() instead of being forwarded to the pty.
     - zoom (Ctrl+=/Ctrl+Minus, plus Ctrl+Keypad+/Ctrl+Keypad- aliases),
       which this function can't apply itself (it has no font/atlas
       ownership -- that lives in core/main.c's app_t) -- instead it
       accumulates the net requested change into *out_zoom_delta (each
       press is +-1; caller starts it at 0 and applies the final value,
       e.g. via a helper that rebuilds the glyph atlas at the new size).
   Pointer/touchpad events are currently observed and discarded (mouse
   reporting is `wrap`'s job per PLAN.md and isn't wired up yet).
   Returns false only on a transport write failure or fatal libinput
   error; individual malformed/unmappable events are skipped, not
   fatal. Caller should check screen's dirty region after this call
   (a scrollback shortcut marks it dirty without producing any pty
   output to trigger the caller's usual render-on-new-data path). */
bool ghostcon_input_dispatch(ghostcon_input_t *input, ghostcon_transport_t *transport,
                              ghostcon_screen_t *screen, int *out_zoom_delta);
