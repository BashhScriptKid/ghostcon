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
    uint16_t                osc_len;
    uint16_t                dcs_len;

    /* Internal buffer for collecting OSC/DCS data */
    char    *buf;            /* allocated buffer (or local buffer for small) */
    uint16_t buf_cap;
    char     local_buf[2048]; /* small-buffer optimization (large enough for OSC) */
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
