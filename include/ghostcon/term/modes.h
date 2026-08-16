#pragma once

#include <stdint.h>
#include <stdbool.h>

/* DEC/ANSI terminal modes bitfield */

typedef uint64_t ghostcon_modes_t;

/* Bit positions for each mode */
#define GC_MODE_IRM        0   /* Insert/Replace (ANSI) */
#define GC_MODE_KAM        1   /* Keyboard Action */
#define GC_MODE_SRM        2   /* Send/Receive (echo) */
#define GC_MODE_LNM        3   /* Line Feed/New Line */
#define GC_MODE_DECCKM     4   /* Cursor Keys (DEC) */
#define GC_MODE_DECCOLM    5   /* 132 Columns */
#define GC_MODE_DECSCNM    6   /* Screen Reverse */
#define GC_MODE_DECOM      7   /* Origin */
#define GC_MODE_DECAWM     8   /* Auto Wrap */
#define GC_MODE_DECARM     9   /* Auto Repeat */
#define GC_MODE_DECTCEM    10  /* Text Cursor Enable */
#define GC_MODE_DECNKM     11  /* Numeric Keypad */
#define GC_MODE_DECPAM     12  /* Application Keypad */
#define GC_MODE_X10_MOUSE  13
#define GC_MODE_VT200_MOUSE 14
#define GC_MODE_BTN_EVENT_MOUSE 15
#define GC_MODE_ANY_EVENT_MOUSE 16
#define GC_MODE_FOCUS_EVENT 17
#define GC_MODE_EXT_MOUSE  18
#define GC_MODE_SGR_MOUSE  19
#define GC_MODE_ALT_SCREEN 20
#define GC_MODE_BRACKETED_PASTE 21
#define GC_MODE_SYNCHRONIZED_OUTPUT 22
#define GC_MODE_APPLICATION_CURSOR 23
#define GC_MODE_ORIGIN 24
#define GC_MODE_AUTO_WRAP 25
#define GC_MODE_REVERSE_VIDEO 26
#define GC_MODE_INSERT 27

static inline void ghostcon_modes_set(ghostcon_modes_t *m, int bit) {
    *m |= ((uint64_t)1 << bit);
}

static inline void ghostcon_modes_reset(ghostcon_modes_t *m, int bit) {
    *m &= ~((uint64_t)1 << bit);
}

static inline bool ghostcon_modes_get(const ghostcon_modes_t *m, int bit) {
    return (*m >> bit) & 1;
}

static inline void ghostcon_modes_clear(ghostcon_modes_t *m) {
    *m = 0;
}
