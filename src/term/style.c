#include "ghostcon/term/style.h"
#include <stdlib.h>
#include <string.h>

/* fg_palette=0/bg_palette=0 would otherwise be indistinguishable from
   "explicitly set to ANSI black via SGR 30/40" — Ghostty's real
   Style.fg_color/bg_color default to a tagged `.none` ("use terminal
   default"), which this flat-flags port represents via the FG/BG_DEFAULT
   bits. Those must be set here, matching this constant's own doc comment
   ("fg=default, bg=default") — found via the renderer actually exercising
   color resolution for the first time; Phase 0's tests never checked
   colors, only codepoints/dirty tracking, so this went unnoticed. */
const ghostcon_style_t GHOSTCON_STYLE_DEFAULT = {
    .flags = GC_STYLE_FG_DEFAULT | GC_STYLE_BG_DEFAULT,
};

/* ------------------------------------------------------------------ */
/* Simple ref-counted style set using open-addressing hash table       */
/* ------------------------------------------------------------------ */

/* Initial capacity for style sets */
#define STYLE_SET_MIN_CAPACITY 16

/* A single entry in the hash table */
typedef struct {
    ghostcon_style_id_t id;       /* 0 = empty slot (id 0 is reserved for default) */
    uint16_t            refcount;
    ghostcon_style_t    style;
} style_entry_t;

struct ghostcon_style_set {
    style_entry_t *entries;
    uint16_t       capacity;
    uint16_t       count;
};

static uint32_t
hash_style(const ghostcon_style_t *s) {
    /* Simple FNV-1a hash over the style bytes */
    uint32_t h = 2166136261u;
    const uint8_t *p = (const uint8_t *)s;
    for (size_t i = 0; i < sizeof(ghostcon_style_t); i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

ghostcon_style_set_t *
ghostcon_style_set_create(uint16_t capacity) {
    if (capacity < STYLE_SET_MIN_CAPACITY)
        capacity = STYLE_SET_MIN_CAPACITY;

    ghostcon_style_set_t *set = (ghostcon_style_set_t *)calloc(1, sizeof(*set));
    if (!set)
        return NULL;

    set->entries = (style_entry_t *)calloc(capacity, sizeof(style_entry_t));
    if (!set->entries) {
        free(set);
        return NULL;
    }

    set->capacity = capacity;
    set->count = 0;
    return set;
}

void
ghostcon_style_set_destroy(ghostcon_style_set_t *set) {
    if (!set)
        return;
    free(set->entries);
    free(set);
}

/* Find an entry by style, return index or -1 if not found */
static int16_t
find_entry(const ghostcon_style_set_t *set, const ghostcon_style_t *style) {
    if (set->capacity == 0)
        return -1;

    uint32_t h = hash_style(style);
    uint16_t idx = h % set->capacity;

    for (uint16_t i = 0; i < set->capacity; i++) {
        uint16_t slot = (idx + i) % set->capacity;
        style_entry_t *e = &set->entries[slot];
        if (e->id == 0)
            return -1; /* empty slot → not found */
        if (ghostcon_style_eq(&e->style, style))
            return (int16_t)slot;
    }
    return -1; /* table full */
}

/* Grow the table to new_capacity */
static bool
grow_table(ghostcon_style_set_t *set, uint16_t new_capacity) {
    style_entry_t *old = set->entries;
    uint16_t old_cap = set->capacity;

    style_entry_t *new = (style_entry_t *)calloc(new_capacity, sizeof(style_entry_t));
    if (!new)
        return false;

    set->entries = new;
    set->capacity = new_capacity;
    set->count = 0;

    /* Rehash all entries */
    for (uint16_t i = 0; i < old_cap; i++) {
        if (old[i].id != 0) {
            /* Re-add */
            uint32_t h = hash_style(&old[i].style);
            uint16_t idx = h % new_capacity;
            for (uint16_t j = 0; j < new_capacity; j++) {
                uint16_t slot = (idx + j) % new_capacity;
                if (new[slot].id == 0) {
                    new[slot] = old[i];
                    set->count++;
                    break;
                }
            }
        }
    }

    free(old);
    return true;
}

ghostcon_style_id_t
ghostcon_style_set_add(ghostcon_style_set_t *set, const ghostcon_style_t *style) {
    /* Default style always maps to ID 0 */
    if (ghostcon_style_eq(style, &GHOSTCON_STYLE_DEFAULT))
        return GC_STYLE_DEFAULT_ID;

    /* Check if already exists */
    int16_t existing = find_entry(set, style);
    if (existing >= 0) {
        set->entries[existing].refcount++;
        return set->entries[existing].id;
    }

    /* Grow if load factor > 0.75 */
    if (set->count >= (uint16_t)(set->capacity * 0.75f)) {
        if (!grow_table(set, set->capacity * 2))
            return GC_STYLE_DEFAULT_ID;
    }

    /* Find empty slot */
    uint32_t h = hash_style(style);
    uint16_t idx = h % set->capacity;

    for (uint16_t i = 0; i < set->capacity; i++) {
        uint16_t slot = (idx + i) % set->capacity;
        if (set->entries[slot].id == 0) {
            /* Assign next available ID (skipping 0) */
            ghostcon_style_id_t new_id = GC_STYLE_DEFAULT_ID + 1;
            while (new_id <= GC_STYLE_ID_MAX) {
                bool used = false;
                /* Check if any existing entry uses this id */
                for (uint16_t j = 0; j < set->capacity; j++) {
                    if (set->entries[j].id == new_id) {
                        used = true;
                        break;
                    }
                }
                if (!used)
                    break;
                new_id++;
            }

            set->entries[slot].id = new_id;
            set->entries[slot].refcount = 1;
            set->entries[slot].style = *style;
            set->count++;
            return new_id;
        }
    }

    return GC_STYLE_DEFAULT_ID; /* shouldn't happen */
}

void
ghostcon_style_set_ref(ghostcon_style_set_t *set, ghostcon_style_id_t id) {
    if (id == GC_STYLE_DEFAULT_ID)
        return;

    for (uint16_t i = 0; i < set->capacity; i++) {
        if (set->entries[i].id == id) {
            set->entries[i].refcount++;
            return;
        }
    }
}

void
ghostcon_style_set_unref(ghostcon_style_set_t *set, ghostcon_style_id_t id) {
    if (id == GC_STYLE_DEFAULT_ID)
        return;

    for (uint16_t i = 0; i < set->capacity; i++) {
        if (set->entries[i].id == id) {
            if (set->entries[i].refcount > 0)
                set->entries[i].refcount--;
            if (set->entries[i].refcount == 0) {
                set->entries[i].id = 0;
                set->count--;
            }
            return;
        }
    }
}

const ghostcon_style_t *
ghostcon_style_set_get(const ghostcon_style_set_t *set, ghostcon_style_id_t id) {
    if (id == GC_STYLE_DEFAULT_ID)
        return &GHOSTCON_STYLE_DEFAULT;

    for (uint16_t i = 0; i < set->capacity; i++) {
        if (set->entries[i].id == id)
            return &set->entries[i].style;
    }
    return &GHOSTCON_STYLE_DEFAULT;
}
