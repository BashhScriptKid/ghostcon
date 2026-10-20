/* stream_apc.h
 *
 * Private header for the APC/DCS dispatch module (stream_apc.c).
 *
 * NOT part of the public API (it lives under src/, not include/)
 */

#pragma once

#include "stream_internal.h"

/* Cross-module dispatch entry points */
extern void gc_stream_apc_dispatch(ghostcon_stream_t *st, ghostcon_screen_t *s);
extern void gc_stream_dcs_dispatch(ghostcon_stream_t *st, ghostcon_screen_t *s);

/* Grows st->buf to fit one more byte (up to the DCS cap). Only the
 * DCS_PASSTHROUGH state machine state calls this
 *
 * see stream.c.
 */
extern bool gc_stream_dcs_buf_grow(ghostcon_stream_t *st);
