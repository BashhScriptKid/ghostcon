#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "screen.h"

/* ------------------------------------------------------------------ */
/* Stream state — ECMA-48 / DEC VT state machine                      */
/* ------------------------------------------------------------------ */

typedef enum {
    GC_STREAM_GROUND,
    GC_STREAM_ESC,
    GC_STREAM_ESC_INTERMEDIATE,
    GC_STREAM_CSI_ENTRY,
    GC_STREAM_CSI_PARAM,
    GC_STREAM_CSI_INTERMEDIATE,
    GC_STREAM_CSI_IGNORE,
    GC_STREAM_DCS_ENTRY,
    GC_STREAM_DCS_PARAM,
    GC_STREAM_DCS_INTERMEDIATE,
    GC_STREAM_DCS_PASSTHROUGH,
    GC_STREAM_DCS_PASSTHROUGH_ESC,
    GC_STREAM_DCS_IGNORE,
    GC_STREAM_OSC_STRING,
    GC_STREAM_SOS_PM_APC_STRING,
} ghostcon_stream_state_t;

#define GC_STREAM_MAX_INTERMEDIATES 4
#define GC_STREAM_MAX_PARAMS        24

/* ------------------------------------------------------------------ */
/* Output channel                                                      */
/*                                                                     */
/* Terminal responses (DSR, DA, DECRPM, ...) are delivered through a   */
/* callback instead of being written directly to a PTY. The term layer */
/* bridges this to the host.                                           */
/* ------------------------------------------------------------------ */

typedef void (*ghostcon_output_fn)(void *userdata,
                                   const uint8_t *data, size_t len);

/* ------------------------------------------------------------------ */
/* Title channel                                                       */
/*                                                                     */
/* OSC 0/2 (window/icon title) carries no rendering meaning on a bare  */
/* TTY -- there's no window chrome. ghostcon-core repurposes it as     */
/* process identity (argv[0]/PR_SET_NAME, see PLAN.md's "Process       */
/* identity" section), which requires prctl()/argv rewriting the term  */
/* layer has no business doing -- so the raw title string is handed    */
/* up through this callback instead of being interpreted here.        */
/* ------------------------------------------------------------------ */

typedef void (*ghostcon_title_fn)(void *userdata, const char *title);

/* ------------------------------------------------------------------ */
/* Notify channel                                                      */
/*                                                                     */
/* OSC 9/777 (desktop notifications) -- stub tier, no ghostcon-ipc      */
/* broker exists yet (PLAN.md's own deferred item), so the message is   */
/* handed up rather than acted on here; ghostcon-core logs it via        */
/* core/diag.c, which term/ deliberately has no dependency on (diag.c   */
/* needs libsystemd; term/ stays usable standalone, e.g. by the         */
/* ghostcon_dump tool and its tests, without pulling that in).          */
/* ------------------------------------------------------------------ */

typedef void (*ghostcon_notify_fn)(void *userdata, const char *message);

/* ------------------------------------------------------------------ */
/* Stream processor                                                    */
/* ------------------------------------------------------------------ */

typedef struct ghostcon_stream ghostcon_stream_t;

struct ghostcon_stream {
    ghostcon_stream_state_t state;
    uint8_t                 intermediates[GC_STREAM_MAX_INTERMEDIATES];
    uint8_t                 intermediates_idx;
    uint16_t                params[GC_STREAM_MAX_PARAMS];
    uint8_t                 params_idx;
    uint16_t                param_acc;
    uint8_t                 param_acc_idx;
    uint32_t                utf8_acc;      /* UTF-8 DFA codepoint accumulator */
    uint8_t                 utf8_state;    /* UTF-8 DFA state (0 = accept) */
    uint32_t                osc_len;
    uint32_t                dcs_len;
    char                    dcs_final; /* the 0x40-0x7E byte that entered DCS_PASSTHROUGH (e.g. 'q' for sixel) */
    uint32_t                apc_len;
    char                    apc_introducer; /* 'X'/'^'/'_' -- which of SOS/PM/APC we're in */
    bool                    apc_pending;    /* saw ESC mid-APC, awaiting '\' to confirm ST */
    bool                    apc_overflow;   /* payload exceeded buf_cap -- reject, don't truncate-and-accept */

