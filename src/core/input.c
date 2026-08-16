#include "ghostcon/core/input.h"

#include <fcntl.h>
#include <libinput.h>
#include <libudev.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon.h>

#include <linux/input-event-codes.h>

/* ------------------------------------------------------------------ */
/* evdev keycode -> GhosttyKey (W3C UI Events "code" table)             */
/* ------------------------------------------------------------------ */

GhosttyKey
ghostcon_input_evdev_to_ghostty_key(uint32_t evdev_code)
{
    switch (evdev_code) {
    case KEY_GRAVE:      return GHOSTTY_KEY_BACKQUOTE;
    case KEY_BACKSLASH:  return GHOSTTY_KEY_BACKSLASH;
    case KEY_LEFTBRACE:  return GHOSTTY_KEY_BRACKET_LEFT;
    case KEY_RIGHTBRACE: return GHOSTTY_KEY_BRACKET_RIGHT;
    case KEY_COMMA:      return GHOSTTY_KEY_COMMA;
    case KEY_0: return GHOSTTY_KEY_DIGIT_0;
    case KEY_1: return GHOSTTY_KEY_DIGIT_1;
    case KEY_2: return GHOSTTY_KEY_DIGIT_2;
    case KEY_3: return GHOSTTY_KEY_DIGIT_3;
    case KEY_4: return GHOSTTY_KEY_DIGIT_4;
    case KEY_5: return GHOSTTY_KEY_DIGIT_5;
    case KEY_6: return GHOSTTY_KEY_DIGIT_6;
    case KEY_7: return GHOSTTY_KEY_DIGIT_7;
    case KEY_8: return GHOSTTY_KEY_DIGIT_8;
    case KEY_9: return GHOSTTY_KEY_DIGIT_9;
    case KEY_EQUAL:      return GHOSTTY_KEY_EQUAL;
    case KEY_102ND:      return GHOSTTY_KEY_INTL_BACKSLASH;
    case KEY_RO:         return GHOSTTY_KEY_INTL_RO;
    case KEY_YEN:        return GHOSTTY_KEY_INTL_YEN;
    case KEY_A: return GHOSTTY_KEY_A;
    case KEY_B: return GHOSTTY_KEY_B;
    case KEY_C: return GHOSTTY_KEY_C;
    case KEY_D: return GHOSTTY_KEY_D;
    case KEY_E: return GHOSTTY_KEY_E;
    case KEY_F: return GHOSTTY_KEY_F;
    case KEY_G: return GHOSTTY_KEY_G;
    case KEY_H: return GHOSTTY_KEY_H;
    case KEY_I: return GHOSTTY_KEY_I;
    case KEY_J: return GHOSTTY_KEY_J;
    case KEY_K: return GHOSTTY_KEY_K;
    case KEY_L: return GHOSTTY_KEY_L;
    case KEY_M: return GHOSTTY_KEY_M;
    case KEY_N: return GHOSTTY_KEY_N;
    case KEY_O: return GHOSTTY_KEY_O;
    case KEY_P: return GHOSTTY_KEY_P;
    case KEY_Q: return GHOSTTY_KEY_Q;
    case KEY_R: return GHOSTTY_KEY_R;
    case KEY_S: return GHOSTTY_KEY_S;
    case KEY_T: return GHOSTTY_KEY_T;
    case KEY_U: return GHOSTTY_KEY_U;
    case KEY_V: return GHOSTTY_KEY_V;
    case KEY_W: return GHOSTTY_KEY_W;
    case KEY_X: return GHOSTTY_KEY_X;
    case KEY_Y: return GHOSTTY_KEY_Y;
    case KEY_Z: return GHOSTTY_KEY_Z;
    case KEY_MINUS:      return GHOSTTY_KEY_MINUS;
    case KEY_DOT:        return GHOSTTY_KEY_PERIOD;
    case KEY_APOSTROPHE: return GHOSTTY_KEY_QUOTE;
    case KEY_SEMICOLON:  return GHOSTTY_KEY_SEMICOLON;
    case KEY_SLASH:      return GHOSTTY_KEY_SLASH;

    case KEY_LEFTALT:    return GHOSTTY_KEY_ALT_LEFT;
    case KEY_RIGHTALT:   return GHOSTTY_KEY_ALT_RIGHT;
    case KEY_BACKSPACE:  return GHOSTTY_KEY_BACKSPACE;
    case KEY_CAPSLOCK:   return GHOSTTY_KEY_CAPS_LOCK;
    case KEY_COMPOSE:    return GHOSTTY_KEY_CONTEXT_MENU;
    case KEY_LEFTCTRL:   return GHOSTTY_KEY_CONTROL_LEFT;
    case KEY_RIGHTCTRL:  return GHOSTTY_KEY_CONTROL_RIGHT;
    case KEY_ENTER:      return GHOSTTY_KEY_ENTER;
    case KEY_LEFTMETA:   return GHOSTTY_KEY_META_LEFT;
    case KEY_RIGHTMETA:  return GHOSTTY_KEY_META_RIGHT;
    case KEY_LEFTSHIFT:  return GHOSTTY_KEY_SHIFT_LEFT;
    case KEY_RIGHTSHIFT: return GHOSTTY_KEY_SHIFT_RIGHT;
    case KEY_SPACE:      return GHOSTTY_KEY_SPACE;
    case KEY_TAB:        return GHOSTTY_KEY_TAB;
    case KEY_HENKAN:     return GHOSTTY_KEY_CONVERT;
    case KEY_KATAKANAHIRAGANA: return GHOSTTY_KEY_KANA_MODE;
    case KEY_MUHENKAN:   return GHOSTTY_KEY_NON_CONVERT;

    case KEY_DELETE:     return GHOSTTY_KEY_DELETE;
    case KEY_END:        return GHOSTTY_KEY_END;
    case KEY_HELP:       return GHOSTTY_KEY_HELP;
    case KEY_HOME:       return GHOSTTY_KEY_HOME;
    case KEY_INSERT:     return GHOSTTY_KEY_INSERT;
    case KEY_PAGEDOWN:   return GHOSTTY_KEY_PAGE_DOWN;
    case KEY_PAGEUP:     return GHOSTTY_KEY_PAGE_UP;

    case KEY_DOWN:  return GHOSTTY_KEY_ARROW_DOWN;
    case KEY_LEFT:  return GHOSTTY_KEY_ARROW_LEFT;
    case KEY_RIGHT: return GHOSTTY_KEY_ARROW_RIGHT;
    case KEY_UP:    return GHOSTTY_KEY_ARROW_UP;

    case KEY_NUMLOCK: return GHOSTTY_KEY_NUM_LOCK;
    case KEY_KP0: return GHOSTTY_KEY_NUMPAD_0;
    case KEY_KP1: return GHOSTTY_KEY_NUMPAD_1;
    case KEY_KP2: return GHOSTTY_KEY_NUMPAD_2;
    case KEY_KP3: return GHOSTTY_KEY_NUMPAD_3;
    case KEY_KP4: return GHOSTTY_KEY_NUMPAD_4;
    case KEY_KP5: return GHOSTTY_KEY_NUMPAD_5;
    case KEY_KP6: return GHOSTTY_KEY_NUMPAD_6;
    case KEY_KP7: return GHOSTTY_KEY_NUMPAD_7;
    case KEY_KP8: return GHOSTTY_KEY_NUMPAD_8;
    case KEY_KP9: return GHOSTTY_KEY_NUMPAD_9;
    case KEY_KPPLUS:     return GHOSTTY_KEY_NUMPAD_ADD;
    case KEY_KPCOMMA:    return GHOSTTY_KEY_NUMPAD_COMMA;
    case KEY_KPDOT:      return GHOSTTY_KEY_NUMPAD_DECIMAL;
    case KEY_KPSLASH:    return GHOSTTY_KEY_NUMPAD_DIVIDE;
    case KEY_KPENTER:    return GHOSTTY_KEY_NUMPAD_ENTER;
    case KEY_KPEQUAL:    return GHOSTTY_KEY_NUMPAD_EQUAL;
    case KEY_KPASTERISK: return GHOSTTY_KEY_NUMPAD_MULTIPLY;
    case KEY_KPLEFTPAREN:  return GHOSTTY_KEY_NUMPAD_PAREN_LEFT;
    case KEY_KPRIGHTPAREN: return GHOSTTY_KEY_NUMPAD_PAREN_RIGHT;
    case KEY_KPMINUS:    return GHOSTTY_KEY_NUMPAD_SUBTRACT;

    case KEY_ESC: return GHOSTTY_KEY_ESCAPE;
    case KEY_F1:  return GHOSTTY_KEY_F1;
    case KEY_F2:  return GHOSTTY_KEY_F2;
    case KEY_F3:  return GHOSTTY_KEY_F3;
    case KEY_F4:  return GHOSTTY_KEY_F4;
    case KEY_F5:  return GHOSTTY_KEY_F5;
    case KEY_F6:  return GHOSTTY_KEY_F6;
    case KEY_F7:  return GHOSTTY_KEY_F7;
    case KEY_F8:  return GHOSTTY_KEY_F8;
    case KEY_F9:  return GHOSTTY_KEY_F9;
    case KEY_F10: return GHOSTTY_KEY_F10;
    case KEY_F11: return GHOSTTY_KEY_F11;
    case KEY_F12: return GHOSTTY_KEY_F12;
    case KEY_F13: return GHOSTTY_KEY_F13;
    case KEY_F14: return GHOSTTY_KEY_F14;
    case KEY_F15: return GHOSTTY_KEY_F15;
    case KEY_F16: return GHOSTTY_KEY_F16;
    case KEY_F17: return GHOSTTY_KEY_F17;
    case KEY_F18: return GHOSTTY_KEY_F18;
    case KEY_F19: return GHOSTTY_KEY_F19;
    case KEY_F20: return GHOSTTY_KEY_F20;
    case KEY_F21: return GHOSTTY_KEY_F21;
    case KEY_F22: return GHOSTTY_KEY_F22;
    case KEY_F23: return GHOSTTY_KEY_F23;
    case KEY_F24: return GHOSTTY_KEY_F24;
    case KEY_FN:  return GHOSTTY_KEY_FN;
    case KEY_SYSRQ:      return GHOSTTY_KEY_PRINT_SCREEN;
    case KEY_SCROLLLOCK: return GHOSTTY_KEY_SCROLL_LOCK;
    case KEY_PAUSE:      return GHOSTTY_KEY_PAUSE;

    case KEY_BACK:      return GHOSTTY_KEY_BROWSER_BACK;
    case KEY_FORWARD:   return GHOSTTY_KEY_BROWSER_FORWARD;
    case KEY_HOMEPAGE:  return GHOSTTY_KEY_BROWSER_HOME;
    case KEY_REFRESH:   return GHOSTTY_KEY_BROWSER_REFRESH;
    case KEY_SEARCH:    return GHOSTTY_KEY_BROWSER_SEARCH;
    case KEY_STOP:      return GHOSTTY_KEY_BROWSER_STOP;
    case KEY_EJECTCD:   return GHOSTTY_KEY_EJECT;
    case KEY_MAIL:      return GHOSTTY_KEY_LAUNCH_MAIL;
    case KEY_PLAYPAUSE:    return GHOSTTY_KEY_MEDIA_PLAY_PAUSE;
    case KEY_STOPCD:       return GHOSTTY_KEY_MEDIA_STOP;
    case KEY_NEXTSONG:     return GHOSTTY_KEY_MEDIA_TRACK_NEXT;
    case KEY_PREVIOUSSONG: return GHOSTTY_KEY_MEDIA_TRACK_PREVIOUS;
    case KEY_POWER: return GHOSTTY_KEY_POWER;
    case KEY_SLEEP: return GHOSTTY_KEY_SLEEP;
    case KEY_VOLUMEDOWN: return GHOSTTY_KEY_AUDIO_VOLUME_DOWN;
    case KEY_MUTE:       return GHOSTTY_KEY_AUDIO_VOLUME_MUTE;
    case KEY_VOLUMEUP:   return GHOSTTY_KEY_AUDIO_VOLUME_UP;
    case KEY_WAKEUP:     return GHOSTTY_KEY_WAKE_UP;

    case KEY_COPY:  return GHOSTTY_KEY_COPY;
    case KEY_CUT:   return GHOSTTY_KEY_CUT;
    case KEY_PASTE: return GHOSTTY_KEY_PASTE;

    default: return GHOSTTY_KEY_UNIDENTIFIED;
    }
}

