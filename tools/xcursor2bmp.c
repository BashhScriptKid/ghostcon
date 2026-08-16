/*
 * xcursor2bmp -- flattens a whole Xcursor theme directory into a
 * directory of BMP files, one per cursor name found (largest
 * available size, same rule ghostcon_cursor_load_xcursor() itself
 * uses). Lets a theme be pre-converted into the manual per-state BMP
 * overrides ghostcon's [cursor] config table supports, without
 * ghostcon needing to parse Xcursor at runtime at all if preferred.
 *
 * Usage: xcursor2bmp <theme_dir> <output_dir>
 * <theme_dir> may be either a theme root (looked for via its own
 * cursors/ subdirectory) or a cursors/ directory directly -- same
 * acceptance ghostcon_cursor_load_xcursor() has, reused here via the
 * same function.
 */

#include "ghostcon/core/cursor_image.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

int
main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s <theme_dir> <output_dir>\n", argv[0]);
        return 2;
    }
    const char *theme_dir = argv[1];
    const char *output_dir = argv[2];

    /* Unlike ghostcon_cursor_load_xcursor()'s single-FILE lookup (where
       trying theme_dir-as-cursors-dir first is unambiguous -- a
       specific file either exists there or it doesn't), a directory
       LISTING can't use the same order: opendir(theme_dir) succeeds
       just as readily on the theme ROOT (which also contains
       index.theme, icon-theme.cache, etc., not just cursor files) as
       on the actual cursors/ dir -- found live: pointing this at a
       real theme root silently "succeeded" by listing (and skipping,
       one by one, as invalid) the root's own non-cursor files instead
       of ever finding the real cursors/ subdirectory. Prefer
       theme_dir/cursors when it exists; only fall back to treating
       theme_dir itself as the cursors dir if that subdirectory isn't
       there at all. */
    char cursors_dir[1024];
    snprintf(cursors_dir, sizeof(cursors_dir), "%s/cursors", theme_dir);
    DIR *d = opendir(cursors_dir);
    if (!d) {
        snprintf(cursors_dir, sizeof(cursors_dir), "%s", theme_dir);
        d = opendir(cursors_dir);
    }
    if (!d) {
        fprintf(stderr, "xcursor2bmp: cannot open %s/cursors (or %s)\n", theme_dir, theme_dir);
        return 1;
    }

    if (mkdir(output_dir, 0755) != 0) {
        struct stat st;
        if (stat(output_dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
            fprintf(stderr, "xcursor2bmp: cannot create output dir %s\n", output_dir);
            closedir(d);
            return 1;
        }
        /* Already exists as a directory -- fine, write into it. */
    }

    int converted = 0, skipped = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.')
            continue; /* skip ., .., and hidden files */

        uint32_t w, h, hot_x, hot_y;
        uint32_t *pixels = ghostcon_cursor_load_xcursor(theme_dir, ent->d_name, &w, &h, &hot_x, &hot_y);
        if (!pixels) {
            fprintf(stderr, "xcursor2bmp: skipping %s (not a valid Xcursor file)\n", ent->d_name);
            skipped++;
            continue;
        }

        char out_path[1200];
        snprintf(out_path, sizeof(out_path), "%s/%s.bmp", output_dir, ent->d_name);
        if (ghostcon_cursor_write_bmp(out_path, pixels, w, h)) {
            printf("xcursor2bmp: %s -> %s (%ux%u, hotspot %u,%u)\n",
                   ent->d_name, out_path, w, h, hot_x, hot_y);
            converted++;
        } else {
            fprintf(stderr, "xcursor2bmp: failed to write %s\n", out_path);
            skipped++;
        }
        free(pixels);
    }
    closedir(d);

    printf("xcursor2bmp: %d converted, %d skipped\n", converted, skipped);
    return converted > 0 ? 0 : 1;
}
