#pragma once

#include <stdio.h>

#include "ghostcon/term/screen.h"

/* Canonical, diffable screen-state dump format shared between
   tools/ghostcon_dump.c (offline replay) and core/main.c's live
   dump-current-frame hotkey (Ctrl+Alt+D) -- same format used by
   tools/ghostty_dump.c, so output from either path can be diffed
   directly against the reference implementation. See
   tools/run_compare.sh for that comparison workflow. */
void ghostcon_screen_dump(FILE *f, ghostcon_screen_t *screen);
