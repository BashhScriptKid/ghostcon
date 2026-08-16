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
        load_bool(general, "clear_on_logout", &cfg->clear_on_logout);
        load_int(general, "zoom_step", &cfg->zoom_step);
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
