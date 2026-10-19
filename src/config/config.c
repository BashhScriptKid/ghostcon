#define _DEFAULT_SOURCE

#include "ghostcon/config/config.h"

#include <stddef.h>
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
     * core/main.c's DEFAULT_DRM_NODE, supervisor/main.c's
     * DEFAULT_RUN_DIR/DEFAULT_CANARY_DEADLINE_MS, core/main.c's FONT_SIZE.
     */
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
    cfg->ligatures = false; /* opt-in -- see config.h's own doc comment */
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

/// HELPER FUNCTIONS FOR TOML LOADING
static void
load_string(toml_table_t *tab, const char *key, char *out, size_t out_len)
{
    toml_datum_t d = toml_string_in(tab, key);
    if (!d.ok)
        return;
    snprintf(out, out_len, "%s", d.u.s);
    free(d.u.s);
}

/* Table-driven field loading
 *
 * Adding a config key is one row in the relevant table below, not a new
 * load_*() call in ghostcon_config_load(). The table stores the field's
 * type, TOML key, and struct byte offset; load_fields() walks it.
 * Strings also carry their destination capacity.
 */
typedef enum {
    CFG_FIELD_STRING,
    CFG_FIELD_INT,
    CFG_FIELD_BOOL,
    CFG_FIELD_FLOAT,
} cfg_field_type_t;

typedef struct {
    cfg_field_type_t type;
    const char *key;
    size_t offset;
    size_t size; /* capacity in bytes; CFG_FIELD_STRING only */
} cfg_field_t;

#define CFG_STRING(key, member) \
    { CFG_FIELD_STRING, (key), offsetof(ghostcon_config_t, member), \
      sizeof(((ghostcon_config_t *)0)->member) }
#define CFG_INT(key, member) \
    { CFG_FIELD_INT, (key), offsetof(ghostcon_config_t, member), 0 }
#define CFG_BOOL(key, member) \
    { CFG_FIELD_BOOL, (key), offsetof(ghostcon_config_t, member), 0 }
#define CFG_FLOAT(key, member) \
    { CFG_FIELD_FLOAT, (key), offsetof(ghostcon_config_t, member), 0 }
#define CFG_FIELD_COUNT(fields) (sizeof(fields) / sizeof((fields)[0]))

/* Unknown/wrong-typed TOML values are silently skipped (patch semantics:
 * a bad key leaves that field at its default), matching this loader's
 * pre-table behavior.
 */
static void
load_field(toml_table_t *tab, const cfg_field_t *field, ghostcon_config_t *cfg)
{
    void *dst = (char *)cfg + field->offset;

    switch (field->type) {
    case CFG_FIELD_STRING: {
        toml_datum_t d = toml_string_in(tab, field->key);
        if (d.ok) {
            snprintf((char *)dst, field->size, "%s", d.u.s);
            free(d.u.s);
        }
        break;
    }
    case CFG_FIELD_INT: {
        toml_datum_t d = toml_int_in(tab, field->key);
        if (d.ok)
            *(int *)dst = (int)d.u.i;
        break;
    }
    case CFG_FIELD_BOOL: {
        toml_datum_t d = toml_bool_in(tab, field->key);
        if (d.ok)
            *(bool *)dst = d.u.b;
        break;
    }
    case CFG_FIELD_FLOAT: {
        toml_datum_t d = toml_double_in(tab, field->key);
        if (d.ok)
            *(float *)dst = (float)d.u.d;
        break;
    }
    }
}

static void
load_fields(toml_table_t *tab, const cfg_field_t *fields, size_t count,
            ghostcon_config_t *cfg)
{
    for (size_t i = 0; i < count; i++)
        load_field(tab, &fields[i], cfg);
}

static const cfg_field_t GENERAL_FIELDS[] = {
    CFG_STRING("drm_node",              drm_node),
    CFG_STRING("run_dir",               run_dir),
    CFG_INT   ("canary_deadline_ms",    canary_deadline_ms),
    CFG_BOOL  ("disable_wall",          disable_wall),
    CFG_BOOL  ("disable_kmscon_fallback", disable_kmscon_fallback),
    CFG_INT   ("font_size",             font_size),
    CFG_STRING("font_family",           font_family),
    CFG_STRING("font_variant",          font_variant),
    CFG_BOOL  ("clear_on_logout",       clear_on_logout),
    CFG_INT   ("zoom_step",             zoom_step),
    CFG_INT   ("repeat_rate_ms",        repeat_rate_ms),
    CFG_INT   ("scrollback_lines",      scrollback_lines),
    CFG_INT   ("repeat_delay_ms",       repeat_delay_ms),
    CFG_STRING("antialiasing",          antialiasing),
    CFG_STRING("subpixel_order",        subpixel_order),
    CFG_BOOL  ("gamma_correct",         gamma_correct),
    CFG_BOOL  ("ligatures",             ligatures),
    CFG_STRING("theme",                 theme),
};

