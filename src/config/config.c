#define _DEFAULT_SOURCE

#include "ghostcon/config/config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <unistd.h>

#include <toml.h>

void
ghostcon_config_defaults(ghostcon_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    /* Matches every binary's own hardcoded fallback today -- see
       core/main.c's DEFAULT_DRM_NODE, supervisor/main.c's
       DEFAULT_RUN_DIR/DEFAULT_CANARY_DEADLINE_MS, core/main.c's
       FONT_SIZE. */
    snprintf(cfg->drm_node, sizeof(cfg->drm_node), "%s", "/dev/dri/card1");
    snprintf(cfg->run_dir, sizeof(cfg->run_dir), "%s", "/run/ghostcon");
    cfg->canary_deadline_ms = 4000;
    cfg->disable_wall = false;
    cfg->disable_kmscon_fallback = false;
    cfg->font_size = 16;
    cfg->clear_on_logout = true;
    cfg->zoom_step = 2;
    cfg->scrollback_lines = 2000;
    cfg->repeat_delay_ms = 250;
    cfg->repeat_rate_ms = 50;
    snprintf(cfg->antialiasing, sizeof(cfg->antialiasing), "%s", "grayscale");
    snprintf(cfg->subpixel_order, sizeof(cfg->subpixel_order), "%s", "rgb");
    cfg->gamma_correct = false; /* see gles.c FRAG_SRC's doc comment: this
                                    correction is only valid paired with true
                                    linear-space blending (an sRGB-format
                                    framebuffer ghostcon doesn't have), which
                                    ghostcon-core lacks -- applied alone it
                                    over-darkens partial-coverage pixels,
                                    visibly dimming thin strokes relative to
                                    thick ones. Off until real linear
                                    blending exists to pair it with. */
    cfg->cursor_base_scale = 1.0f;
    cfg->cursor_scale_with_terminal = true;
    snprintf(cfg->copy_to_clipboard_binding, sizeof(cfg->copy_to_clipboard_binding), "%s", "ctrl+shift+c");
    snprintf(cfg->paste_from_clipboard_binding, sizeof(cfg->paste_from_clipboard_binding), "%s", "ctrl+shift+v");

    cfg->mouse_enable = true;
    cfg->mouse_scroll_speed = 1.0f;
    cfg->mouse_sensitivity = 0.0f;

    cfg->touchpad_enable = true;
    cfg->touchpad_scroll_speed = 1.0f;
    cfg->touchpad_tap_to_click = true;
    cfg->touchpad_natural_scroll = false;
    cfg->touchpad_sensitivity = 0.0f;
}

static void
load_string(toml_table_t *tab, const char *key, char *out, size_t out_len)
{
    toml_datum_t d = toml_string_in(tab, key);
    if (!d.ok)
        return;
    snprintf(out, out_len, "%s", d.u.s);
    free(d.u.s);
}

static void
load_int(toml_table_t *tab, const char *key, int *out)
{
    toml_datum_t d = toml_int_in(tab, key);
    if (d.ok)
        *out = (int)d.u.i;
}

static void
load_bool(toml_table_t *tab, const char *key, bool *out)
{
    toml_datum_t d = toml_bool_in(tab, key);
    if (d.ok)
        *out = d.u.b;
}

static void
load_double(toml_table_t *tab, const char *key, float *out)
{
    toml_datum_t d = toml_double_in(tab, key);
    if (d.ok)
        *out = (float)d.u.d;
}

