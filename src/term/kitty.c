#include "ghostcon/term/kitty.h"

void
ghostcon_kitty_push(ghostcon_kitty_state_t *k, uint8_t f) {
    /* Overflow wraps around, evicting oldest entry */
    k->stack.idx = (k->stack.idx + 1) % GC_KITTY_STACK_DEPTH;
    k->stack.flags[k->stack.idx] = f;
}

void
ghostcon_kitty_pop(ghostcon_kitty_state_t *k, uint8_t n) {
    /* If n >= depth, reset entire stack */
    if (n >= GC_KITTY_STACK_DEPTH) {
        for (int i = 0; i < GC_KITTY_STACK_DEPTH; i++)
            k->stack.flags[i] = 0;
        k->stack.idx = 0;
        return;
    }

    for (uint8_t i = 0; i < n; i++) {
        k->stack.flags[k->stack.idx] = 0;
        k->stack.idx = (k->stack.idx == 0)
            ? (GC_KITTY_STACK_DEPTH - 1)
            : (k->stack.idx - 1);
    }
}

void
ghostcon_kitty_set_mode(ghostcon_kitty_state_t *k,
                        ghostcon_kitty_set_mode_t mode,
                        uint8_t f)
{
    switch (mode) {
    case GC_KITTY_SET:
        k->stack.flags[k->stack.idx] = f;
        break;
    case GC_KITTY_OR:
        k->stack.flags[k->stack.idx] |= f;
        break;
    case GC_KITTY_NOT:
        k->stack.flags[k->stack.idx] &= ~f;
        break;
    }
}
