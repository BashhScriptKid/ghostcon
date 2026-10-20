#pragma once

/* The screen type, forward-declared.
 *
 * screen.h and selection.h name the same type, and they include each other: the
 * screen embeds a selection by value (so screen.h needs selection.h), and the
 * selection API takes a screen pointer (so selection.h needs the screen type).
 *
 * The declaration therefore lives here, and both headers include it.
 *
 * When each of them declared the type itself, changing the tag in one of them
 * would silently leave the other pointing at a *different* type -- the duplicate
 * declaration would still compile, and the mismatch would only show up as a type
 * error far away from the change that caused it.
 */
typedef struct ghostcon_screen ghostcon_screen_t;
