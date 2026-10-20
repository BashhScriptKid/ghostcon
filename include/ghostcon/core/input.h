#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <ghostty/vt/key.h>

#include "transport.h"
#include "ghostcon/term/screen.h"

/* Input pipeline
 *
 * libinput capture + key encoding
 *
 * Scope per PLAN.md's "Ordered pipeline": this module is stage 1 only (libinput
 * capture
 *
 * "cannot consume/stop an event, only observes and forwards downstream") plus,
 * for Phase 1 (the `keybind` Rust component and mouse-reporting/Kitty-mode-aware
 * parts of `wrap` aren't built yet), the key encoding step too
 *
 * i.e. straight from raw evdev keys to `pty-ttyN` via transport.c, no keybind
 * interception yet.
 *
 * See PLAN.md Phase 1 item 5 and IMPLEMENTATION.md's module table.
 *
 * Split into two halves on purpose: - ghostcon_input_encode_key() is a pure
 * function (evdev keycode + action + mods + xkb-derived text -> encoded escape
 * sequence) with no libinput/hardware dependency, so it's directly unit-testable
 * (see tests/test_input.c) without real hardware.
 *
 * -ghostcon_input_t owns the actual libinput context, xkbcommon keymap/state, and
 * dispatch loop, and calls the above per event.
 */

/* Maps a Linux evdev keycode (linux/input-event-codes.h KEY_* values, as
 * returned by libinput_event_keyboard_get_key()) to Ghostty's
 * layout-independent physical key code.
 *
 * Returns GHOSTTY_KEY_UNIDENTIFIED for anything not in the W3C UI Events
 * "code" table this mirrors.
 */
extern GhosttyKey ghostcon_input_evdev_to_ghostty_key(uint32_t evdev_code);

/* Encodes one key event given already-resolved modifier/text state (the caller
 *
 * either real xkbcommon-backed dispatch, or a test
 *
 * supplies utf8/unshifted_codepoint since deriving those needs a keymap, which
 * this pure function deliberately doesn't own).
 *
 * Returns the number of bytes written to `out` (may be 0 for keys that
 * legitimately encode to nothing, e.g. a bare modifier press), or SIZE_MAX on
 * encoder error.
 */
extern size_t ghostcon_input_encode_key(GhosttyKeyEncoder encoder,
                                         GhosttyKeyAction action,
                                         uint32_t evdev_code,
                                         GhosttyMods mods,
                                         const char *utf8, size_t utf8_len,
                                         uint32_t unshifted_codepoint,
                                         char *out, size_t out_len);

/* Parses a Ghostty-style keybinding trigger string ("ctrl+shift+c") --mirrors
 * Ghostty's own Trigger.parse() syntax (mod names joined by '+', single-letter or
 * named key last) so a config author coming from Ghostty already knows the
 * format.
 *
 * Only covers what this project's [keybindings] table actually needs (letters + a
 * handful of named keys), not Ghostty's full W3C key table.
 *
 * Modifier names: ctrl/shift/alt/super, case-insensitive, order-independent, no
 * duplicates. Returns false (out-params untouched) on any unrecognized token or
 * an empty/malformed spec.
 */
extern bool ghostcon_parse_keybinding(const char *spec, GhosttyMods *out_mods, uint32_t *out_evdev_code);

typedef struct ghostcon_input ghostcon_input_t;

/* Opens a libinput context on the given seat (typically "seat0") via the udev
 * backend, and an xkbcommon context/keymap/state using the system's default
 * keyboard layout.
 *
 * Does not require root
 *
 * evdev nodes are group-readable/writable by the "input" group on most distros
 * (this machine included), and libinput's udev backend doesn't grab devices
 * exclusively, so this never interferes with the desktop session's own input
 * handling.
 */