/* ------------------------------------------------------------------ */
/* Pure per-event encode step                                          */
/* ------------------------------------------------------------------ */

size_t
ghostcon_input_encode_key(GhosttyKeyEncoder encoder,
                           GhosttyKeyAction action,
                           uint32_t evdev_code,
                           GhosttyMods mods,
                           const char *utf8, size_t utf8_len,
                           uint32_t unshifted_codepoint,
                           char *out, size_t out_len)
{
    GhosttyKeyEvent event;
    if (ghostty_key_event_new(NULL, &event) != GHOSTTY_SUCCESS)
        return (size_t)-1;

    ghostty_key_event_set_action(event, action);
    ghostty_key_event_set_key(event, ghostcon_input_evdev_to_ghostty_key(evdev_code));
    ghostty_key_event_set_mods(event, mods);
    ghostty_key_event_set_utf8(event, utf8, utf8_len);
    ghostty_key_event_set_unshifted_codepoint(event, unshifted_codepoint);

    size_t written = 0;
    GhosttyResult result = ghostty_key_encoder_encode(encoder, event, out, out_len, &written);

    ghostty_key_event_free(event);

    return result == GHOSTTY_SUCCESS ? written : (size_t)-1;
}

/* ------------------------------------------------------------------ */
/* Live libinput + xkbcommon context                                   */
/* ------------------------------------------------------------------ */

