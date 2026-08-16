#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "cell.h"

/* ------------------------------------------------------------------ */
/* HyperlinkSet — a ref-counted, id-indexed set of interned URIs        */
/*                                                                     */
/* OSC 8 support. Deliberately mirrors ghostcon_style_set_t's shape     */
/* exactly (style.h/style.c) -- same open-addressing hash table, same   */
/* ref-counted id-reuse scheme -- since cell.h's hyperlink_id field is  */
/* the same 15-bit width as style_id and follows the same "0 = none/    */
/* default" convention. See cell.h's memory-layout doc comment.         */
/* ------------------------------------------------------------------ */

#define GC_HYPERLINK_ID_NONE 0
#define GC_HYPERLINK_URI_MAX 1024

typedef struct ghostcon_hyperlink_set ghostcon_hyperlink_set_t;

ghostcon_hyperlink_set_t *ghostcon_hyperlink_set_create(uint16_t capacity);
void ghostcon_hyperlink_set_destroy(ghostcon_hyperlink_set_t *set);

/* Interns `uri`, returning its id. Repeated calls with the same URI
   string return the same id with an incremented refcount. Returns
   GC_HYPERLINK_ID_NONE if `uri` is empty or the table is full. */
ghostcon_style_id_t ghostcon_hyperlink_set_add(ghostcon_hyperlink_set_t *set, const char *uri);

void ghostcon_hyperlink_set_ref(ghostcon_hyperlink_set_t *set, ghostcon_style_id_t id);
void ghostcon_hyperlink_set_unref(ghostcon_hyperlink_set_t *set, ghostcon_style_id_t id);

/* Returns the URI for `id`, or NULL for GC_HYPERLINK_ID_NONE / an
   id that's since been fully unreffed. */
const char *ghostcon_hyperlink_set_get(const ghostcon_hyperlink_set_t *set, ghostcon_style_id_t id);