bool
ghostcon_config_load(const char *path, ghostcon_config_t *cfg)
{
    ghostcon_config_defaults(cfg);

    FILE *fp = fopen(path, "r");
    if (!fp)
        return true; /* missing file -- not an error, pure defaults stand */

    char errbuf[256];
    toml_table_t *root = toml_parse_file(fp, errbuf, sizeof(errbuf));
    fclose(fp);
    if (!root)
        return false; /* file exists but doesn't parse -- a real error */

    toml_table_t *general = toml_table_in(root, "general");
    if (general) {
        load_string(general, "drm_node", cfg->drm_node, sizeof(cfg->drm_node));
        load_string(general, "run_dir", cfg->run_dir, sizeof(cfg->run_dir));
        load_int(general, "canary_deadline_ms", &cfg->canary_deadline_ms);
        load_bool(general, "disable_wall", &cfg->disable_wall);
        load_bool(general, "disable_kmscon_fallback", &cfg->disable_kmscon_fallback);
        load_int(general, "font_size", &cfg->font_size);
        load_string(general, "font_family", cfg->font_family, sizeof(cfg->font_family));
        load_string(general, "font_variant", cfg->font_variant, sizeof(cfg->font_variant));
        load_bool(general, "clear_on_logout", &cfg->clear_on_logout);
        load_int(general, "zoom_step", &cfg->zoom_step);
        load_int(general, "scrollback_lines", &cfg->scrollback_lines);
        load_int(general, "repeat_delay_ms", &cfg->repeat_delay_ms);
        load_int(general, "repeat_rate_ms", &cfg->repeat_rate_ms);
        load_string(general, "antialiasing", cfg->antialiasing, sizeof(cfg->antialiasing));
        load_string(general, "subpixel_order", cfg->subpixel_order, sizeof(cfg->subpixel_order));
        load_bool(general, "gamma_correct", &cfg->gamma_correct);
        load_string(general, "theme", cfg->theme, sizeof(cfg->theme));
    }

    toml_table_t *colors = toml_table_in(root, "colors");
    if (colors) {
        load_string(colors, "background", cfg->color_background, sizeof(cfg->color_background));
        load_string(colors, "foreground", cfg->color_foreground, sizeof(cfg->color_foreground));
        load_string(colors, "cursor", cfg->color_cursor, sizeof(cfg->color_cursor));
        char key[8];
        for (int i = 0; i < 16; i++) {
            snprintf(key, sizeof(key), "color%d", i);
            load_string(colors, key, cfg->color[i], sizeof(cfg->color[i]));
        }
    }

    toml_table_t *cursor = toml_table_in(root, "cursor");
    if (cursor) {
        load_string(cursor, "theme", cfg->cursor_theme, sizeof(cfg->cursor_theme));
        load_string(cursor, "default", cfg->cursor_default_path, sizeof(cfg->cursor_default_path));
        load_string(cursor, "link", cfg->cursor_link_path, sizeof(cfg->cursor_link_path));
        load_double(cursor, "base_scale", &cfg->cursor_base_scale);
        load_bool(cursor, "scale_with_terminal", &cfg->cursor_scale_with_terminal);
        load_string(cursor, "default_hot_pos", cfg->cursor_default_hot_pos, sizeof(cfg->cursor_default_hot_pos));
        load_string(cursor, "link_hot_pos", cfg->cursor_link_hot_pos, sizeof(cfg->cursor_link_hot_pos));
    }

    toml_table_t *keybindings = toml_table_in(root, "keybindings");
    if (keybindings) {
        load_string(keybindings, "copy_to_clipboard", cfg->copy_to_clipboard_binding,
                    sizeof(cfg->copy_to_clipboard_binding));
        load_string(keybindings, "paste_from_clipboard", cfg->paste_from_clipboard_binding,
                    sizeof(cfg->paste_from_clipboard_binding));
    }

    toml_table_t *mouse = toml_table_in(root, "mouse");
    if (mouse) {
        load_bool(mouse, "enable", &cfg->mouse_enable);
        load_double(mouse, "scroll_speed", &cfg->mouse_scroll_speed);
        load_double(mouse, "sensitivity", &cfg->mouse_sensitivity);
    }

    toml_table_t *touchpad = toml_table_in(root, "touchpad");
    if (touchpad) {
        load_bool(touchpad, "enable", &cfg->touchpad_enable);
        load_double(touchpad, "scroll_speed", &cfg->touchpad_scroll_speed);
        load_bool(touchpad, "tap_to_click", &cfg->touchpad_tap_to_click);
        load_bool(touchpad, "natural_scroll", &cfg->touchpad_natural_scroll);
        load_double(touchpad, "sensitivity", &cfg->touchpad_sensitivity);
    }

    toml_free(root);
    return true;
}

