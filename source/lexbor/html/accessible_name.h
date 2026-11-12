/*
 * Copyright (C) 2026 Alexander Borisov
 *
 * Author: Alexander Borisov <borisov@lexbor.com>
 */

#ifndef LEXBOR_HTML_ACCESSIBLE_NAME_H
#define LEXBOR_HTML_ACCESSIBLE_NAME_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lexbor/html/base.h"
#include "lexbor/dom/interfaces/element.h"


/*
 * Computes a static accessible name for an HTML control according to a
 * fixed subset of the WAI-ARIA accessible name and description computation.
 *
 * Supported controls:
 *   button, a[href], img, input (types text, search, checkbox, radio;
 *   missing or unknown type defaults to text), select, textarea, and any
 *   element whose explicit role attribute contains one of:
 *   button, link, checkbox, switch, textbox, combobox.
 *
 * Name sources, in order:
 *   1. aria-labelledby, if it carries at least one valid IDREF;
 *   2. non-empty aria-label;
 *   3. the native name (label elements, alt, contents, form values);
 *   4. the title attribute.
 *
 * The returned buffer is owned by the element's document and must be
 * released with lxb_dom_document_destroy_text(). An empty name is returned
 * as a releaseable zero-length string. NULL means an allocation failure.
 *
 * The DOM tree and its attributes are never modified and nothing is cached
 * between calls.
 */
LXB_API lxb_char_t *
lxb_html_element_accessible_name(lxb_dom_element_t *element, size_t *len);


#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LEXBOR_HTML_ACCESSIBLE_NAME_H */