struct ghostcon_input {
    struct udev *udev;
    struct libinput *li;

    struct xkb_context *xkb_ctx;
    struct xkb_keymap *xkb_keymap;
    struct xkb_state *xkb_state;
    /* Fresh, never-updated state for querying the "unshifted" codepoint
       of a key — see ghostcon_input_dispatch. */
    struct xkb_state *xkb_state_unshifted;

    GhosttyKeyEncoder encoder;
};

static int
open_restricted(const char *path, int flags, void *user_data)
{
    (void)user_data;
    return open(path, flags);
}

static void
close_restricted(int fd, void *user_data)
{
    (void)user_data;
    close(fd);
}

static const struct libinput_interface LI_INTERFACE = {
    .open_restricted = open_restricted,
    .close_restricted = close_restricted,
};

ghostcon_input_t *
ghostcon_input_open(const char *seat_id)
{
    ghostcon_input_t *input = calloc(1, sizeof(*input));
    if (!input)
        return NULL;

    input->udev = udev_new();
    if (!input->udev) {
        fprintf(stderr, "input: udev_new failed\n");
        goto fail;
    }

    input->li = libinput_udev_create_context(&LI_INTERFACE, NULL, input->udev);
    if (!input->li) {
        fprintf(stderr, "input: libinput_udev_create_context failed\n");
        goto fail;
    }
    if (libinput_udev_assign_seat(input->li, seat_id) != 0) {
        fprintf(stderr, "input: libinput_udev_assign_seat(%s) failed\n", seat_id);
        goto fail;
    }

    input->xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!input->xkb_ctx) {
        fprintf(stderr, "input: xkb_context_new failed\n");
        goto fail;
    }
    input->xkb_keymap = xkb_keymap_new_from_names(input->xkb_ctx, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!input->xkb_keymap) {
        fprintf(stderr, "input: xkb_keymap_new_from_names failed\n");
        goto fail;
    }
    input->xkb_state = xkb_state_new(input->xkb_keymap);
    input->xkb_state_unshifted = xkb_state_new(input->xkb_keymap);
    if (!input->xkb_state || !input->xkb_state_unshifted) {
        fprintf(stderr, "input: xkb_state_new failed\n");
        goto fail;
    }

    if (ghostty_key_encoder_new(NULL, &input->encoder) != GHOSTTY_SUCCESS) {
        fprintf(stderr, "input: ghostty_key_encoder_new failed\n");
        goto fail;
    }

    /* Explicit legacy-mode defaults -- found live: Ctrl+D (and by
       extension every other Ctrl+letter combo) was being encoded as
       kitty keyboard protocol CSI-u sequences (e.g. "\x1b[4;5u")
       instead of the plain single control byte (0x04) any ordinary
       shell expects, landing as literal garbage text instead of being
       interpreted. Real terminals only enable the kitty protocol (or
       xterm's modifyOtherKeys) when the connected APPLICATION
       explicitly requests it via its own CSI sequence -- ghostcon has
       no code yet to intercept and track those requests (a real gap,
       left for a future pass), so without setting these explicitly the
       encoder's own out-of-the-box default apparently isn't the plain
       legacy encoding a bare, unaware shell needs. */
    GhosttyKittyKeyFlags kitty_disabled = GHOSTTY_KITTY_KEY_DISABLED;
    ghostty_key_encoder_setopt(input->encoder,
                                GHOSTTY_KEY_ENCODER_OPT_KITTY_FLAGS,
                                &kitty_disabled);
    bool modify_other_keys_off = false;
    ghostty_key_encoder_setopt(input->encoder,
                                GHOSTTY_KEY_ENCODER_OPT_MODIFY_OTHER_KEYS_STATE_2,
                                &modify_other_keys_off);

    return input;

