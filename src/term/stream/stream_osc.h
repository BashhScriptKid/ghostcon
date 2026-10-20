/* stream_osc.h
 *
 * Private header for the OSC dispatch module (stream_osc.c).
 *
 * NOT part of the public API (it lives under src/, not include/)
 */

#pragma once

#include "stream_internal.h"

/* Cross-module dispatch entry point */
extern void gc_stream_osc_dispatch(ghostcon_stream_t *st, ghostcon_screen_t *s);