/* Stable for this input context's lifetime: a fresh context is opened per
 * VT acquire, the same as everything else in app_t's per-acquire-cycle
 * category. See core/main.c's comment on `input`.
 *
 * Unlike cell_w/cell_h, which can change mid-lifetime via a font_size zoom
 * or reload, the values below are passed fresh to
 * ghostcon_input_dispatch() instead of being fixed here.
 */
extern ghostcon_input_t *ghostcon_input_open(const char *seat_id, int viewport_w, int viewport_h,
                                              int repeat_delay_ms, int repeat_rate_ms);
extern void ghostcon_input_close(ghostcon_input_t *input);

/* fd suitable for poll()'ing in the main event loop. */
extern int ghostcon_input_fd(const ghostcon_input_t *input);

/* Key auto-repeat timer fd, suitable for poll()ing (POLLIN) alongside
 * ghostcon_input_fd() above.
 *
 * Fires once after the initial delay, then at a fixed interval, for as
 * long as a repeatable key is held. Modifiers are excluded automatically.
 *
 * -1 if repeat is unavailable (timerfd_create() failed at
 * ghostcon_input_open() time), in which case auto-repeat silently does
 * nothing.
 */
extern int ghostcon_input_repeat_fd(const ghostcon_input_t *input);

/* Call when ghostcon_input_repeat_fd()'s fd is POLLIN-ready: drains the
 * timer and, if a key is still marked as repeating, resends the exact bytes
 * its initial press produced.
 *
 * Returns false only on a transport write failure, matching
 * ghostcon_input_dispatch()'s own convention.
 */
extern bool ghostcon_input_repeat_fire(ghostcon_input_t *input, ghostcon_transport_t *transport);

/* Mirrors the handful of terminal modes the key encoder needs to know about
 * (currently just DECCKM/application-cursor) from live screen state into
 * the encoder.
 *
 * Cheap. Call before dispatch each time, there's no dirty-tracking for this
 * on the screen side.
 */
extern void ghostcon_input_sync_modes(ghostcon_input_t *input, const ghostcon_screen_t *screen);

/* Pushes the configured copy/paste keybindings (already parsed via
 * ghostcon_parse_keybinding()) into this input context
 *
 * a mutable setter rather than an ghostcon_input_open() constructor parameter
 * because [keybindings] is hot-reloadable, same as every other config-driven
 * value in this tree; core/main.c calls this once after ghostcon_input_open() and
 * again on every config reload.
 *
 * Defaults (ctrl+shift+c/v) are already set at ghostcon_input_open() time, so
 * copy/paste work correctly even before the first call to this.
 */
extern void ghostcon_input_set_clipboard_bindings(ghostcon_input_t *input,
                                                   GhosttyMods copy_mods, uint32_t copy_evdev,
                                                   GhosttyMods paste_mods, uint32_t paste_evdev);

/* Pushes the [mouse]/[touchpad] config tables (config.h's own doc comment has the
 * field-by-field libinput mapping) into this input context, applying immediately
 * to every currently-tracked device of that class (not just newly hotplugged
 * ones).
 *
 * Same mutable-setter/ hot-reload convention as
 * ghostcon_input_set_clipboard_bindings() above
 *
 * core/main.c calls each once after ghostcon_input_open() and again on every
 * config reload.
 *
 * Defaults (enabled, 1.0 scroll speed, tap-to-click on, natural-scroll off,
 * libinput's own default sensitivity) are already set at ghostcon_input_open()
 * time.
 */
extern void ghostcon_input_set_mouse_config(ghostcon_input_t *input, bool enable,
                                             float scroll_speed, float sensitivity);
extern void ghostcon_input_set_touchpad_config(ghostcon_input_t *input, bool enable,
                                                float scroll_speed, bool tap_to_click,
                                                bool natural_scroll, float sensitivity);