void
ghostcon_config_export_env(const ghostcon_config_t *cfg, const char *config_path,
                            bool overwrite)
{
    int ow = overwrite ? 1 : 0;
    char buf[32];

    setenv("GHOSTCON_CONFIG_PATH", config_path, 1); /* always the resolved path, unconditionally */

    setenv("GHOSTCON_DRM_NODE", cfg->drm_node, ow);
    setenv("GHOSTCON_RUN_DIR", cfg->run_dir, ow);

    snprintf(buf, sizeof(buf), "%d", cfg->canary_deadline_ms);
    setenv("GHOSTCON_CANARY_DEADLINE_MS", buf, ow);

    /* Readers check presence only (getenv() truthiness), not the value
       -- "1" means on, ABSENT means off. On a plain startup export
       (overwrite=false) that's already correct by construction: a false
       bool here just never calls setenv, leaving it absent. On a
       hot-reload export (overwrite=true) a bool can flip in either
       direction, so a false value must actively unsetenv() a
       previously-true one, not just skip setenv(). */
    if (cfg->disable_wall)
        setenv("GHOSTCON_DISABLE_WALL", "1", ow);
    else if (overwrite)
        unsetenv("GHOSTCON_DISABLE_WALL");
    if (cfg->disable_kmscon_fallback)
        setenv("GHOSTCON_DISABLE_KMSCON_FALLBACK", "1", ow);
    else if (overwrite)
        unsetenv("GHOSTCON_DISABLE_KMSCON_FALLBACK");

    snprintf(buf, sizeof(buf), "%d", cfg->font_size);
    setenv("GHOSTCON_FONT_SIZE", buf, ow);

    /* Default true (see core/main.c's CLEAR_ON_LOGOUT_DEFAULT), unlike
       every other bool above -- only export "0" when the config
       explicitly turns it OFF, so the reader's own true-default stands
       when this key is left out of the file entirely (the common
       case). Same reload-direction-flip reasoning as above, inverted:
       a hot-reload back to true must unsetenv() a previously-exported
       "0", not leave it stuck off. */
    if (!cfg->clear_on_logout)
        setenv("GHOSTCON_CLEAR_ON_LOGOUT", "0", ow);
    else if (overwrite)
        unsetenv("GHOSTCON_CLEAR_ON_LOGOUT");

    snprintf(buf, sizeof(buf), "%d", cfg->zoom_step);
    setenv("GHOSTCON_ZOOM_STEP", buf, ow);
}

int
ghostcon_config_watch_open(const char *path)
{
    char dir[512];
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash)
        *slash = '\0';
    else
        snprintf(dir, sizeof(dir), "."); /* no directory component -- watch cwd */

    int fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (fd < 0)
        return -1;

    /* IN_CLOSE_WRITE covers a plain in-place write; IN_MOVED_TO covers
       the write-temp-then-rename-over-original pattern most editors
       (vim included) use by default -- watching the directory rather
       than the file itself is what makes that second case observable
       at all (see this function's own header doc comment). IN_MODIFY
       is a cheap extra safety net for tools that do neither. */
    if (inotify_add_watch(fd, dir, IN_CLOSE_WRITE | IN_MOVED_TO | IN_MODIFY) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

bool
ghostcon_config_watch_check(int inotify_fd, const char *path)
{
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;

    bool matched = false;
    /* Aligned so the buffer can be reinterpreted as a sequence of
       variable-length struct inotify_event records, per inotify(7). */
    union {
        char buf[4096];
        struct inotify_event align;
    } u;

    for (;;) {
        ssize_t n = read(inotify_fd, u.buf, sizeof(u.buf));
        if (n <= 0)
            break; /* EAGAIN (nothing more pending) or a real error -- either way, done */

        for (char *p = u.buf; p < u.buf + n; ) {
            struct inotify_event *ev = (struct inotify_event *)(void *)p;
            if (ev->len > 0 && strcmp(ev->name, base) == 0)
                matched = true;
            p += sizeof(struct inotify_event) + ev->len;
        }
    }
    return matched;
}
