/*
 * test_input — unit tests for core/input.c's pure key-mapping/encoding
 * step (ghostcon_input_evdev_to_ghostty_key / ghostcon_input_encode_key),
 * with no libinput/hardware dependency. See input.h's file comment for
 * why this is split out from the live dispatch loop.
 */

#include "ghostcon/core/input.h"

#include <linux/input-event-codes.h>
#include <stdio.h>
#include <string.h>

static int failures = 0;

static void
check_encode(const char *name, const char *out, size_t n,
             const char *expected, size_t expected_len)
{
    if (n != expected_len || memcmp(out, expected, expected_len) != 0) {
        fprintf(stderr, "FAIL: %s: got %zu bytes:", name, n);
        for (size_t i = 0; i < n; i++)
            fprintf(stderr, " %02x", (unsigned char)out[i]);
        fprintf(stderr, ", expected %zu bytes:", expected_len);
        for (size_t i = 0; i < expected_len; i++)
            fprintf(stderr, " %02x", (unsigned char)expected[i]);
        fprintf(stderr, "\n");
        failures++;
        return;
    }
    printf("PASS: encode %s\n", name);
}

static void
check_mapping(uint32_t evdev_code, GhosttyKey expected, const char *name)
{
    GhosttyKey got = ghostcon_input_evdev_to_ghostty_key(evdev_code);
    if (got != expected) {
        fprintf(stderr, "FAIL: mapping %s: got %d, expected %d\n", name, got, expected);
        failures++;
        return;
    }
    printf("PASS: mapping %s\n", name);
}

int
main(void)
{
    check_mapping(KEY_A, GHOSTTY_KEY_A, "KEY_A");
    check_mapping(KEY_UP, GHOSTTY_KEY_ARROW_UP, "KEY_UP");
    check_mapping(KEY_ENTER, GHOSTTY_KEY_ENTER, "KEY_ENTER");
    check_mapping(0xFFFF, GHOSTTY_KEY_UNIDENTIFIED, "unknown code");

    GhosttyKeyEncoder enc;
    if (ghostty_key_encoder_new(NULL, &enc) != GHOSTTY_SUCCESS) {
        fprintf(stderr, "FAIL: ghostty_key_encoder_new\n");
        return 1;
    }

    char out[128];
    size_t n;

    n = ghostcon_input_encode_key(enc, GHOSTTY_KEY_ACTION_PRESS, KEY_A, 0, "a", 1, 'a', out, sizeof(out));
    check_encode("plain 'a'", out, n, "a", 1);

    n = ghostcon_input_encode_key(enc, GHOSTTY_KEY_ACTION_PRESS, KEY_C, GHOSTTY_MODS_CTRL, "", 0, 'c', out, sizeof(out));
    check_encode("Ctrl+C", out, n, "\x03", 1);

    n = ghostcon_input_encode_key(enc, GHOSTTY_KEY_ACTION_PRESS, KEY_ENTER, 0, "\r", 1, '\r', out, sizeof(out));
    check_encode("Enter", out, n, "\x0d", 1);

    n = ghostcon_input_encode_key(enc, GHOSTTY_KEY_ACTION_PRESS, KEY_UP, 0, "", 0, 0, out, sizeof(out));
    check_encode("Arrow Up (normal cursor mode)", out, n, "\x1b[A", 3);

    uint8_t app_cursor = 1;
    ghostty_key_encoder_setopt(enc, GHOSTTY_KEY_ENCODER_OPT_CURSOR_KEY_APPLICATION, &app_cursor);
    n = ghostcon_input_encode_key(enc, GHOSTTY_KEY_ACTION_PRESS, KEY_UP, 0, "", 0, 0, out, sizeof(out));
    check_encode("Arrow Up (application cursor mode)", out, n, "\x1bOA", 3);

    n = ghostcon_input_encode_key(enc, GHOSTTY_KEY_ACTION_RELEASE, KEY_A, 0, "", 0, 0, out, sizeof(out));
    check_encode("Release 'a' (legacy mode reports nothing)", out, n, "", 0);

    ghostty_key_encoder_free(enc);

    /* --- ghostcon_parse_keybinding() --- */
    {
        GhosttyMods mods;
        uint32_t evdev;

        if (ghostcon_parse_keybinding("ctrl+shift+c", &mods, &evdev) &&
            mods == (GHOSTTY_MODS_CTRL | GHOSTTY_MODS_SHIFT) && evdev == KEY_C)
            printf("PASS: parse_keybinding: ctrl+shift+c\n");
        else { fprintf(stderr, "FAIL: parse_keybinding: ctrl+shift+c\n"); failures++; }

        if (ghostcon_parse_keybinding("ctrl+shift+v", &mods, &evdev) &&
            mods == (GHOSTTY_MODS_CTRL | GHOSTTY_MODS_SHIFT) && evdev == KEY_V)
            printf("PASS: parse_keybinding: ctrl+shift+v\n");
        else { fprintf(stderr, "FAIL: parse_keybinding: ctrl+shift+v\n"); failures++; }

        /* Case-insensitive, order-independent. */
        if (ghostcon_parse_keybinding("SHIFT+CTRL+C", &mods, &evdev) &&
            mods == (GHOSTTY_MODS_CTRL | GHOSTTY_MODS_SHIFT) && evdev == KEY_C)
            printf("PASS: parse_keybinding: case-insensitive, reordered mods\n");
        else { fprintf(stderr, "FAIL: parse_keybinding: case-insensitive, reordered mods\n"); failures++; }

        if (ghostcon_parse_keybinding("shift+insert", &mods, &evdev) &&
            mods == GHOSTTY_MODS_SHIFT && evdev == KEY_INSERT)
            printf("PASS: parse_keybinding: named key (insert)\n");
        else { fprintf(stderr, "FAIL: parse_keybinding: named key (insert)\n"); failures++; }

        if (ghostcon_parse_keybinding("a", &mods, &evdev) && mods == 0 && evdev == KEY_A)
            printf("PASS: parse_keybinding: bare key, no modifiers\n");
        else { fprintf(stderr, "FAIL: parse_keybinding: bare key, no modifiers\n"); failures++; }

        if (!ghostcon_parse_keybinding("ctrl+boguskey", &mods, &evdev))
            printf("PASS: parse_keybinding: unrecognized key name rejected\n");
        else { fprintf(stderr, "FAIL: parse_keybinding: unrecognized key name rejected\n"); failures++; }

        if (!ghostcon_parse_keybinding("", &mods, &evdev))
            printf("PASS: parse_keybinding: empty spec rejected\n");
        else { fprintf(stderr, "FAIL: parse_keybinding: empty spec rejected\n"); failures++; }

        if (!ghostcon_parse_keybinding("ctrl+c+v", &mods, &evdev))
            printf("PASS: parse_keybinding: two key tokens rejected\n");
        else { fprintf(stderr, "FAIL: parse_keybinding: two key tokens rejected\n"); failures++; }
    }

    if (failures > 0) {
        fprintf(stderr, "%d test(s) failed\n", failures);
        return 1;
    }
    printf("ALL MAPPING TESTS PASSED (encode output above is for manual inspection)\n");
    return 0;
}
