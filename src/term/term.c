#include "ghostcon/term/term.h"

bool
ghostcon_term_init(ghostcon_term_t *term,
                   uint16_t cols, uint16_t rows,
                   uint16_t scrollback_cap)
{
    if (!ghostcon_screen_init(&term->screen, cols, rows, scrollback_cap))
        return false;
    if (!ghostcon_stream_init(&term->stream)) {
        ghostcon_screen_deinit(&term->screen);
        return false;
    }
    term->cols = cols;
    term->rows = rows;
    return true;
}

void
ghostcon_term_deinit(ghostcon_term_t *term) {
    ghostcon_screen_deinit(&term->screen);
    ghostcon_stream_deinit(&term->stream);
}

void
ghostcon_term_feed(ghostcon_term_t *term,
                   const uint8_t *data, size_t len)
{
    ghostcon_stream_process(&term->stream, data, len, &term->screen);
}

bool
ghostcon_term_resize(ghostcon_term_t *term,
                     uint16_t new_cols, uint16_t new_rows)
{
    if (!ghostcon_screen_resize(&term->screen, new_cols, new_rows))
        return false;
    term->cols = new_cols;
    term->rows = new_rows;
    return true;
}

void
ghostcon_term_set_output(ghostcon_term_t *term,
                         ghostcon_output_fn fn, void *userdata)
{
    ghostcon_stream_set_output(&term->stream, fn, userdata);
}

void
ghostcon_term_set_title(ghostcon_term_t *term,
                        ghostcon_title_fn fn, void *userdata)
{
    ghostcon_stream_set_title(&term->stream, fn, userdata);
}

void
ghostcon_term_set_notify(ghostcon_term_t *term,
                         ghostcon_notify_fn fn, void *userdata)
{
    ghostcon_stream_set_notify(&term->stream, fn, userdata);
}