static const cfg_field_t COLORS_FIELDS[] = {
    CFG_STRING("background", color_background),
    CFG_STRING("foreground", color_foreground),
    CFG_STRING("cursor",     color_cursor),
};

static const cfg_field_t CURSOR_FIELDS[] = {
    CFG_STRING("theme",                cursor_theme),
    CFG_STRING("default",              cursor_default_path),
    CFG_STRING("link",                 cursor_link_path),
    CFG_FLOAT ("base_scale",           cursor_base_scale),
    CFG_BOOL  ("scale_with_terminal",  cursor_scale_with_terminal),
    CFG_STRING("default_hot_pos",      cursor_default_hot_pos),
    CFG_STRING("link_hot_pos",         cursor_link_hot_pos),
};

static const cfg_field_t KEYBINDINGS_FIELDS[] = {
    CFG_STRING("copy_to_clipboard",    copy_to_clipboard_binding),
    CFG_STRING("paste_from_clipboard", paste_from_clipboard_binding),
};

static const cfg_field_t MOUSE_FIELDS[] = {
    CFG_BOOL ("enable",       mouse_enable),
    CFG_FLOAT("scroll_speed", mouse_scroll_speed),
    CFG_FLOAT("sensitivity",  mouse_sensitivity),
};

static const cfg_field_t TOUCHPAD_FIELDS[] = {
    CFG_BOOL ("enable",         touchpad_enable),
    CFG_FLOAT("scroll_speed",   touchpad_scroll_speed),
    CFG_BOOL ("tap_to_click",   touchpad_tap_to_click),
    CFG_BOOL ("natural_scroll", touchpad_natural_scroll),
    CFG_FLOAT("sensitivity",    touchpad_sensitivity),
};
/// END HELPER FUNCTIONS

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
    if (general)
        load_fields(general, GENERAL_FIELDS, CFG_FIELD_COUNT(GENERAL_FIELDS), cfg);

    toml_table_t *colors = toml_table_in(root, "colors");
    if (colors) {
        load_fields(colors, COLORS_FIELDS, CFG_FIELD_COUNT(COLORS_FIELDS), cfg);

        char key[8];
        for (int i = 0; i < 16; i++) {
            snprintf(key, sizeof(key), "color%d", i);
            load_string(colors, key, cfg->color[i], sizeof(cfg->color[i]));
        }
    }

    toml_table_t *cursor = toml_table_in(root, "cursor");
    if (cursor)
        load_fields(cursor, CURSOR_FIELDS, CFG_FIELD_COUNT(CURSOR_FIELDS), cfg);

    toml_table_t *keybindings = toml_table_in(root, "keybindings");
    if (keybindings)
        load_fields(keybindings, KEYBINDINGS_FIELDS,
                    CFG_FIELD_COUNT(KEYBINDINGS_FIELDS), cfg);

    toml_table_t *mouse = toml_table_in(root, "mouse");
    if (mouse)
        load_fields(mouse, MOUSE_FIELDS, CFG_FIELD_COUNT(MOUSE_FIELDS), cfg);

    toml_table_t *touchpad = toml_table_in(root, "touchpad");
    if (touchpad)
        load_fields(touchpad, TOUCHPAD_FIELDS, CFG_FIELD_COUNT(TOUCHPAD_FIELDS), cfg);

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
    setenv("GHOSTCON_RUN_DIR",  cfg->run_dir,  ow);

    snprintf(buf, sizeof(buf), "%d", cfg->canary_deadline_ms);
    setenv("GHOSTCON_CANARY_DEADLINE_MS", buf, ow);

    /* Readers check presence only (getenv() truthiness), not the value --
     * "1" means on, ABSENT means off. On a plain startup export
     * (overwrite=false) that's already correct by construction: a false
     * bool here just never calls setenv, leaving it absent. On a hot-reload
     * export (overwrite=true) a bool can flip in either direction, so a
     * false value must actively unsetenv() a previously-true one, not just
     * skip setenv().
     */
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
     * every other bool above -- only export "0" when the config explicitly
     * turns it OFF, so the reader's own true-default stands when this key
     * is left out of the file entirely (the common case). Same
     * reload-direction-flip reasoning as above, inverted: a hot-reload back
     * to true must unsetenv() a previously-exported "0", not leave it stuck
     * off.
     */
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

    /* IN_CLOSE_WRITE covers a plain in-place write; IN_MOVED_TO covers the
     * write-temp-then-rename-over-original pattern most editors (vim
     * included) use by default -- watching the directory rather than the
     * file itself is what makes that second case observable at all (see
     * this function's own header doc comment). IN_MODIFY is a cheap extra
     * safety net for tools that do neither.
     */
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
     * variable-length struct inotify_event records, per inotify(7).
     */
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
