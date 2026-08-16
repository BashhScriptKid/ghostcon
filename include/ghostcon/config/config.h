#pragma once

#include <stdbool.h>

/* ------------------------------------------------------------------ */
/* TOML config — loaded once, by undead-head only                      */
/*                                                                     */
/* Every other binary in the tree stays exactly as it was before this  */
/* existed: they all already read their tunables via getenv("GHOSTCON_ */
/* *"). undead-head loads this file, then setenv()s the corresponding   */
/* variable for each config-backed value it found (using overwrite=0,  */
/* so an already-set env var — e.g. from a test harness — always wins   */
/* over the file), before spawning ghost-ptyserv/supervisor. Those      */
/* child processes inherit the environment normally, so the whole tree  */
/* picks the values up with no other file needing to know a config file */
/* exists. Precedence: explicit env var > config file > hardcoded       */
/* default.                                                             */
/* ------------------------------------------------------------------ */

typedef struct {
    char drm_node[256];
    char run_dir[256];
    int  canary_deadline_ms;
    bool disable_wall;
    bool disable_kmscon_fallback;
    int  font_size;
    bool clear_on_logout;
} ghostcon_config_t;

/* Fills `cfg` with the hardcoded defaults (matching what every binary
   already falls back to today when no env var is set). */
void ghostcon_config_defaults(ghostcon_config_t *cfg);

/* Loads `path`, overlaying its values onto the defaults. A missing file
   is not an error -- returns true with `cfg` left at pure defaults.
   Returns false only if the file exists but fails to parse. */
bool ghostcon_config_load(const char *path, ghostcon_config_t *cfg);

/* setenv()s GHOSTCON_* for every config-backed value in `cfg`, using
   overwrite=0 so an already-set env var wins. Call this once, in
   undead-head, before spawning anything. */
void ghostcon_config_export_env(const ghostcon_config_t *cfg);