    /* Internal buffer for collecting OSC/DCS/APC data. local_buf's
       6144 bytes covers one Kitty graphics chunk (4096 base64 payload
       bytes, the protocol's own per-chunk cap, plus control-data
       slack) and any OSC/DCS payload that fits without growing.
       Un-chunked DCS payloads (sixel, which has no chunking mechanism
       in the protocol at all, unlike Kitty) routinely exceed that --
       see the DCS_PASSTHROUGH case in stream.c, which reallocates
       *buf up to GHOSTCON_STREAM_DCS_MAX_BUF as needed rather than
       silently truncating. buf_cap/osc_len/dcs_len/apc_len are
       uint32_t (not uint16_t, the original size before sixel) for the
       same reason -- a 16-bit length field would itself have capped
       any single payload at 65535 bytes regardless of how big *buf
       was allowed to grow. */
    char    *buf;            /* allocated buffer (or local buffer for small) */
    uint32_t buf_cap;
    char     local_buf[6144];
    bool     osc_pending;    /* OSC data collected, awaiting dispatch */
    bool     osc_terminated_by_bel; /* OSC was terminated by BEL (vs ST) */

    /* Output channel for terminal responses (DSR/DA/DECRPM/OSC) */
    ghostcon_output_fn     output_fn;
    void                  *output_userdata;

    /* Title channel for OSC 0/2 (process-identity repurposing) */
    ghostcon_title_fn       title_fn;
    void                   *title_userdata;

    /* Notify channel for OSC 9/777 (desktop notifications, stub tier) */
    ghostcon_notify_fn       notify_fn;
    void                    *notify_userdata;

    /* Renderer's current cell pixel size, needed to compute the
       post-placement cursor move Kitty graphics does by default (see
       ghostcon_kitty_graphics_handle's doc comment) -- 0 means
       unknown, which suppresses that move rather than guessing wrong.
       Set via ghostcon_stream_set_cell_size(); term/ has no other way
       to learn this since cell size is purely a rendering/font
       concept. */
    int32_t cell_w, cell_h;
};

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

/* Initialize a stream processor. Returns true on success. */
bool ghostcon_stream_init(ghostcon_stream_t *st);

/* Destroy stream, freeing any allocated memory */
void ghostcon_stream_deinit(ghostcon_stream_t *st);

/* Reset stream state to ground (e.g. on terminal reset) */
void ghostcon_stream_reset(ghostcon_stream_t *st);

/* Set the output callback for terminal responses. Pass NULL to disable. */
void ghostcon_stream_set_output(ghostcon_stream_t *st,
                                ghostcon_output_fn fn, void *userdata);

/* Set the title callback for OSC 0/2. Pass NULL to disable. */
void ghostcon_stream_set_title(ghostcon_stream_t *st,
                               ghostcon_title_fn fn, void *userdata);

/* Set the notify callback for OSC 9/777. Pass NULL to disable. */
void ghostcon_stream_set_notify(ghostcon_stream_t *st,
                                ghostcon_notify_fn fn, void *userdata);

/* Tell the stream the renderer's current cell pixel size, so Kitty
   graphics placements can compute their default post-placement cursor
   move (see ghostcon_kitty_graphics_handle's doc comment). Call again
   whenever it changes (font size change, DPI change, etc). Never
   called at all (both stay 0) just means that cursor move is skipped,
   not a crash. */
void ghostcon_stream_set_cell_size(ghostcon_stream_t *st, int32_t cell_w, int32_t cell_h);

/* ------------------------------------------------------------------ */
/* Processing                                                          */
/* ------------------------------------------------------------------ */

/* Feed bytes to the stream processor.
   The stream processor calls the appropriate screen functions. */
void ghostcon_stream_process(ghostcon_stream_t *st,
                             const uint8_t *data, size_t len,
                             ghostcon_screen_t *screen);

/* Feed a single byte */
void ghostcon_stream_process_byte(ghostcon_stream_t *st,
                                  uint8_t c,
                                  ghostcon_screen_t *screen);
