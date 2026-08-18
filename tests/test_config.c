#define _DEFAULT_SOURCE

#include "ghostcon/config/config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
    else { printf("PASS: %s\n", msg); } \
} while (0)

int
main(void)
{
    /* Nonexistent path -- pure defaults, not an error. */
    ghostcon_config_t cfg;
    bool ok = ghostcon_config_load("/nonexistent/ghostcon-test.toml", &cfg);
    CHECK(ok, "missing config file is not an error");
    CHECK(strcmp(cfg.drm_node, "/dev/dri/card1") == 0, "default drm_node");
    CHECK(cfg.canary_deadline_ms == 4000, "default canary_deadline_ms");
    CHECK(cfg.font_size == 16, "default font_size");
    CHECK(cfg.clear_on_logout == true, "default clear_on_logout");
    CHECK(cfg.zoom_step == 2, "default zoom_step");
    CHECK(cfg.font_family[0] == '\0', "default font_family is empty");
    CHECK(cfg.font_variant[0] == '\0', "default font_variant is empty");
    CHECK(strcmp(cfg.antialiasing, "grayscale") == 0, "default antialiasing");
    CHECK(strcmp(cfg.subpixel_order, "rgb") == 0, "default subpixel_order");
    CHECK(cfg.gamma_correct == false, "default gamma_correct");
    CHECK(cfg.scrollback_lines == 2000, "default scrollback_lines");
    CHECK(cfg.repeat_delay_ms == 250, "default repeat_delay_ms");
    CHECK(cfg.repeat_rate_ms == 50, "default repeat_rate_ms");
    CHECK(cfg.mouse_enable == true, "default mouse_enable");
    CHECK(cfg.mouse_scroll_speed == 1.0f, "default mouse_scroll_speed");
    CHECK(cfg.mouse_sensitivity == 0.0f, "default mouse_sensitivity");
    CHECK(cfg.touchpad_enable == true, "default touchpad_enable");
    CHECK(cfg.touchpad_scroll_speed == 1.0f, "default touchpad_scroll_speed");
    CHECK(cfg.touchpad_tap_to_click == true, "default touchpad_tap_to_click");
    CHECK(cfg.touchpad_natural_scroll == false, "default touchpad_natural_scroll");
    CHECK(cfg.touchpad_sensitivity == 0.0f, "default touchpad_sensitivity");
    CHECK(cfg.cursor_theme[0] == '\0', "default cursor_theme is empty");
    CHECK(cfg.cursor_default_path[0] == '\0', "default cursor_default_path is empty");
    CHECK(cfg.cursor_link_path[0] == '\0', "default cursor_link_path is empty");
    CHECK(cfg.cursor_base_scale == 1.0f, "default cursor_base_scale");
    CHECK(cfg.cursor_scale_with_terminal == true, "default cursor_scale_with_terminal");
    CHECK(strcmp(cfg.copy_to_clipboard_binding, "ctrl+shift+c") == 0, "default copy_to_clipboard_binding");
    CHECK(strcmp(cfg.paste_from_clipboard_binding, "ctrl+shift+v") == 0, "default paste_from_clipboard_binding");
    CHECK(cfg.cursor_default_hot_pos[0] == '\0', "default cursor_default_hot_pos is empty");
    CHECK(cfg.cursor_link_hot_pos[0] == '\0', "default cursor_link_hot_pos is empty");

    /* mkstemp() overwrites its template with the resolved name in place,
       so each use below needs a fresh copy of the template string --
       reusing an already-resolved path (no more "XXXXXX") fails. */
#define PATH_TEMPLATE "/tmp/ghostcon-test-config-XXXXXX"
    char path[sizeof(PATH_TEMPLATE)];

    /* Partial file: only drm_node set -- everything else stays default. */
    strcpy(path, PATH_TEMPLATE);
    int fd = mkstemp(path);
    CHECK(fd >= 0, "mkstemp for partial config");
    if (fd >= 0) {
        const char *contents = "[general]\ndrm_node = \"/dev/dri/card2\"\n";
        write(fd, contents, strlen(contents));
        close(fd);

        ok = ghostcon_config_load(path, &cfg);
        CHECK(ok, "partial config file parses");
        CHECK(strcmp(cfg.drm_node, "/dev/dri/card2") == 0,
              "partial config overrides drm_node");
        CHECK(cfg.canary_deadline_ms == 4000,
              "partial config leaves canary_deadline_ms at default");
        unlink(path);
    }

    /* Full file: every key set. */
    strcpy(path, PATH_TEMPLATE);
    fd = mkstemp(path);
    CHECK(fd >= 0, "mkstemp for full config");
    if (fd >= 0) {
        const char *contents =
            "[general]\n"
            "drm_node = \"/dev/dri/card3\"\n"
            "run_dir = \"/tmp/ghostcon-run\"\n"
            "canary_deadline_ms = 9000\n"
            "disable_wall = true\n"
            "disable_kmscon_fallback = true\n"
            "font_size = 20\n"
            "font_family = \"JetBrainsMono Nerd Font Mono\"\n"
            "font_variant = \"Bold\"\n"
            "antialiasing = \"cleartype\"\n"
            "subpixel_order = \"bgr\"\n"
            "gamma_correct = true\n"
            "scrollback_lines = 5000\n"
            "repeat_delay_ms = 300\n"
            "repeat_rate_ms = 25\n"
            "clear_on_logout = false\n"
            "zoom_step = 3\n"
            "[cursor]\n"
            "theme = \"/usr/share/icons/Breeze\"\n"
            "default = \"/etc/terminalcursor/default.bmp\"\n"
            "link = \"/etc/terminalcursor/link.bmp\"\n"
            "base_scale = 0.5\n"
            "scale_with_terminal = false\n"
            "default_hot_pos = \"12,8\"\n"
            "link_hot_pos = \"3,4\"\n"
            "[keybindings]\n"
            "copy_to_clipboard = \"ctrl+alt+c\"\n"
            "paste_from_clipboard = \"ctrl+alt+v\"\n"
            "[mouse]\n"
            "enable = false\n"
            "scroll_speed = 2.5\n"
            "sensitivity = 0.5\n"
            "[touchpad]\n"
            "enable = false\n"
            "scroll_speed = 0.5\n"
            "tap_to_click = false\n"
            "natural_scroll = true\n"
            "sensitivity = -0.5\n";
        write(fd, contents, strlen(contents));
        close(fd);

        ok = ghostcon_config_load(path, &cfg);
        CHECK(ok, "full config file parses");
        CHECK(strcmp(cfg.drm_node, "/dev/dri/card3") == 0, "full config drm_node");
        CHECK(strcmp(cfg.run_dir, "/tmp/ghostcon-run") == 0, "full config run_dir");
        CHECK(cfg.canary_deadline_ms == 9000, "full config canary_deadline_ms");
        CHECK(cfg.disable_wall == true, "full config disable_wall");
        CHECK(cfg.disable_kmscon_fallback == true, "full config disable_kmscon_fallback");
        CHECK(cfg.font_size == 20, "full config font_size");
        CHECK(strcmp(cfg.font_family, "JetBrainsMono Nerd Font Mono") == 0, "full config font_family");
        CHECK(strcmp(cfg.font_variant, "Bold") == 0, "full config font_variant");
        CHECK(strcmp(cfg.antialiasing, "cleartype") == 0, "full config antialiasing");
        CHECK(strcmp(cfg.subpixel_order, "bgr") == 0, "full config subpixel_order");
        CHECK(cfg.gamma_correct == true, "full config gamma_correct");
        CHECK(cfg.scrollback_lines == 5000, "full config scrollback_lines");
        CHECK(cfg.repeat_delay_ms == 300, "full config repeat_delay_ms");
        CHECK(cfg.repeat_rate_ms == 25, "full config repeat_rate_ms");
        CHECK(cfg.clear_on_logout == false, "full config clear_on_logout");
        CHECK(cfg.zoom_step == 3, "full config zoom_step");
        CHECK(strcmp(cfg.cursor_theme, "/usr/share/icons/Breeze") == 0, "full config cursor_theme");
        CHECK(strcmp(cfg.cursor_default_path, "/etc/terminalcursor/default.bmp") == 0,
              "full config cursor_default_path");
        CHECK(strcmp(cfg.cursor_link_path, "/etc/terminalcursor/link.bmp") == 0,
              "full config cursor_link_path");
        CHECK(cfg.cursor_base_scale == 0.5f, "full config cursor_base_scale");
        CHECK(cfg.cursor_scale_with_terminal == false, "full config cursor_scale_with_terminal");
        CHECK(strcmp(cfg.copy_to_clipboard_binding, "ctrl+alt+c") == 0, "full config copy_to_clipboard_binding");
        CHECK(strcmp(cfg.paste_from_clipboard_binding, "ctrl+alt+v") == 0, "full config paste_from_clipboard_binding");
        CHECK(cfg.mouse_enable == false, "full config mouse_enable");
        CHECK(cfg.mouse_scroll_speed == 2.5f, "full config mouse_scroll_speed");
        CHECK(cfg.mouse_sensitivity == 0.5f, "full config mouse_sensitivity");
        CHECK(cfg.touchpad_enable == false, "full config touchpad_enable");
        CHECK(cfg.touchpad_scroll_speed == 0.5f, "full config touchpad_scroll_speed");
        CHECK(cfg.touchpad_tap_to_click == false, "full config touchpad_tap_to_click");
        CHECK(cfg.touchpad_natural_scroll == true, "full config touchpad_natural_scroll");
        CHECK(cfg.touchpad_sensitivity == -0.5f, "full config touchpad_sensitivity");
        CHECK(strcmp(cfg.cursor_default_hot_pos, "12,8") == 0, "full config cursor_default_hot_pos");
        CHECK(strcmp(cfg.cursor_link_hot_pos, "3,4") == 0, "full config cursor_link_hot_pos");
        unlink(path);
    }

    /* Malformed file: a real parse error, not a missing-file default. */
    strcpy(path, PATH_TEMPLATE);
    fd = mkstemp(path);
    CHECK(fd >= 0, "mkstemp for malformed config");
    if (fd >= 0) {
        const char *contents = "[general\ndrm_node = \n";
        write(fd, contents, strlen(contents));
        close(fd);

        ok = ghostcon_config_load(path, &cfg);
        CHECK(!ok, "malformed config file is reported as an error");
        unlink(path);
    }

    /* Env var precedence: an already-set env var beats the config file. */
    ghostcon_config_defaults(&cfg);
    snprintf(cfg.drm_node, sizeof(cfg.drm_node), "%s", "/dev/dri/from-config");
    setenv("GHOSTCON_DRM_NODE", "/dev/dri/from-env", 1);
    ghostcon_config_export_env(&cfg, "/tmp/unused.toml", false);
    const char *exported = getenv("GHOSTCON_DRM_NODE");
    CHECK(exported && strcmp(exported, "/dev/dri/from-env") == 0,
          "export_env(overwrite=false) does not overwrite an already-set env var");
    unsetenv("GHOSTCON_DRM_NODE");

    ghostcon_config_export_env(&cfg, "/tmp/unused.toml", false);
    exported = getenv("GHOSTCON_DRM_NODE");
    CHECK(exported && strcmp(exported, "/dev/dri/from-config") == 0,
          "export_env(overwrite=false) sets the env var when none was already present");

    /* overwrite=true (hot-reload semantics): must actually take effect
       over an already-set value, unlike the overwrite=false case above. */
    setenv("GHOSTCON_DRM_NODE", "/dev/dri/stale", 1);
    ghostcon_config_export_env(&cfg, "/tmp/unused.toml", true);
    exported = getenv("GHOSTCON_DRM_NODE");
    CHECK(exported && strcmp(exported, "/dev/dri/from-config") == 0,
          "export_env(overwrite=true) replaces an already-set env var");

    /* GHOSTCON_CONFIG_PATH itself is always exported, regardless of
       overwrite -- it's how supervisor/ghostcon-core know what to
       watch for hot-reload. */
    exported = getenv("GHOSTCON_CONFIG_PATH");
    CHECK(exported && strcmp(exported, "/tmp/unused.toml") == 0,
          "export_env always exports GHOSTCON_CONFIG_PATH");

    /* Bool flip in both directions on reload: disable_wall true->false
       must unsetenv (readers check presence only, not value -- see
       config.c's own comment), not just skip setenv(). */
    unsetenv("GHOSTCON_DISABLE_WALL");
    cfg.disable_wall = true;
    ghostcon_config_export_env(&cfg, "/tmp/unused.toml", true);
    CHECK(getenv("GHOSTCON_DISABLE_WALL") != NULL,
          "export_env(overwrite=true) sets a bool flipped to true");
    cfg.disable_wall = false;
    ghostcon_config_export_env(&cfg, "/tmp/unused.toml", true);
    CHECK(getenv("GHOSTCON_DISABLE_WALL") == NULL,
          "export_env(overwrite=true) unsets a bool flipped back to false");

    /* Hot-reload watch helper (inotify). */
    {
        char dir_template[] = "/tmp/ghostcon-test-watch-XXXXXX";
        char *dir = mkdtemp(dir_template);
        CHECK(dir != NULL, "mkdtemp for watch test");
        if (dir) {
            char watched_path[512], other_path[512];
            snprintf(watched_path, sizeof(watched_path), "%s/ghostcon.toml", dir);
            snprintf(other_path, sizeof(other_path), "%s/unrelated.txt", dir);

            /* File doesn't need to exist yet -- only its parent
               directory does (watch_open watches the directory, not
               the file itself; see its own doc comment on why). */
            int watch_fd = ghostcon_config_watch_open(watched_path);
            CHECK(watch_fd >= 0, "config_watch_open succeeds");

            if (watch_fd >= 0) {
                /* Unrelated file in the same directory must NOT match. */
                FILE *f = fopen(other_path, "w");
                if (f) { fputs("x", f); fclose(f); }
                usleep(50000); /* let inotify deliver before draining */
                CHECK(!ghostcon_config_watch_check(watch_fd, watched_path),
                      "config_watch_check ignores an unrelated file");

                /* write-temp-then-rename, the pattern most editors use
                   by default (vim included) -- must match. */
                char tmp_path[512];
                snprintf(tmp_path, sizeof(tmp_path), "%s/ghostcon.toml.tmp", dir);
                f = fopen(tmp_path, "w");
                if (f) { fputs("[general]\n", f); fclose(f); }
                rename(tmp_path, watched_path);
                usleep(50000);
                CHECK(ghostcon_config_watch_check(watch_fd, watched_path),
                      "config_watch_check detects a write-then-rename-over save");

                close(watch_fd);
            }

            unlink(other_path);
            unlink(watched_path);
            rmdir(dir);
        }
    }

    if (failures > 0) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("ALL TESTS PASSED\n");
    return 0;
}
