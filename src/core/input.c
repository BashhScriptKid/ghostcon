#define _DEFAULT_SOURCE /* CLOCK_MONOTONIC under -std=c11 without this */

#include "ghostcon/core/input.h"
#include "ghostcon/term/base64.h"
#include "ghostcon/term/mouse.h"
#include "ghostcon/term/selection.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <libinput.h>
#include <libudev.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/timerfd.h>
#include <time.h>
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

    /* Key auto-repeat -- libinput itself never generates repeat events
       (that's userspace's job); mirrors kmscon's own approach
       (input_uxkb.c: xkb_keymap_key_repeats() gates which keys repeat
       at all -- modifiers are excluded by the keymap itself, no manual
       blocklist needed -- plus a timer that resends the same encoded
       bytes the initial press produced). repeat_fd < 0 means repeat is
       unavailable (timerfd_create failed) -- not fatal, matches this
       module's existing tolerance for the ctl socket/etc being absent. */
    int      repeat_fd;
    bool     repeating;
    uint32_t repeat_evdev_code;
    char     repeat_encoded[128];
    size_t   repeat_len;

    /* Pointer -- absolute pixel position (mice/touchpads both report
       RELATIVE motion via libinput, see handle_pointer_event()'s own
       doc comment; this is the position that relative motion
       accumulates into), clamped to [0,viewport_w) x [0,viewport_h).
       pressed_buttons is a bitmask of which of GC_MOUSE_LEFT/MIDDLE/
       RIGHT are currently held, needed for mode-1002 drag-motion
       filtering (term/mouse.c's ghostcon_mouse_encode()) and to know
       what button a MOTION event should report as "held". */
    int      viewport_w, viewport_h;
    int      pointer_x, pointer_y;
    uint32_t pressed_buttons; /* bit i set = (ghostcon_mouse_button_t)i is held */

    /* Whether the button/drag currently held down was intercepted for
       LOCAL selection (should_intercept_for_selection() returned true
       when it was pressed) rather than sent to the app as a mouse
       report -- decided once, at press time, and remembered until
       release, so a drag can't switch modes mid-gesture if e.g. the
       app toggles mouse-reporting mid-drag (pathological, but cheap to
       guard against). */
    bool     left_button_intercepted;

    /* Configurable copy/paste shortcuts (config.h's [keybindings]
       table, parsed once via ghostcon_parse_keybinding() and pushed in
       via ghostcon_input_set_clipboard_bindings() -- see that
       function's own doc comment for why this is a mutable setter
       rather than an ghostcon_input_open() parameter). Defaulted here
       to this project's own defaults (ctrl+shift+c/v) so copy/paste
       already work correctly even before the first config (re)load
       completes, matching every other config-driven field's "sane
       default until config says otherwise" convention. */
    GhosttyMods copy_mods, paste_mods;
    uint32_t    copy_evdev, paste_evdev;
};

/* kmscon's own documented defaults (xkb-repeat-delay/xkb-repeat-rate) --
   not made configurable yet, same "don't build config plumbing nobody
   asked for" scoping as everything else added this session. */
