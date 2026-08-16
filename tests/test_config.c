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
            "clear_on_logout = false\n";
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
        CHECK(cfg.clear_on_logout == false, "full config clear_on_logout");
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
    ghostcon_config_export_env(&cfg);
    const char *exported = getenv("GHOSTCON_DRM_NODE");
    CHECK(exported && strcmp(exported, "/dev/dri/from-env") == 0,
          "export_env does not overwrite an already-set env var");
    unsetenv("GHOSTCON_DRM_NODE");

    ghostcon_config_export_env(&cfg);
    exported = getenv("GHOSTCON_DRM_NODE");
    CHECK(exported && strcmp(exported, "/dev/dri/from-config") == 0,
          "export_env sets the env var when none was already present");

    if (failures > 0) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("ALL TESTS PASSED\n");
    return 0;
}