fail:
    ghostcon_input_close(input);
    return NULL;
}

void
ghostcon_input_close(ghostcon_input_t *input)
{
    if (!input)
        return;
    if (input->encoder)
        ghostty_key_encoder_free(input->encoder);
    if (input->xkb_state)
        xkb_state_unref(input->xkb_state);
    if (input->xkb_state_unshifted)
        xkb_state_unref(input->xkb_state_unshifted);
    if (input->xkb_keymap)
        xkb_keymap_unref(input->xkb_keymap);
    if (input->xkb_ctx)
        xkb_context_unref(input->xkb_ctx);
    if (input->li)
        libinput_unref(input->li);
    if (input->udev)
        udev_unref(input->udev);
    free(input);
}

int
ghostcon_input_fd(const ghostcon_input_t *input)
{
    return libinput_get_fd(input->li);
}

void
ghostcon_input_sync_modes(ghostcon_input_t *input, const ghostcon_screen_t *screen)
{
    uint8_t app_cursor = screen->application_cursor;
    ghostty_key_encoder_setopt(input->encoder,
                                GHOSTTY_KEY_ENCODER_OPT_CURSOR_KEY_APPLICATION,
                                &app_cursor);
}

static GhosttyMods
current_mods(struct xkb_state *state)
{
    GhosttyMods mods = 0;
    if (xkb_state_mod_name_is_active(state, XKB_MOD_NAME_SHIFT, XKB_STATE_MODS_EFFECTIVE) > 0)
        mods |= GHOSTTY_MODS_SHIFT;
    if (xkb_state_mod_name_is_active(state, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE) > 0)
        mods |= GHOSTTY_MODS_CTRL;
    if (xkb_state_mod_name_is_active(state, XKB_MOD_NAME_ALT, XKB_STATE_MODS_EFFECTIVE) > 0)
        mods |= GHOSTTY_MODS_ALT;
    if (xkb_state_mod_name_is_active(state, XKB_MOD_NAME_LOGO, XKB_STATE_MODS_EFFECTIVE) > 0)
        mods |= GHOSTTY_MODS_SUPER;
    if (xkb_state_mod_name_is_active(state, XKB_MOD_NAME_CAPS, XKB_STATE_MODS_EFFECTIVE) > 0)
        mods |= GHOSTTY_MODS_CAPS_LOCK;
    if (xkb_state_mod_name_is_active(state, XKB_MOD_NAME_NUM, XKB_STATE_MODS_EFFECTIVE) > 0)
        mods |= GHOSTTY_MODS_NUM_LOCK;
    return mods;
}