#define GHOSTCON_REPEAT_DELAY_MS 250
#define GHOSTCON_REPEAT_RATE_MS 50

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
ghostcon_input_open(const char *seat_id, int viewport_w, int viewport_h)
{
    ghostcon_input_t *input = calloc(1, sizeof(*input));
    if (!input)
        return NULL;
    input->repeat_fd = -1; /* calloc() zeroes to fd 0, a real (if unlikely) fd */
    input->viewport_w = viewport_w;
    input->viewport_h = viewport_h;
    input->pointer_x = viewport_w / 2;
    input->pointer_y = viewport_h / 2;

    /* Sane defaults until config.c's [keybindings] table pushes real
       values via ghostcon_input_set_clipboard_bindings() -- these
       calls can't fail (both specs are this project's own hardcoded
       defaults, always valid). */
    ghostcon_parse_keybinding("ctrl+shift+c", &input->copy_mods, &input->copy_evdev);
    ghostcon_parse_keybinding("ctrl+shift+v", &input->paste_mods, &input->paste_evdev);

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

    /* Explicit legacy-mode defaults for this fresh encoder -- found
       live: Ctrl+D (and by extension every other Ctrl+letter combo)
       was being encoded as kitty keyboard protocol CSI-u sequences
       (e.g. "\x1b[4;5u") instead of the plain single control byte
       (0x04) any ordinary shell expects, landing as literal garbage
       text instead of being interpreted -- the encoder's own out-of-
       the-box default apparently isn't the plain legacy encoding a
       bare, unaware shell needs. Just the STARTING point now, not
       permanent: ghostcon_input_sync_modes() (called every dispatch)
       re-syncs GHOSTTY_KEY_ENCODER_OPT_KITTY_FLAGS from
       screen->kitty's live, connected-app-negotiated state (see that
       function's own doc comment) -- this only matters for the brief
       window before the first sync_modes() call, or if an app never
       requests the protocol at all (screen->kitty then stays at its
       own init default of 0/disabled anyway, so this and that agree).
       GHOSTTY_KEY_ENCODER_OPT_MODIFY_OTHER_KEYS_STATE_2 (xterm's
       older, separate mechanism for the same class of problem) has no
       tracked live state anywhere in this tree yet, so it stays
       permanently forced off here -- a smaller, still-open version of
       the same gap the kitty flags used to have. */
    GhosttyKittyKeyFlags kitty_disabled = GHOSTTY_KITTY_KEY_DISABLED;
    ghostty_key_encoder_setopt(input->encoder,
                                GHOSTTY_KEY_ENCODER_OPT_KITTY_FLAGS,
                                &kitty_disabled);
    bool modify_other_keys_off = false;
    ghostty_key_encoder_setopt(input->encoder,
                                GHOSTTY_KEY_ENCODER_OPT_MODIFY_OTHER_KEYS_STATE_2,
                                &modify_other_keys_off);

    input->repeat_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (input->repeat_fd < 0)
        fprintf(stderr, "input: timerfd_create failed, key auto-repeat disabled: %s\n",
                strerror(errno));

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
    if (input->repeat_fd >= 0)
        close(input->repeat_fd);
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

int
ghostcon_input_repeat_fd(const ghostcon_input_t *input)
{
    return input->repeat_fd;
}

bool
ghostcon_input_repeat_fire(ghostcon_input_t *input, ghostcon_transport_t *transport)
{
    uint64_t expirations;
    ssize_t r = read(input->repeat_fd, &expirations, sizeof(expirations));
    (void)r; /* EAGAIN if the timer fired zero times since last drain; nothing to do either way */

    if (!input->repeating)
        return true;

    ssize_t w = ghostcon_transport_write(transport,
                                          (const uint8_t *)input->repeat_encoded,
                                          input->repeat_len);
    return w == (ssize_t)input->repeat_len;
}

void
ghostcon_input_sync_modes(ghostcon_input_t *input, const ghostcon_screen_t *screen)
{
    uint8_t app_cursor = screen->application_cursor;
    ghostty_key_encoder_setopt(input->encoder,
                                GHOSTTY_KEY_ENCODER_OPT_CURSOR_KEY_APPLICATION,
                                &app_cursor);

    /* Live Kitty keyboard protocol negotiation -- previously
       hardcoded to GHOSTTY_KITTY_KEY_DISABLED permanently at
       ghostcon_input_open() time (see that function's own doc comment
       on why: a real live bug where an unaware shell received CSI-u
       sequences it couldn't parse). term/stream.c now actually tracks
       what a connected app requests via its own CSI >/</=/? u
       sequences (screen->kitty, previously write-only dead state --
       nothing ever read it back), so this can finally honor that
       instead of always forcing legacy mode -- GhosttyKittyKeyFlags
       is bit-for-bit identical to this project's own GC_KITTY_* flags
       (both DISAMBIGUATE=bit0/REPORT_EVENTS=bit1/.../REPORT_ASSOCIATED
       =bit4), so no translation is needed, just a direct pass-through.
       An app that never asked for Kitty protocol still gets flags=0
       (GHOSTTY_KITTY_KEY_DISABLED) here, i.e. legacy encoding by
       default -- unchanged behavior for the common case. */
    GhosttyKittyKeyFlags kitty_flags = ghostcon_kitty_current(&screen->kitty);
    ghostty_key_encoder_setopt(input->encoder,
                                GHOSTTY_KEY_ENCODER_OPT_KITTY_FLAGS,
                                &kitty_flags);
}

void
ghostcon_input_set_clipboard_bindings(ghostcon_input_t *input,
                                       GhosttyMods copy_mods, uint32_t copy_evdev,
                                       GhosttyMods paste_mods, uint32_t paste_evdev)
{
    input->copy_mods = copy_mods;
    input->copy_evdev = copy_evdev;
    input->paste_mods = paste_mods;
    input->paste_evdev = paste_evdev;
}

/* Named keys beyond plain letters -- deliberately small, only what's
   plausible to rebind copy/paste to; not Ghostty's full W3C key table
   (see ghostcon_parse_keybinding()'s own doc comment on scope). */
static const struct { const char *name; uint32_t evdev; } KEYBINDING_NAMED_KEYS[] = {
    { "insert", KEY_INSERT }, { "space", KEY_SPACE }, { "tab", KEY_TAB },
    { "enter", KEY_ENTER }, { "return", KEY_ENTER }, { "escape", KEY_ESC },
    { "comma", KEY_COMMA }, { "period", KEY_DOT }, { "slash", KEY_SLASH },
    { "semicolon", KEY_SEMICOLON },
};

bool
ghostcon_parse_keybinding(const char *spec, GhosttyMods *out_mods, uint32_t *out_evdev_code)
{
    if (!spec || !*spec)
        return false;

    GhosttyMods mods = 0;
    uint32_t evdev = 0;
    bool have_key = false;

    char buf[64];
    snprintf(buf, sizeof(buf), "%s", spec);

    char *saveptr = NULL;
    for (char *tok = strtok_r(buf, "+", &saveptr); tok; tok = strtok_r(NULL, "+", &saveptr)) {
        for (char *p = tok; *p; p++)
            *p = (char)tolower((unsigned char)*p);

        if (strcmp(tok, "ctrl") == 0) { mods |= GHOSTTY_MODS_CTRL; continue; }
        if (strcmp(tok, "shift") == 0) { mods |= GHOSTTY_MODS_SHIFT; continue; }
        if (strcmp(tok, "alt") == 0) { mods |= GHOSTTY_MODS_ALT; continue; }
        if (strcmp(tok, "super") == 0) { mods |= GHOSTTY_MODS_SUPER; continue; }

        if (have_key)
            return false; /* only one key token allowed, and it must be last */

        if (strlen(tok) == 1 && tok[0] >= 'a' && tok[0] <= 'z') {
            /* evdev codes follow physical QWERTY layout, NOT alphabetical
               order (e.g. KEY_A=30, KEY_Z=44, KEY_C=46 -- nowhere near
               contiguous), so this needs an explicit table, not
               KEY_A + offset. */
            static const uint32_t LETTER_EVDEV[26] = {
                KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G, KEY_H, KEY_I,
                KEY_J, KEY_K, KEY_L, KEY_M, KEY_N, KEY_O, KEY_P, KEY_Q, KEY_R,
                KEY_S, KEY_T, KEY_U, KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z,
            };
            evdev = LETTER_EVDEV[tok[0] - 'a'];
            have_key = true;
            continue;
        }
        if (strlen(tok) == 1 && tok[0] >= '0' && tok[0] <= '9') {
            static const uint32_t DIGIT_EVDEV[10] = {
                KEY_0, KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9,
            };
            evdev = DIGIT_EVDEV[tok[0] - '0'];
            have_key = true;
            continue;
        }

        bool matched = false;
        for (size_t i = 0; i < sizeof(KEYBINDING_NAMED_KEYS) / sizeof(KEYBINDING_NAMED_KEYS[0]); i++) {
            if (strcmp(tok, KEYBINDING_NAMED_KEYS[i].name) == 0) {
                evdev = KEYBINDING_NAMED_KEYS[i].evdev;
                have_key = true;
                matched = true;
                break;
            }
        }
        if (!matched)
            return false; /* unrecognized token */
    }

    if (!have_key)
        return false;

    *out_mods = mods;
    *out_evdev_code = evdev;
    return true;
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

/* Ctrl+=/Ctrl+Minus -- zoom shortcuts, mirroring kmscon's grab-zoom-in/
   grab-zoom-out defaults (kmscon documents them as <Ctrl>Plus/<Ctrl>Minus;
   bound here to the unshifted '='/'-' keys instead, matching how most
   terminals/browsers do Ctrl+= to zoom in without requiring Shift too).
   KEY_KPPLUS/KEY_KPMINUS are aliases for the numpad. This function has
   no font/atlas ownership (that's core/main.c's app_t) -- it just
   accumulates the requested DIRECTION (+-1 "one zoom tick", not a point
   count) into *zoom_delta; the caller multiplies by its own
   config-driven step size (app->zoom_step) when actually applying it,
   since input.c has no config access and shouldn't need any just for
   this. Press-only, same reasoning as handle_scroll_shortcut. */
static bool
handle_zoom_shortcut(int *zoom_delta, uint32_t evdev_code, GhosttyMods mods,
                      GhosttyKeyAction action)
{
    if (mods != GHOSTTY_MODS_CTRL || action != GHOSTTY_KEY_ACTION_PRESS)
        return false;

    switch (evdev_code) {
    case KEY_EQUAL:
    case KEY_KPPLUS:
        *zoom_delta += 1;
        return true;
    case KEY_MINUS:
    case KEY_KPMINUS:
        *zoom_delta -= 1;
        return true;
    default:
        return false;
    }
}

/* Forward declarations -- defined later in this file (near
   send_mouse_report()), but needed here since handle_keyboard_event()
   comes first. */
static void copy_selection_to_clipboard(ghostcon_screen_t *screen);
static void paste_clipboard_to_pty(ghostcon_screen_t *screen, ghostcon_transport_t *transport);

static bool
handle_keyboard_event(ghostcon_input_t *input, struct libinput_event *ev,
                       ghostcon_transport_t *transport, ghostcon_screen_t *screen,
                       int *zoom_delta)
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

    /* Releasing the key currently auto-repeating stops it -- checked
       unconditionally here (not folded into the write-path arm logic
       below) since a release often encodes to zero bytes in legacy
       mode and would otherwise never reach that code. */
    if (action == GHOSTTY_KEY_ACTION_RELEASE && input->repeating &&
        input->repeat_evdev_code == evdev_code) {
        input->repeating = false;
        if (input->repeat_fd >= 0)
            timerfd_settime(input->repeat_fd, 0, &(struct itimerspec){0}, NULL);
    }

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

    if (handle_zoom_shortcut(zoom_delta, evdev_code, mods, action))
        return true; /* consumed locally, never forwarded to the pty */

    /* Configurable copy/paste shortcuts ([keybindings] in ghostcon.toml,
       parsed via ghostcon_parse_keybinding(), defaulting to
       ctrl+shift+c/v -- Ghostty's own Linux defaults, matched
       deliberately since ghostcon already wraps libghostty-vt). Exact
       mods match (not "held among others"), same specificity as the
       scroll/zoom shortcuts above. Press-only. */
    if (mods == input->copy_mods && evdev_code == input->copy_evdev &&
        action == GHOSTTY_KEY_ACTION_PRESS) {
        copy_selection_to_clipboard(screen);
        return true;
    }
    if (mods == input->paste_mods && evdev_code == input->paste_evdev &&
        action == GHOSTTY_KEY_ACTION_PRESS) {
        paste_clipboard_to_pty(screen, transport);
        return true;
    }

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
    if (w != (ssize_t)written)
        return false;

    /* Arm/restart auto-repeat for this press if the active keymap says
       this key should repeat (modifiers etc. are excluded by the
       keymap itself). timerfd_settime() unconditionally replaces
       whatever the timer was previously doing, so a different key
       pressed while one was already repeating naturally takes over --
       no separate "stop the old one first" step needed. */
    if (action == GHOSTTY_KEY_ACTION_PRESS && input->repeat_fd >= 0 &&
        xkb_keymap_key_repeats(input->xkb_keymap, xkb_code)) {
        input->repeating = true;
        input->repeat_evdev_code = evdev_code;
        memcpy(input->repeat_encoded, encoded, written);
        input->repeat_len = written;

        struct itimerspec spec = {
            .it_value    = { .tv_sec = GHOSTCON_REPEAT_DELAY_MS / 1000,
                              .tv_nsec = (long)(GHOSTCON_REPEAT_DELAY_MS % 1000) * 1000000L },
            .it_interval = { .tv_sec = GHOSTCON_REPEAT_RATE_MS / 1000,
                              .tv_nsec = (long)(GHOSTCON_REPEAT_RATE_MS % 1000) * 1000000L },
        };
        timerfd_settime(input->repeat_fd, 0, &spec, NULL);
    }

    return true;
}

/* Maps an evdev button code (BTN_LEFT/BTN_RIGHT/BTN_MIDDLE, as
   returned by libinput_event_pointer_get_button()) to our button enum.
   Returns false for anything else (side/extra buttons, etc.) -- not
   handled in this pass, event is a silent no-op rather than an error. */
static bool
evdev_button_to_mouse(uint32_t code, ghostcon_mouse_button_t *out)
{
    switch (code) {
    case BTN_LEFT:   *out = GC_MOUSE_LEFT;   return true;
    case BTN_MIDDLE: *out = GC_MOUSE_MIDDLE; return true;
    case BTN_RIGHT:  *out = GC_MOUSE_RIGHT;  return true;
    default:         return false;
    }
}

/* Encodes and sends one mouse report (if term/mouse.c's
   ghostcon_mouse_encode() decides this event should be reported at
   all, per screen's negotiated protocol) using input's current
   pointer_x/y. Returns false only on a transport write failure. */
static bool
send_mouse_report(ghostcon_input_t *input, ghostcon_transport_t *transport,
                   ghostcon_screen_t *screen, int cell_w, int cell_h,
                   ghostcon_mouse_button_t button, ghostcon_mouse_action_t action)
{
    int col = cell_w > 0 ? input->pointer_x / cell_w : 0;
    int row = cell_h > 0 ? input->pointer_y / cell_h : 0;

    char encoded[32];
    size_t written = ghostcon_mouse_encode(screen, button, action, current_mods(input->xkb_state),
                                            col, row, encoded, sizeof(encoded));
    if (written == 0)
        return true; /* not reportable in the current mode -- not an error */

    ssize_t w = ghostcon_transport_write(transport, (const uint8_t *)encoded, written);
    return w == (ssize_t)written;
}

/* Decides whether a click should be intercepted for LOCAL selection
   (or middle-click paste) rather than forwarded to the app as a mouse
   report -- mirrors xterm's own "shift bypasses an app's mouse grab"
   convention: local wins if the app hasn't asked for mouse tracking at
   all, or if Shift is held and the app hasn't set XTSHIFTESCAPE
   (mouse_shift_capture) to ask for shift-clicks too. */
static bool
should_intercept_for_selection(const ghostcon_screen_t *screen, GhosttyMods mods)
{
    if (!screen->mouse_tracking)
        return true;
    if ((mods & GHOSTTY_MODS_SHIFT) && !screen->mouse_shift_capture)
        return true;
    return false;
}

/* Extracts the active selection as plain text and stores it, base64-
   encoded, into screen->clipboard -- the same buffer OSC 52 already
   uses, kept to its "always holds base64" convention regardless of
   which path wrote it. A no-op if there's no active selection. Scratch
   buffer sized for base64's ~4/3 expansion (clipboard's 4096 bytes can
   only hold ~3072 raw bytes once encoded), not sizeof(screen->clipboard)
   itself -- using the full 4096 there would silently truncate. */
static void
copy_selection_to_clipboard(ghostcon_screen_t *screen)
{
    char text[3072];
    size_t n = ghostcon_selection_extract_text(screen, text, sizeof(text));
    if (n == 0)
        return;
    ghostcon_base64_encode((const uint8_t *)text, n, screen->clipboard, sizeof(screen->clipboard));
}

/* Decodes screen->clipboard (base64, same buffer OSC 52 reads/writes)
   and writes the raw bytes to the pty, wrapped in bracketed-paste
   markers if the app has enabled mode 2004 -- skipping that wrap would
   visibly break paste in any bracketed-paste-aware program (vim,
   readline), since they'd have no way to tell pasted text apart from
   typed keystrokes. Shared by the configurable paste shortcut and
   middle-click paste. */
static void
paste_clipboard_to_pty(ghostcon_screen_t *screen, ghostcon_transport_t *transport)
{
    uint8_t decoded[sizeof(screen->clipboard)];
    size_t n = ghostcon_base64_decode(screen->clipboard, decoded, sizeof(decoded));
    if (n == 0)
        return;

    if (screen->bracketed_paste)
        ghostcon_transport_write(transport, (const uint8_t *)"\x1b[200~", 6);
    ghostcon_transport_write(transport, decoded, n);
    if (screen->bracketed_paste)
        ghostcon_transport_write(transport, (const uint8_t *)"\x1b[201~", 6);
}

/* Pointer capture -- mice AND touchpads alike. libinput's own device
   model groups mice/trackballs/touchpads under one
   LIBINPUT_DEVICE_CAP_POINTER capability, and touchpad relative
   dragging is delivered as the exact same LIBINPUT_EVENT_POINTER_MOTION
   (relative dx/dy) a mouse produces -- MOTION_ABSOLUTE is for
   genuinely absolute-positioning devices (touchscreens/tablets), not
   touchpads used as a relative pointer. kmscon, by contrast, hand-
   rolls touchpad support at the raw evdev level (manual ABS_X/ABS_Y
   offset-tracking per touchpad in its own input_pointer.c) because it
   doesn't route pointer input through libinput at all -- verified
   against that source before writing this; nothing device-type-
   specific is needed here, both flow through the same already-open
   libinput context ghostcon_input_open() uses for keyboard. Returns
   false only on a transport write failure. */
static bool
handle_pointer_event(ghostcon_input_t *input, struct libinput_event *ev,
                      enum libinput_event_type type, ghostcon_transport_t *transport,
                      ghostcon_screen_t *screen, int cell_w, int cell_h,
                      ghostcon_input_pointer_t *out_pointer)
{
    struct libinput_event_pointer *p = libinput_event_get_pointer_event(ev);

    switch (type) {
    case LIBINPUT_EVENT_POINTER_MOTION:
        input->pointer_x += (int)libinput_event_pointer_get_dx(p);
        input->pointer_y += (int)libinput_event_pointer_get_dy(p);
        break;
    case LIBINPUT_EVENT_POINTER_MOTION_ABSOLUTE:
        input->pointer_x = (int)libinput_event_pointer_get_absolute_x_transformed(
            p, (uint32_t)input->viewport_w);
        input->pointer_y = (int)libinput_event_pointer_get_absolute_y_transformed(
            p, (uint32_t)input->viewport_h);
        break;
    case LIBINPUT_EVENT_POINTER_BUTTON: {
        ghostcon_mouse_button_t button;
        if (!evdev_button_to_mouse(libinput_event_pointer_get_button(p), &button))
            return true; /* side/extra button we don't map -- silent no-op */
        bool pressed = libinput_event_pointer_get_button_state(p) == LIBINPUT_BUTTON_STATE_PRESSED;

        /* Middle-click paste (X11 convention) -- only when this click
           would otherwise be intercepted for local selection; if the
           app has grabbed mouse reporting and Shift isn't overriding
           it, middle-click reports to the app like any other button,
           same as before this feature existed. */
        if (button == GC_MOUSE_MIDDLE && pressed &&
            should_intercept_for_selection(screen, current_mods(input->xkb_state))) {
            paste_clipboard_to_pty(screen, transport);
            return true;
        }

        /* The intercept decision is made ONCE, at press time, and
           remembered until release -- so a drag can't switch between
           local selection and app-reported mid-gesture if e.g. the
           app toggles mouse-reporting mid-drag (pathological, but
           cheap to guard against; see the struct field's own doc
           comment). */
        if (button == GC_MOUSE_LEFT && pressed)
            input->left_button_intercepted =
                should_intercept_for_selection(screen, current_mods(input->xkb_state));

        if (button == GC_MOUSE_LEFT && input->left_button_intercepted) {
            if (pressed)
                input->pressed_buttons |= (1u << button);
            else
                input->pressed_buttons &= ~(1u << button);
            out_pointer->left_pressed = pressed;
            out_pointer->left_released = !pressed;
            return true; /* swallowed locally, never forwarded to the pty */
        }

        if (pressed)
            input->pressed_buttons |= (1u << button);
        else
            input->pressed_buttons &= ~(1u << button);
        return send_mouse_report(input, transport, screen, cell_w, cell_h, button,
                                  pressed ? GC_MOUSE_PRESS : GC_MOUSE_RELEASE);
    }
    case LIBINPUT_EVENT_POINTER_SCROLL_WHEEL:
    case LIBINPUT_EVENT_POINTER_SCROLL_FINGER: {
        /* v120 for a real wheel (per-detent already discretized by
           libinput); plain scroll_value for touchpad two-finger scroll
           (a continuous pixel-ish distance) -- it is an application
           bug to call get_scroll_value_v120() on a non-wheel event
           (per libinput.h's own doc comment), hence the split here.
           Not accumulated/thresholded for finger-scroll (a possible
           follow-up if it fires too eagerly in practice) -- any
           nonzero value on the vertical axis fires one tick, matching
           the wheel case exactly for simplicity in this pass. */
        if (!libinput_event_pointer_has_axis(p, LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL))
            return true;
        double value = (type == LIBINPUT_EVENT_POINTER_SCROLL_WHEEL)
                            ? libinput_event_pointer_get_scroll_value_v120(p, LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL)
                            : libinput_event_pointer_get_scroll_value(p, LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL);
        if (value == 0.0)
            return true;
        ghostcon_mouse_button_t wheel = (value > 0) ? GC_MOUSE_WHEEL_DOWN : GC_MOUSE_WHEEL_UP;
        return send_mouse_report(input, transport, screen, cell_w, cell_h, wheel, GC_MOUSE_PRESS);
    }
    default:
        return true; /* other pointer/gesture event types: not handled in this pass */
    }

    if (input->pointer_x < 0) input->pointer_x = 0;
    if (input->pointer_y < 0) input->pointer_y = 0;
    if (input->pointer_x >= input->viewport_w) input->pointer_x = input->viewport_w - 1;
    if (input->pointer_y >= input->viewport_h) input->pointer_y = input->viewport_h - 1;

    out_pointer->x = input->pointer_x;
    out_pointer->y = input->pointer_y;
    out_pointer->moved = true;

    /* A drag currently intercepted for local selection must not ALSO
       report motion to the app -- out_pointer->moved above already
       lets main.c drive ghostcon_selection_update() from this event;
       reporting it to the pty too would leak drag motion into a mouse-
       aware app for a click that was, by definition, kept local (see
       should_intercept_for_selection()). */
    if (input->left_button_intercepted && (input->pressed_buttons & (1u << GC_MOUSE_LEFT)))
        return true;

    ghostcon_mouse_button_t held = GC_MOUSE_NONE;
    for (int b = 0; b < 3; b++) {
        if (input->pressed_buttons & (1u << b)) {
            held = (ghostcon_mouse_button_t)b;
            break;
        }
    }
    return send_mouse_report(input, transport, screen, cell_w, cell_h, held, GC_MOUSE_MOTION);
}

bool
ghostcon_input_dispatch(ghostcon_input_t *input, ghostcon_transport_t *transport,
                         ghostcon_screen_t *screen, int cell_w, int cell_h,
                         int *out_zoom_delta, ghostcon_input_pointer_t *out_pointer)
{
    out_pointer->x = input->pointer_x;
    out_pointer->y = input->pointer_y;
    out_pointer->moved = false;
    out_pointer->left_pressed = false;
    out_pointer->left_released = false;

    if (libinput_dispatch(input->li) != 0) {
        fprintf(stderr, "input: libinput_dispatch failed\n");
        return false;
    }

    struct libinput_event *ev;
    while ((ev = libinput_get_event(input->li)) != NULL) {
        enum libinput_event_type type = libinput_event_get_type(ev);

        if (type == LIBINPUT_EVENT_KEYBOARD_KEY) {
            if (!handle_keyboard_event(input, ev, transport, screen, out_zoom_delta)) {
                libinput_event_destroy(ev);
                return false;
            }
        } else if (type == LIBINPUT_EVENT_POINTER_MOTION ||
                   type == LIBINPUT_EVENT_POINTER_MOTION_ABSOLUTE ||
                   type == LIBINPUT_EVENT_POINTER_BUTTON ||
                   type == LIBINPUT_EVENT_POINTER_SCROLL_WHEEL ||
                   type == LIBINPUT_EVENT_POINTER_SCROLL_FINGER) {
            if (!handle_pointer_event(input, ev, type, transport, screen, cell_w, cell_h, out_pointer)) {
                libinput_event_destroy(ev);
                return false;
            }
        }
        /* Other pointer/gesture event types (LIBINPUT_EVENT_POINTER_AXIS
           -- deprecated/legacy, superseded by SCROLL_WHEEL/FINGER above;
           tablet/gesture events): observed and discarded, not handled
           in this pass. */

        libinput_event_destroy(ev);
    }
    return true;
}
