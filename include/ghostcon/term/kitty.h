#pragma once

#include <stdint.h>
#include <stdbool.h>

/* Kitty keyboard protocol state */
/* https://sw.kovidgoyal.net/kitty/keyboard-protocol/ */

/* Flags bit positions */
#define GC_KITTY_DISAMBIGUATE      (1 << 0)  /* disambiguate escape codes (CSI u)          */
#define GC_KITTY_REPORT_EVENTS     (1 << 1)  /* report event types (key press/release)      */
#define GC_KITTY_REPORT_ALTERNATES (1 << 2)  /* report alternate keys                      */
#define GC_KITTY_REPORT_ALL        (1 << 3)  /* report all keys as escape codes            */
#define GC_KITTY_REPORT_ASSOCIATED (1 << 4)  /* report associated text                     */

#define GC_KITTY_FLAGS_ALL         ((uint8_t)(GC_KITTY_DISAMBIGUATE | GC_KITTY_REPORT_EVENTS | \
                                              GC_KITTY_REPORT_ALTERNATES | GC_KITTY_REPORT_ALL | \
                                              GC_KITTY_REPORT_ASSOCIATED))

/* Set modes for CSI = u */
typedef enum {
    GC_KITTY_SET    = 0,  /* replace current flags */
    GC_KITTY_OR     = 1,  /* bitwise OR into current flags */
    GC_KITTY_NOT    = 2,  /* bitwise AND NOT into current flags */
} ghostcon_kitty_set_mode_t;

/* Flag stack — fixed-size ring buffer, depth 8 */
#define GC_KITTY_STACK_DEPTH 8

typedef struct {
    uint8_t flags[GC_KITTY_STACK_DEPTH];
    uint8_t idx;  /* 0..7, current top of stack */
} ghostcon_kitty_stack_t;

/* Complete kitty keyboard state */
typedef struct {
    ghostcon_kitty_stack_t stack;
} ghostcon_kitty_state_t;

/* Initialize kitty state (all flags disabled) */
static inline void
ghostcon_kitty_init(ghostcon_kitty_state_t *k) {
    for (int i = 0; i < GC_KITTY_STACK_DEPTH; i++)
        k->stack.flags[i] = 0;
    k->stack.idx = 0;
}

/* Push flags onto the stack (CSI > {flags} u) */
void ghostcon_kitty_push(ghostcon_kitty_state_t *k, uint8_t f);

/* Pop n entries from the stack (CSI < {n} u) */
void ghostcon_kitty_pop(ghostcon_kitty_state_t *k, uint8_t n);

/* Apply set/or/not to current flags (CSI = {mode} {flags} u) */
void ghostcon_kitty_set_mode(ghostcon_kitty_state_t *k,
                             ghostcon_kitty_set_mode_t mode,
                             uint8_t f);

/* Get current flags (top of stack) */
static inline uint8_t
ghostcon_kitty_current(const ghostcon_kitty_state_t *k) {
    return k->stack.flags[k->stack.idx];
}

/* Check if protocol is active (current flags != 0) */
static inline bool
ghostcon_kitty_is_active(const ghostcon_kitty_state_t *k) {
    return ghostcon_kitty_current(k) != 0;
}