/* Ctrl+Alt+F1..F12 is reserved for the kernel's own VT switch handling
   -- libinput reads raw evdev events completely independently of that
   (see core/main.c's own doc comment on the analogous input-leak bug
   for why: this device has no concept of "which reader should get
   this event," every open fd sees every keystroke). Without this
   filter, the SAME physical Ctrl+Alt+Fn press that triggers a real VT
   switch ALSO gets encoded and forwarded here, landing as literal
   escaped text in whatever's running in the shell at that moment --
   found live: switching to a GNOME session with Ctrl+Alt+F6 left
   "^[[17;7~" typed into the login prompt on the VT being switched away
   from. xkb_state_update_key() (already called before this point)
   still runs regardless, so modifier tracking stays correct even
   though the keystroke itself is swallowed here. */
static bool
is_vt_switch_combo(uint32_t evdev_code, GhosttyMods mods)
{
    if (!(mods & GHOSTTY_MODS_CTRL) || !(mods & GHOSTTY_MODS_ALT))
        return false;
    switch (evdev_code) {
    case KEY_F1: case KEY_F2: case KEY_F3: case KEY_F4:
    case KEY_F5: case KEY_F6: case KEY_F7: case KEY_F8:
    case KEY_F9: case KEY_F10: case KEY_F11: case KEY_F12:
        return true;
    default:
        return false;
    }
}

