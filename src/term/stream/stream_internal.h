/* stream_internal.h
 *
 * The stream module's shared preamble: the includes every file in the module
 * needs, and a description of how the module is split.
 *
 * NOT part of the public API (it lives under src/, not include/)
 *
 * The module is four files:
 *
 *   stream.c       the state machine: it collects the bytes of one escape
 *                  sequence and hands each sequence category to exactly one
 *                  dispatch module
 *   stream_csi.c   CSI finals (incl. DEC-private variants)
 *   stream_osc.c   OSC payloads (manual number tier + libghostty-vt fallback)
 *   stream_apc.c   Kitty graphics / SOS-PM-APC payloads, and sixel (DCS q)
 *
 * Each dispatch module has its own private header, and each file includes only
 * the ones it uses:
 *
 *   stream_csi.h   the CSI parameter helpers and the CSI dispatch entry
 *   stream_osc.h   the OSC dispatch entry
 *   stream_apc.h   the APC and DCS dispatch entries
 *
 * Adding a new escape sequence is normally one function + one line in that
 * module's dispatch table, not the monolithic switch.
 *
 * Note on opacity: ghostcon_stream_t's struct internals ARE visible here.
 *
 * The public header (include/ghostcon/term/stream.h) defines the struct in full,
 * and ghostcon_term_t embeds the stream by value, so the layout is not free to
 * change.
 */

#pragma once

#include <stdbool.h>
#include <string.h>

#include "ghostcon/term/stream.h"
