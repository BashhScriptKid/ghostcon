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

/* Parses a Ghostty-style keybinding trigger string ("ctrl+shift+c") --
   mirrors Ghostty's own Trigger.parse() syntax (mod names joined by
   '+', single-letter or named key last) so a config author coming
   from Ghostty already knows the format. Only covers what this
   project's [keybindings] table actually needs (letters + a handful
   of named keys), not Ghostty's full W3C key table. Modifier names:
   ctrl/shift/alt/super, case-insensitive, order-independent, no
   duplicates. Returns false (out-params untouched) on any unrecognized
   token or an empty/malformed spec. */
bool ghostcon_parse_keybinding(const char *spec, GhosttyMods *out_mods, uint32_t *out_evdev_code);

typedef struct ghostcon_input ghostcon_input_t;

/* Opens a libinput context on the given seat (typically "seat0") via
   the udev backend, and an xkbcommon context/keymap/state using the
   system's default keyboard layout. Does not require root — evdev
   nodes are group-readable/writable by the "input" group on most
   distros (this machine included), and libinput's udev backend
   doesn't grab devices exclusively, so this never interferes with the
   desktop session's own input handling. */
/* viewport_w/viewport_h are the physical screen pixel dimensions
   (core/kms.c's kms->width/height) -- used to clamp the absolute
   pointer position tracked internally. Stable for this input context's
   whole lifetime (a fresh context is opened per VT acquire, same as
   everything else in app_t's "per-acquire-cycle" category -- see
   core/main.c's own doc comment on `input`), unlike cell_w/cell_h
   (which CAN change mid-lifetime via a font_size zoom/reload) --
   that's why those are passed fresh to ghostcon_input_dispatch() below
   instead of being fixed here too. */
ghostcon_input_t *ghostcon_input_open(const char *seat_id, int viewport_w, int viewport_h);
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

/* Pushes the configured copy/paste keybindings (already parsed via
   ghostcon_parse_keybinding()) into this input context -- a mutable
   setter rather than an ghostcon_input_open() constructor parameter
   because [keybindings] is hot-reloadable, same as every other
   config-driven value in this tree; core/main.c calls this once after
   ghostcon_input_open() and again on every config reload. Defaults
   (ctrl+shift+c/v) are already set at ghostcon_input_open() time, so
   copy/paste work correctly even before the first call to this. */
void ghostcon_input_set_clipboard_bindings(ghostcon_input_t *input,
                                            GhosttyMods copy_mods, uint32_t copy_evdev,
                                            GhosttyMods paste_mods, uint32_t paste_evdev);

/* Reported by ghostcon_input_dispatch() below -- the absolute pointer
   pixel position (top-left origin, same space as everything else in
   this tree), always valid, plus whether it changed this call. Caller
   (core/main.c) uses `moved` to decide whether to call
   ghostcon_kms_move_cursor() -- deliberately NOT folded into the
   render-on-dirty path, since the whole point of a hardware cursor
   plane is that its movement latency doesn't depend on content
   rendering (see kms.h's own doc comment on ghostcon_kms_move_cursor). */
typedef struct {
    int  x, y;
    bool moved;
    /* Edge-triggered left-button transitions, set ONLY when this click
       was intercepted for local text selection (see core/input.c's
       should_intercept_for_selection()) rather than reported to the
       app as a mouse escape sequence -- both stay false for every
       other event. Middle-click paste needs no field here; it's fully
       handled inside input.c (direct screen/transport access, no
       per-frame state main.c needs to drive). */
    bool left_pressed;
    bool left_released;
} ghostcon_input_pointer_t;

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

   Pointer events (motion, buttons, wheel -- mice AND touchpads alike,
   see core/input.c's handle_pointer_event() doc comment for why
   libinput makes that automatic) are converted to a terminal column/
   row using `cell_w`/`cell_h` and encoded via term/mouse.c's
   ghostcon_mouse_encode() per `screen`'s currently negotiated
   protocol, written to `transport` the same way keyboard bytes are
   (0 bytes encoded — tracking off, or this event type isn't reported
   in the current mode — is silently not sent, not an error). The
   absolute pointer pixel position (independent of whether any app
   requested mouse reporting -- the visible cursor sprite always
   tracks it) is reported via `*out_pointer`.

   Returns false only on a transport write failure or fatal libinput
   error; individual malformed/unmappable events are skipped, not
   fatal. Caller should check screen's dirty region after this call
   (a scrollback shortcut marks it dirty without producing any pty
   output to trigger the caller's usual render-on-new-data path).

   `active` gates everything EXCEPT draining the queue and keeping XKB
   modifier tracking correct -- found live: libinput's udev backend
   reads raw evdev events directly, completely independent of which VT
   is actually the kernel's current foreground one (same reasoning
   is_vt_switch_combo()'s own doc comment already covers for Ctrl+Alt+Fn
   specifically) -- so a ghostcon-core instance whose VT_PROCESS release
   signal was ever missed (leaving app->display_acquired stuck true)
   would otherwise keep acting on every keystroke typed ANYWHERE on the
   physical keyboard, including zoom/scroll shortcuts and raw key
   forwarding to its own pty, even while a completely different VT is
   what's actually visible. Callers should pass `active = false`
   whenever a direct kernel check (e.g. /sys/class/tty/tty0/active)
   disagrees with this process's own tracked VT-ownership state, as a
   defensive safety net for exactly that missed-signal case -- NOT as
   the primary mechanism for "don't process input while inactive"
   (that's already handled by not having an open ghostcon_input_t at
   all outside an acquire/release cycle; see core/main.c's app_t doc
   comment on `input`). When false: keyboard events still update XKB
   modifier state (so Ctrl/Shift tracking doesn't desync for whenever
   this DOES become active again) and are still drained from the
   queue (avoiding the backlog-buildup problem this same mechanism
   exists to prevent elsewhere), but no key bytes are forwarded to
   `transport`, no shortcuts fire, and pointer events are skipped
   entirely (out_pointer stays at its already-tracked position,
   moved=false). */
/* out_dump_requested: set to true (never cleared -- caller starts it
   at false, same convention as *out_zoom_delta) when Ctrl+Alt+D was
   pressed this call. core/main.c owns the actual dump-to-file since
   it has app->term; this function only detects the chord. See
   handle_dump_shortcut()'s own doc comment for why this exists. */
bool ghostcon_input_dispatch(ghostcon_input_t *input, ghostcon_transport_t *transport,
                              ghostcon_screen_t *screen, int cell_w, int cell_h,
                              int *out_zoom_delta, ghostcon_input_pointer_t *out_pointer,
                              bool *out_dump_requested, bool active);