/* Shift+Up/Down/PageUp/PageDown — scrollback view shortcuts, mirroring
   kmscon's grab-scroll-up/-down/grab-page-up/-down defaults. Exact
   Shift-only match (not "Shift held among others"), same specificity
   kmscon's own default grabs use, so e.g. an app that wants Shift+Ctrl+Up
   for something else still gets it forwarded normally. Press-only
   (release is a no-op, same as is_vt_switch_combo's handling doesn't
   need to distinguish press/release since nothing forwards on release
   either once true is returned here). */
static bool
handle_scroll_shortcut(ghostcon_screen_t *screen, uint32_t evdev_code,
                        GhosttyMods mods, GhosttyKeyAction action)
{
    if (mods != GHOSTTY_MODS_SHIFT || action != GHOSTTY_KEY_ACTION_PRESS)
        return false;

    switch (evdev_code) {
    case KEY_UP:       ghostcon_screen_scroll_view(screen, 1); return true;
    case KEY_DOWN:     ghostcon_screen_scroll_view(screen, -1); return true;
    case KEY_PAGEUP:   ghostcon_screen_scroll_view(screen, (int)screen->rows_visible); return true;
    case KEY_PAGEDOWN: ghostcon_screen_scroll_view(screen, -(int)screen->rows_visible); return true;
    default: return false;
    }
}