/* Reported by ghostcon_input_dispatch() below
 *
 * the absolute pointer pixel position (top-left origin, same space as everything
 * else in this tree), always valid, plus whether it changed this call.
 *
 * Caller (core/main.c) uses `moved` to decide whether to call
 * ghostcon_kms_move_cursor() --deliberately NOT folded into the render-on-dirty
 * path, since the whole point of a hardware cursor plane is that its movement
 * latency doesn't depend on content rendering (see kms.h's own doc comment on
 * ghostcon_kms_move_cursor).
 */
typedef struct
{
    int  x, y;
    bool moved;
    /* Edge-triggered left-button transitions, set ONLY when this click was
     * intercepted for local text selection (see core/input.c's
     * should_intercept_for_selection()) rather than reported to the app as a
     * mouse escape sequence
     *
     * both stay false for every other event.
     *
     * Middle-click paste needs no field here. It's fully handled inside input.c
     * (direct screen/transport access, no per-frame state main.c needs to drive).
     */
    bool left_pressed;
    bool left_released;
} ghostcon_input_pointer_t;

/* Drains all pending libinput events.
 *
 * Keyboard events are encoded (see ghostcon_input_encode_key()) and written to
 * `transport`, except for two fixed shortcut sets:
 *
 * Scrolling: Shift+Up/Down/PageUp/PageDown (kmscon's grab-scroll / grab-page
 * defaults) calls ghostcon_screen_scroll_view() instead of forwarding anything to
 * the pty.
 *
 * Zoom: Ctrl+=/Ctrl+Minus, plus Ctrl+Keypad+/Ctrl+Keypad-, cannot be applied here
 * because this layer does not own the font or atlas. Each press adds +1 or -1 to
 * *out_zoom_delta
 *
 * the caller starts it at zero and applies the result, for example by rebuilding
 * the glyph atlas.
 *
 * Pointer events (motion, buttons, wheel; mice and touchpads alike) are converted
 * to a terminal column/row and encoded with ghostcon_mouse_encode() using
 * `screen`'s current protocol. They are written to `transport` like key bytes.
 *
 * An event that encodes to zero bytes (tracking off, or an unreported event type)
 * is dropped silently.
 *
 * `*out_pointer` receives the absolute pixel position whether or not an
 * application requested mouse reporting, because the cursor sprite always tracks
 * it.
 *
 * Returns false only for a transport write failure or a fatal libinput error.
 * Malformed or unmappable events are skipped.
 *
 * The caller should check the screen's dirty region afterwards, because a
 * scrollback shortcut dirties it without producing pty output.
 *
 * `active` gates everything except draining the queue and keeping XKB modifier
 * state correct. libinput's udev backend reads raw evdev events independently of
 * which VT is foreground, so a process whose VT_PROCESS release was missed would
 * otherwise react to keys typed on any VT.
 *
 * Callers should pass active=false when a kernel check (for example
 * /sys/class/tty/tty0/active) disagrees with their own ownership state.
 *
 * This is a safety net for a missed signal, not the normal mechanism[1].
 *
 * [1] While the VT is inactive, no input context normally exists. See
 *     app_t's `input` comment in core/main.c.
 *
 * With active=false, key events still update XKB modifiers and are drained, but
 * nothing is forwarded, no shortcut fires, and pointer events are skipped
 * (out_pointer is unchanged and moved=false).
 */
/* out_dump_requested: set to true (never cleared
 *
 * caller starts it at false, same convention as *out_zoom_delta) when Ctrl+Alt+D
 * was pressed this call. core/main.c owns the actual dump-to-file since it has
 * app->term.
 *
 * This function only detects the chord.
 *
 * See handle_dump_shortcut()'s own doc comment for why this exists.
 */
extern bool ghostcon_input_dispatch(ghostcon_input_t *input, ghostcon_transport_t *transport,
                                     ghostcon_screen_t *screen, int cell_w, int cell_h,
                                     int *out_zoom_delta, ghostcon_input_pointer_t *out_pointer,
                                     bool *out_dump_requested, bool active);
