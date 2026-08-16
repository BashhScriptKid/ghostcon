#include "ghostcon/term/hyperlink.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Mirrors style.c's open-addressing ref-counted set exactly -- see
   hyperlink.h's doc comment for why. */

#define HYPERLINK_SET_MIN_CAPACITY 16
#define HYPERLINK_ID_MAX 0x7FFF

typedef struct {
    ghostcon_style_id_t id;       /* 0 = empty slot */
    uint16_t             refcount;
    char                  uri[GC_HYPERLINK_URI_MAX];
} hyperlink_entry_t;

struct ghostcon_hyperlink_set {
    hyperlink_entry_t *entries;
    uint16_t            capacity;
    uint16_t            count;
};

static uint32_t
hash_uri(const char *uri) {
    uint32_t h = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)uri; *p; p++) {
        h ^= *p;
        h *= 16777619u;
    }
    return h;
}

ghostcon_hyperlink_set_t *
ghostcon_hyperlink_set_create(uint16_t capacity) {
    if (capacity < HYPERLINK_SET_MIN_CAPACITY)
        capacity = HYPERLINK_SET_MIN_CAPACITY;

    ghostcon_hyperlink_set_t *set = (ghostcon_hyperlink_set_t *)calloc(1, sizeof(*set));
    if (!set)
        return NULL;

    set->entries = (hyperlink_entry_t *)calloc(capacity, sizeof(hyperlink_entry_t));
    if (!set->entries) {
        free(set);
        return NULL;
    }

    set->capacity = capacity;
    set->count = 0;
    return set;
}

void
ghostcon_hyperlink_set_destroy(ghostcon_hyperlink_set_t *set) {
    if (!set)
        return;
    free(set->entries);
    free(set);
}

static int16_t
find_entry(const ghostcon_hyperlink_set_t *set, const char *uri) {
    if (set->capacity == 0)
        return -1;

    uint32_t h = hash_uri(uri);
    uint16_t idx = h % set->capacity;

    for (uint16_t i = 0; i < set->capacity; i++) {
        uint16_t slot = (idx + i) % set->capacity;
        hyperlink_entry_t *e = &set->entries[slot];
        if (e->id == 0)
            return -1;
        if (strcmp(e->uri, uri) == 0)
            return (int16_t)slot;
    }
    return -1;
}

static bool
grow_table(ghostcon_hyperlink_set_t *set, uint16_t new_capacity) {
    hyperlink_entry_t *old = set->entries;
    uint16_t old_cap = set->capacity;

    hyperlink_entry_t *new = (hyperlink_entry_t *)calloc(new_capacity, sizeof(hyperlink_entry_t));
    if (!new)
        return false;

    set->entries = new;
    set->capacity = new_capacity;
    set->count = 0;

    for (uint16_t i = 0; i < old_cap; i++) {
        if (old[i].id != 0) {
            uint32_t h = hash_uri(old[i].uri);
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
ghostcon_hyperlink_set_add(ghostcon_hyperlink_set_t *set, const char *uri) {
    if (!uri || uri[0] == '\0')
        return GC_HYPERLINK_ID_NONE;

    int16_t existing = find_entry(set, uri);
    if (existing >= 0) {
        set->entries[existing].refcount++;
        return set->entries[existing].id;
    }

    if (set->count >= (uint16_t)(set->capacity * 0.75f)) {
        if (!grow_table(set, set->capacity * 2))
            return GC_HYPERLINK_ID_NONE;
    }

    uint32_t h = hash_uri(uri);
    uint16_t idx = h % set->capacity;

    for (uint16_t i = 0; i < set->capacity; i++) {
        uint16_t slot = (idx + i) % set->capacity;
        if (set->entries[slot].id == 0) {
            ghostcon_style_id_t new_id = GC_HYPERLINK_ID_NONE + 1;
            while (new_id <= HYPERLINK_ID_MAX) {
                bool used = false;
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
            snprintf(set->entries[slot].uri, sizeof(set->entries[slot].uri), "%s", uri);
            set->count++;
            return new_id;
        }
    }

    return GC_HYPERLINK_ID_NONE; /* shouldn't happen */
}

void
ghostcon_hyperlink_set_ref(ghostcon_hyperlink_set_t *set, ghostcon_style_id_t id) {
    if (id == GC_HYPERLINK_ID_NONE)
        return;
    for (uint16_t i = 0; i < set->capacity; i++) {
        if (set->entries[i].id == id) {
            set->entries[i].refcount++;
            return;
        }
    }
}

void
ghostcon_hyperlink_set_unref(ghostcon_hyperlink_set_t *set, ghostcon_style_id_t id) {
    if (id == GC_HYPERLINK_ID_NONE)
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

const char *
ghostcon_hyperlink_set_get(const ghostcon_hyperlink_set_t *set, ghostcon_style_id_t id) {
    if (id == GC_HYPERLINK_ID_NONE)
        return NULL;
    for (uint16_t i = 0; i < set->capacity; i++) {
        if (set->entries[i].id == id)
            return set->entries[i].uri;
    }
    return NULL;
}