static bool
handle_keyboard_event(ghostcon_input_t *input, struct libinput_event *ev,
                       ghostcon_transport_t *transport, ghostcon_screen_t *screen)
{
    struct libinput_event_keyboard *kbev = libinput_event_get_keyboard_event(ev);
    uint32_t evdev_code = libinput_event_keyboard_get_key(kbev);
    enum libinput_key_state state = libinput_event_keyboard_get_key_state(kbev);

    /* XKB keycodes are evdev keycodes + 8 (historical X11 offset). */
    xkb_keycode_t xkb_code = evdev_code + 8;

    xkb_state_update_key(input->xkb_state, xkb_code,
                          state == LIBINPUT_KEY_STATE_PRESSED ? XKB_KEY_DOWN : XKB_KEY_UP);

    GhosttyKeyAction action = state == LIBINPUT_KEY_STATE_PRESSED
                                   ? GHOSTTY_KEY_ACTION_PRESS
                                   : GHOSTTY_KEY_ACTION_RELEASE;

    char utf8[32] = {0};
    int utf8_len = 0;
    uint32_t unshifted_cp = 0;
    if (action == GHOSTTY_KEY_ACTION_PRESS) {
        /* xkb_state_key_get_utf8() on input->xkb_state (which has Ctrl
           currently held for a Ctrl+letter combo) has xkbcommon's own
           built-in Ctrl-to-control-byte transformation baked in -- it
           returns the already-transformed control byte (e.g. 0x04 for
           Ctrl+D), not the plain letter. Feeding that into the Ghostty
           key encoder alongside GHOSTTY_MODS_CTRL confuses its legacy
           encoding path (it can't map an already-non-printable byte to
           a plain Ctrl+letter sequence) and it falls back to kitty-style
           CSI-u (e.g. "\x1b[4;5u" for Ctrl+D landing as literal text in
           the shell). kmscon avoids this entirely by working from raw
           keysyms (xkb_state_key_get_syms/get_one_sym in
           input_uxkb.c), which are NOT affected by Ctrl, and letting its
           VTE layer do its own Ctrl mapping -- xkb_state_key_get_one_sym
           + xkb_keysym_to_utf8 mirrors that here: the keysym lookup
           itself is unaffected by Ctrl (Ctrl isn't a shift-level
           modifier), and xkb_keysym_to_utf8() has no special-case Ctrl
           behavior, so the encoder gets the plain letter and does its
           own (correct, verified) Ctrl+letter encoding. */
        xkb_keysym_t sym = xkb_state_key_get_one_sym(input->xkb_state, xkb_code);
        utf8_len = xkb_keysym_to_utf8(sym, utf8, sizeof(utf8));
        if (utf8_len > 0)
            utf8_len--; /* xkb_keysym_to_utf8 counts the trailing NUL */
        if (utf8_len < 0)
            utf8_len = 0;
        unshifted_cp = xkb_state_key_get_utf32(input->xkb_state_unshifted, xkb_code);
    }

    GhosttyMods mods = current_mods(input->xkb_state);

    if (is_vt_switch_combo(evdev_code, mods))
        return true; /* reserved for the kernel's own VT switch, never forward */

    if (handle_scroll_shortcut(screen, evdev_code, mods, action))
        return true; /* consumed locally, never forwarded to the pty */

    char encoded[128];
    size_t written = ghostcon_input_encode_key(input->encoder, action, evdev_code, mods,
                                                utf8, (size_t)utf8_len, unshifted_cp,
                                                encoded, sizeof(encoded));
    if (written == (size_t)-1 || written == 0)
        return true; /* nothing to send (e.g. bare modifier), not an error */

    /* Any other key that actually reaches the shell snaps the view back
       to live -- found live: after scrolling back, typing kept the
       scrolled-back history on screen instead of returning to where the
       new input (and its echo) actually lands, matching every other
       terminal's behavior of "typing means you want to see what you're
       doing". Scroll shortcuts themselves already returned above and
       never reach here. */
    if (screen->view_offset > 0)
        ghostcon_screen_scroll_view(screen, -(int)screen->view_offset);

    ssize_t w = ghostcon_transport_write(transport, (const uint8_t *)encoded, written);
    return w == (ssize_t)written;
}

bool
ghostcon_input_dispatch(ghostcon_input_t *input, ghostcon_transport_t *transport,
                         ghostcon_screen_t *screen)
{
    if (libinput_dispatch(input->li) != 0) {
        fprintf(stderr, "input: libinput_dispatch failed\n");
        return false;
    }

    struct libinput_event *ev;
    while ((ev = libinput_get_event(input->li)) != NULL) {
        enum libinput_event_type type = libinput_event_get_type(ev);

        if (type == LIBINPUT_EVENT_KEYBOARD_KEY) {
            if (!handle_keyboard_event(input, ev, transport, screen)) {
                libinput_event_destroy(ev);
                return false;
            }
        }
        /* Pointer/touchpad/gesture events: observed and discarded for
           now -- mouse-reporting escape sequences are wrap's job per
           PLAN.md's ordered input pipeline, not wired up yet. */

        libinput_event_destroy(ev);
    }
    return true;
}
