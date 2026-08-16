#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "screen.h"
#include "stream.h"

/* ------------------------------------------------------------------ */
/* Terminal — top-level terminal instance                              */
/*                                                                     */
/* Orchestrates screen + stream + (future: color, selection, etc.)    */
/* This is the main API surface for ghostcon-core.                     */
/* ------------------------------------------------------------------ */

typedef struct {
    ghostcon_screen_t screen;
    ghostcon_stream_t stream;
    uint16_t cols, rows;
} ghostcon_term_t;

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

bool ghostcon_term_init(ghostcon_term_t *term,
                        uint16_t cols, uint16_t rows,
                        uint16_t scrollback_cap);
void ghostcon_term_deinit(ghostcon_term_t *term);

/* ------------------------------------------------------------------ */
/* Feeding data                                                        */
/* ------------------------------------------------------------------ */

/* Feed raw PTY bytes into the terminal.
   Updates screen state via the stream processor. */
void ghostcon_term_feed(ghostcon_term_t *term,
                        const uint8_t *data, size_t len);

/* ------------------------------------------------------------------ */
/* Resize                                                              */
/* ------------------------------------------------------------------ */

bool ghostcon_term_resize(ghostcon_term_t *term,
                          uint16_t new_cols, uint16_t new_rows);

/* ------------------------------------------------------------------ */
/* Output channel                                                      */
/*                                                                     */
/* Register a callback for terminal responses (DSR, DA, DECRPM, OSC). */
/* The callback receives raw response bytes to write back to the PTY.  */
/* ------------------------------------------------------------------ */

void ghostcon_term_set_output(ghostcon_term_t *term,
                              ghostcon_output_fn fn, void *userdata);

/* ------------------------------------------------------------------ */
/* Title channel                                                       */
/*                                                                     */
/* Register a callback for OSC 0/2 (window/icon title). ghostcon-core  */
/* repurposes this as process identity — see PLAN.md.                  */
/* ------------------------------------------------------------------ */

void ghostcon_term_set_title(ghostcon_term_t *term,
                             ghostcon_title_fn fn, void *userdata);

/* ------------------------------------------------------------------ */
/* Notify channel                                                      */
/*                                                                     */
/* Register a callback for OSC 9/777 (desktop notifications, stub      */
/* tier — see stream.h's notify_fn doc comment).                       */
/* ------------------------------------------------------------------ */

void ghostcon_term_set_notify(ghostcon_term_t *term,
                              ghostcon_notify_fn fn, void *userdata);

/* ------------------------------------------------------------------ */
/* Accessors                                                           */
/* ------------------------------------------------------------------ */

static inline ghostcon_screen_t *
ghostcon_term_screen(ghostcon_term_t *term) {
    return &term->screen;
}
