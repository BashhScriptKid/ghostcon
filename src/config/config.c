#define _DEFAULT_SOURCE

#include "ghostcon/config/config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    }

    toml_free(root);
    return true;
}

void
ghostcon_config_export_env(const ghostcon_config_t *cfg)
{
    char buf[32];

    setenv("GHOSTCON_DRM_NODE", cfg->drm_node, 0);
    setenv("GHOSTCON_RUN_DIR", cfg->run_dir, 0);

    snprintf(buf, sizeof(buf), "%d", cfg->canary_deadline_ms);
    setenv("GHOSTCON_CANARY_DEADLINE_MS", buf, 0);

    if (cfg->disable_wall)
        setenv("GHOSTCON_DISABLE_WALL", "1", 0);
    if (cfg->disable_kmscon_fallback)
        setenv("GHOSTCON_DISABLE_KMSCON_FALLBACK", "1", 0);

    /* font_size has no existing GHOSTCON_* env var (core/main.c's
       FONT_SIZE is a compile-time #define, not read from the
       environment at all yet) -- exported anyway so it's available the
       moment core/main.c grows a reader for it, without a second round
       of plumbing through this file. */
    snprintf(buf, sizeof(buf), "%d", cfg->font_size);
    setenv("GHOSTCON_FONT_SIZE", buf, 0);

    /* Default true (see core/main.c's CLEAR_ON_LOGOUT_DEFAULT), unlike
       every other bool above -- only export when the config explicitly
       turns it OFF, so the reader's own true-default stands when this
       key is left out of the file entirely (the common case). */
    if (!cfg->clear_on_logout)
        setenv("GHOSTCON_CLEAR_ON_LOGOUT", "0", 0);
}
