/*
 * Copyright (C) 2026 Alexander Borisov
 *
 * Author: Alexander Borisov <borisov@lexbor.com>
 */

#include "lexbor/html/accessible_name.h"

#include "lexbor/core/str.h"
#include "lexbor/tag/const.h"
#include "lexbor/ns/const.h"
#include "lexbor/dom/interfaces/node.h"
#include "lexbor/dom/interfaces/attr.h"
#include "lexbor/dom/interfaces/text.h"
#include "lexbor/html/interfaces/document.h"
#include "lexbor/html/interfaces/option_element.h"
#include "lexbor/html/interfaces/select_element.h"
#include "lexbor/html/interfaces/text_area_element.h"


typedef enum {
    AN_ROLE_NONE      = 0,
    AN_ROLE_BUTTON    = 1,
    AN_ROLE_LINK      = 2,
    AN_ROLE_CHECKBOX  = 3,
    AN_ROLE_SWITCH    = 4,
    AN_ROLE_TEXTBOX   = 5,
    AN_ROLE_COMBOBOX  = 6
}
an_role_t;

typedef struct {
    /* Accumulated raw name text, document-owned, null terminated. */
    lexbor_str_t       out;

    /* Control currently being named; its own name is never re-entered. */
    lxb_dom_element_t  *target;

    /* Element whose subtree an aria-labelledby traversal entered. */
    lxb_dom_element_t  *ref_root;

    /* aria-labelledby references already consumed in this computation. */
    lxb_dom_element_t  **refs;
    size_t             refs_len;
    size_t             refs_size;

    bool               ok;
}
an_ctx_t;


static bool
an_is_ascii_ws(lxb_char_t c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r';
}

static const lxb_char_t *
an_attr(lxb_dom_element_t *el, const char *name, size_t name_len,
        size_t *value_len)
{
    return lxb_dom_element_get_attribute(el, (const lxb_char_t *) name,
                                         name_len, value_len);
}

static bool
an_is_hidden(lxb_dom_element_t *el)
{
    lxb_dom_node_t *node = lxb_dom_interface_node(el);
    size_t len;

    while (node != NULL && node->type == LXB_DOM_NODE_TYPE_ELEMENT) {
        el = lxb_dom_interface_element(node);

        if (lxb_dom_element_has_attribute(el, (const lxb_char_t *) "hidden", 6)) {
            return true;
        }

        const lxb_char_t *v = an_attr(el, "aria-hidden", 11, &len);
        if (v != NULL && len == 4
            && lexbor_str_data_ncasecmp(v, (const lxb_char_t *) "true", 4))
        {
            return true;
        }

        node = node->parent;
    }

    return false;
}

static an_role_t
an_role_from_token(const lxb_char_t *data, size_t len)
{
    switch (len) {
        case 4:
            if (lexbor_str_data_ncasecmp(data, (const lxb_char_t *) "link", 4)) {
                return AN_ROLE_LINK;
            }

            break;

        case 6:
            if (lexbor_str_data_ncasecmp(data,
                                    (const lxb_char_t *) "button", 6))
            {
                return AN_ROLE_BUTTON;
            }

            if (lexbor_str_data_ncasecmp(data,
                                    (const lxb_char_t *) "switch", 6))
            {
                return AN_ROLE_SWITCH;
            }

            break;

        case 7:
            if (lexbor_str_data_ncasecmp(data,
                                    (const lxb_char_t *) "textbox", 7))
            {
                return AN_ROLE_TEXTBOX;
            }

            break;

        case 8:
            if (lexbor_str_data_ncasecmp(data,
                                    (const lxb_char_t *) "checkbox", 8))
            {
                return AN_ROLE_CHECKBOX;
            }

            if (lexbor_str_data_ncasecmp(data,
                                    (const lxb_char_t *) "combobox", 8))
            {
                return AN_ROLE_COMBOBOX;
            }

            break;

        default:
            break;
    }

    return AN_ROLE_NONE;
}

/* First supported token of the role attribute; tokens are whitespace separated. */
static an_role_t
an_explicit_role(lxb_dom_element_t *el)
{
    size_t len;
    const lxb_char_t *value = an_attr(el, "role", 4, &len);

    if (value == NULL) {
        return AN_ROLE_NONE;
    }

    for (size_t i = 0; i < len; ) {
        size_t begin;
        an_role_t role;

        while (i < len && an_is_ascii_ws(value[i])) {
            i++;
        }

        begin = i;

        while (i < len && !an_is_ascii_ws(value[i])) {
            i++;
        }

        if (i == begin) {
            break;
        }

        role = an_role_from_token(&value[begin], i - begin);
        if (role != AN_ROLE_NONE) {
            return role;
        }
    }

    return AN_ROLE_NONE;
}

static bool
an_input_is_text_type(lxb_dom_element_t *el)
{
    size_t len;
    const lxb_char_t *value = an_attr(el, "type", 4, &len);

    if (value == NULL) {
        return true;
    }

    if (len == 4
        && lexbor_str_data_ncasecmp(value, (const lxb_char_t *) "text", 4))
    {
        return true;
    }

    if (len == 6
        && lexbor_str_data_ncasecmp(value, (const lxb_char_t *) "search", 6))
    {
        return true;
    }

    return false;
}

/*
 * Whether the element is one of the controls covered by this stage.
 * Explicit ARIA roles can promote any element to a name root.
 */
static bool
an_is_named_control(lxb_dom_element_t *el)
{
    lxb_dom_node_t *node = lxb_dom_interface_node(el);
    lxb_tag_id_t tag = node->local_name;

    if (an_explicit_role(el) != AN_ROLE_NONE) {
        return true;
    }

    if (node->ns != LXB_NS_HTML) {
        return false;
    }

    switch (tag) {
        case LXB_TAG_BUTTON:
        case LXB_TAG_IMG:
        case LXB_TAG_SELECT:
        case LXB_TAG_TEXTAREA:
            return true;

        case LXB_TAG_A: {
            size_t len;
            return an_attr(el, "href", 4, &len) != NULL;
        }

        case LXB_TAG_INPUT: {
            size_t len;
            const lxb_char_t *type = an_attr(el, "type", 4, &len);

            if (type == NULL || an_input_is_text_type(el)) {
                return true;
            }

            if (len == 8
                && lexbor_str_data_ncasecmp(type,
                                (const lxb_char_t *) "checkbox", 8))
            {
                return true;
            }

            if (len == 5
                && lexbor_str_data_ncasecmp(type,
                                (const lxb_char_t *) "radio", 5))
            {
                return true;
            }

            return false;
        }

        default:
            return false;
    }
}

/* label[for] may only point at an HTML labelable element. */
static bool
an_is_labelable(lxb_dom_element_t *el)
{
    lxb_dom_node_t *node = lxb_dom_interface_node(el);

    if (node->ns != LXB_NS_HTML) {
        return false;
    }

    switch (node->local_name) {
        case LXB_TAG_BUTTON:
        case LXB_TAG_INPUT:
        case LXB_TAG_SELECT:
        case LXB_TAG_TEXTAREA:
            return true;

        default:
            return false;
    }
}

static bool
an_append_data(an_ctx_t *ctx, const lxb_char_t *data, size_t len)
{
    if (len == 0) {
        return true;
    }

    if (lexbor_str_append(&ctx->out,
            lxb_dom_interface_node(ctx->target)->owner_document->text,
            data, len) == NULL)
    {
        ctx->ok = false;
        return false;
    }

    return true;
}

static void
an_append_space(an_ctx_t *ctx)
{
    if (ctx->out.length > 0 && ctx->out.data[ctx->out.length - 1] != ' ') {
        an_append_data(ctx, (const lxb_char_t *) " ", 1);
    }
}

static void
an_collect_children(lxb_dom_node_t *parent, an_ctx_t *ctx, bool honor_hidden);

static void
an_collect_subtree(lxb_dom_node_t *root, an_ctx_t *ctx, bool honor_hidden);

static bool
an_is_embedded_value_control(lxb_dom_element_t *el)
{
    lxb_dom_node_t *node = lxb_dom_interface_node(el);

    if (node->ns != LXB_NS_HTML) {
        return false;
    }

    switch (node->local_name) {
        case LXB_TAG_INPUT:
            return an_input_is_text_type(el);

        case LXB_TAG_TEXTAREA:
        case LXB_TAG_SELECT:
            return true;

        default:
            return false;
    }
}

/* Selected options live below select elements and optgroups. */
static void
an_select_append_options(lxb_dom_node_t *node, an_ctx_t *ctx)
{
    node = node->first_child;

    while (node != NULL) {
        if (node->type == LXB_DOM_NODE_TYPE_ELEMENT
            && node->ns == LXB_NS_HTML)
        {
            if (node->local_name == LXB_TAG_OPTION) {
                lxb_html_option_element_t *option;

                option = lxb_html_interface_option(node);

                if (lxb_html_option_element_selectedness(option)) {
                    an_append_space(ctx);
                    an_collect_subtree(node, ctx, true);
                }
            }
            else if (node->local_name == LXB_TAG_OPTGROUP) {
                an_select_append_options(node, ctx);
            }
        }

        node = node->next;
    }
}

/* Emit the current DOM value of an embedded form control as name text. */
static void
an_append_control_value(lxb_dom_element_t *el, an_ctx_t *ctx)
{
    lxb_dom_node_t *node = lxb_dom_interface_node(el);
    size_t len;

    if (node->ns != LXB_NS_HTML) {
        return;
    }

    switch (node->local_name) {
        case LXB_TAG_INPUT: {
            const lxb_char_t *value;

            if (!an_input_is_text_type(el)) {
                return;
            }

            value = an_attr(el, "value", 5, &len);
            if (value != NULL) {
                an_append_space(ctx);
                an_append_data(ctx, value, len);
            }

            return;
        }

        case LXB_TAG_TEXTAREA: {
            lxb_dom_node_t *child = node->first_child;

            an_append_space(ctx);

            while (child != NULL) {
                if (child->type == LXB_DOM_NODE_TYPE_TEXT) {
                    lxb_dom_text_t *text = lxb_dom_interface_text(child);

                    an_append_data(ctx, text->char_data.data.data,
                                   text->char_data.data.length);
                }

                child = child->next;
            }

            return;
        }

        case LXB_TAG_SELECT:
            an_select_append_options(node, ctx);
            return;

        default:
            return;
    }
}

static void
an_collect_children(lxb_dom_node_t *parent, an_ctx_t *ctx, bool honor_hidden)
{
    lxb_dom_node_t *node = parent->first_child;

    while (node != NULL) {
        if (node->type == LXB_DOM_NODE_TYPE_TEXT) {
            lxb_dom_text_t *text = lxb_dom_interface_text(node);

            an_append_data(ctx, text->char_data.data.data,
                           text->char_data.data.length);

            node = node->next;
            continue;
        }

        if (node->type != LXB_DOM_NODE_TYPE_ELEMENT) {
            node = node->next;
            continue;
        }

        lxb_dom_element_t *el = lxb_dom_interface_element(node);
        lxb_tag_id_t tag = node->local_name;

        if (node->ns == LXB_NS_HTML
            && (tag == LXB_TAG_SCRIPT || tag == LXB_TAG_STYLE
                || tag == LXB_TAG_TEMPLATE))
        {
            node = node->next;
            continue;
        }

        if (honor_hidden && an_is_hidden(el)) {
            node = node->next;
            continue;
        }

        /*
         * Do not re-enter the control being named (e.g. a label wrapping it):
         * its own name machinery would recurse. A direct aria-labelledby
         * reference to itself is still rendered from this root.
         */
        if (el == ctx->target && el != ctx->ref_root) {
            node = node->next;
            continue;
        }

        /*
         * Inside a traversal, aria-label replaces the descendant's contents;
         * nested aria-labelledby is deliberately not followed here.
         */
        size_t label_len;
        const lxb_char_t *label = an_attr(el, "aria-label", 10, &label_len);

        if (label != NULL && label_len != 0) {
            an_append_data(ctx, label, label_len);
            node = node->next;
            continue;
        }

        if (node->ns == LXB_NS_HTML && tag == LXB_TAG_BR) {
            an_append_space(ctx);
            node = node->next;
            continue;
        }

        /* An embedded image contributes its alt text inline. */
        if (node->ns == LXB_NS_HTML && tag == LXB_TAG_IMG) {
            size_t len;
            const lxb_char_t *alt = an_attr(el, "alt", 3, &len);

            if (alt != NULL) {
                an_append_data(ctx, alt, len);
            }

            node = node->next;
            continue;
        }

        if (el != ctx->target && an_is_embedded_value_control(el)) {
            an_append_control_value(el, ctx);
            node = node->next;
            continue;
        }

        an_collect_children(node, ctx, honor_hidden);
        if (!ctx->ok) {
            return;
        }

        node = node->next;
    }
}

/* Collect the descendants of root; the root element itself is not revisited. */
static void
an_collect_subtree(lxb_dom_node_t *root, an_ctx_t *ctx, bool honor_hidden)
{
    an_collect_children(root, ctx, honor_hidden);
}

static lxb_dom_node_t *
an_first_labelable_descendant(lxb_dom_node_t *root)
{
    lxb_dom_node_t *node = root->first_child;

    while (node != NULL) {
        if (node->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            lxb_dom_element_t *el = lxb_dom_interface_element(node);

            if (an_is_labelable(el)) {
                return node;
            }

            lxb_dom_node_t *found = an_first_labelable_descendant(node);
            if (found != NULL) {
                return found;
            }
        }

        node = node->next;
    }

    return NULL;
}

static bool
an_label_targets(lxb_dom_element_t *label, an_ctx_t *ctx)
{
    lxb_dom_node_t *doc_node;
    size_t len;
    const lxb_char_t *id;

    id = an_attr(label, "for", 3, &len);

    if (id != NULL) {
        lxb_dom_node_t *match;

        doc_node = lxb_dom_interface_node(
                            lxb_dom_interface_node(ctx->target)->owner_document);

        match = lxb_dom_node_by_id(doc_node, id, len);
        if (match == NULL || match->type != LXB_DOM_NODE_TYPE_ELEMENT) {
            return false;
        }

        lxb_dom_element_t *candidate = lxb_dom_interface_element(match);

        return candidate == ctx->target && an_is_labelable(candidate);
    }

    /* A wrapping label associates with its first labelable descendant. */
    lxb_dom_node_t *control;

    control = an_first_labelable_descendant(lxb_dom_interface_node(label));

    return control != NULL
        && lxb_dom_interface_element(control) == ctx->target;
}

static void
an_collect_labels(lxb_dom_node_t *node, an_ctx_t *ctx)
{
    while (node != NULL) {
        if (node->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            lxb_dom_element_t *el = lxb_dom_interface_element(node);

            if (node->ns == LXB_NS_HTML
                && node->local_name == LXB_TAG_LABEL
                && an_label_targets(el, ctx))
            {
                an_append_space(ctx);
                an_collect_subtree(node, ctx, true);
                if (!ctx->ok) {
                    return;
                }
            }
            else if (node->first_child != NULL) {
                an_collect_labels(node->first_child, ctx);
                if (!ctx->ok) {
                    return;
                }
            }
        }

        node = node->next;
    }
}

static void
an_native_name(lxb_dom_element_t *el, an_role_t role, an_ctx_t *ctx)
{
    lxb_dom_node_t *node = lxb_dom_interface_node(el);
    lxb_tag_id_t tag = node->local_name;
    size_t len;

    if (node->ns == LXB_NS_HTML && tag == LXB_TAG_IMG) {
        const lxb_char_t *alt = an_attr(el, "alt", 3, &len);

        if (alt != NULL) {
            an_append_data(ctx, alt, len);
        }

        return;
    }

    if (node->ns == LXB_NS_HTML
        && (tag == LXB_TAG_INPUT || tag == LXB_TAG_SELECT
            || tag == LXB_TAG_TEXTAREA))
    {
        lxb_dom_node_t *doc_node;

        doc_node = lxb_dom_interface_node(node->owner_document);

        /* Real associated labels, joined in document order. */
        an_collect_labels(doc_node->first_child, ctx);

        return;
    }

    (void) role;

    /* Buttons, links and role-named containers take their contents. */
    an_collect_subtree(node, ctx, true);
}

static bool
an_ref_seen(an_ctx_t *ctx, lxb_dom_element_t *el)
{
    for (size_t i = 0; i < ctx->refs_len; i++) {
        if (ctx->refs[i] == el) {
            return true;
        }
    }

    return false;
}

static bool
an_ref_push(an_ctx_t *ctx, lxb_dom_element_t *el)
{
    if (ctx->refs_len == ctx->refs_size) {
        size_t new_size = ctx->refs_size == 0 ? 4 : ctx->refs_size * 2;
        lxb_dom_element_t **tmp;

        tmp = lexbor_realloc(ctx->refs,
                             sizeof(lxb_dom_element_t *) * new_size);
        if (tmp == NULL) {
            ctx->ok = false;
            return false;
        }

        ctx->refs = tmp;
        ctx->refs_size = new_size;
    }

    ctx->refs[ctx->refs_len++] = el;

    return true;
}

/*
 * Renders one referenced element. Nested aria-labelledby attributes are not
 * followed here; aria-label and ordinary text still contribute, so mutually
 * referencing elements cannot loop forever.
 */
static void
an_render_reference(lxb_dom_element_t *el, an_ctx_t *ctx)
{
    size_t len;
    const lxb_char_t *label;

    label = an_attr(el, "aria-label", 10, &len);
    if (label != NULL && len != 0) {
        an_append_data(ctx, label, len);
        return;
    }

    /* A hidden reference root exposes its whole subtree. */
    bool honor_hidden = !an_is_hidden(el);

    ctx->ref_root = el;
    an_collect_subtree(lxb_dom_interface_node(el), ctx, honor_hidden);
    ctx->ref_root = NULL;
}

static bool
an_has_visible_text(an_ctx_t *ctx)
{
    for (size_t i = 0; i < ctx->out.length; i++) {
        if (!an_is_ascii_ws(ctx->out.data[i])) {
            return true;
        }
    }

    return false;
}

/* Returns true when aria-labelledby carried at least one valid IDREF. */
static bool
an_labelledby(lxb_dom_element_t *el, an_ctx_t *ctx)
{
    size_t len;
    const lxb_char_t *value = an_attr(el, "aria-labelledby", 15, &len);

    if (value == NULL) {
        return false;
    }

    lxb_dom_node_t *doc_node;
    bool any = false;

    doc_node = lxb_dom_interface_node(
                        lxb_dom_interface_node(el)->owner_document);

    for (size_t i = 0; i < len; ) {
        size_t begin;
        lxb_dom_node_t *match;

        while (i < len && an_is_ascii_ws(value[i])) {
            i++;
        }

        begin = i;

        while (i < len && !an_is_ascii_ws(value[i])) {
            i++;
        }

        if (i == begin) {
            break;
        }

        match = lxb_dom_node_by_id(doc_node, &value[begin], i - begin);
        if (match == NULL || match->type != LXB_DOM_NODE_TYPE_ELEMENT) {
            continue;
        }

        lxb_dom_element_t *ref = lxb_dom_interface_element(match);

        /* The same reference in one list contributes only once. */
        if (an_ref_seen(ctx, ref)) {
            any = true;
            continue;
        }

        if (!an_ref_push(ctx, ref)) {
            return false;
        }

        any = true;

        an_append_space(ctx);
        an_render_reference(ref, ctx);

        if (!ctx->ok) {
            return false;
        }
    }

    return any;
}

/*
 * Collapse every run of ASCII whitespace into one space and trim both ends.
 * Non-ASCII bytes are copied untouched.
 */
static void
an_normalize(lexbor_str_t *str)
{
    lxb_char_t *data = str->data;
    size_t i = 0, offset = 0;
    bool in_ws = false;

    while (i < str->length) {
        if (an_is_ascii_ws(data[i])) {
            in_ws = true;
            i++;
            continue;
        }

        if (in_ws && offset != 0) {
            data[offset++] = ' ';
        }

        data[offset++] = data[i++];
        in_ws = false;
    }

    data[offset] = 0x00;
    str->length = offset;
}

static lxb_char_t *
an_finish(an_ctx_t *ctx, size_t *out_len)
{
    lxb_dom_document_t *doc;
    lxb_char_t *result;

    doc = lxb_dom_interface_node(ctx->target)->owner_document;

    if (!ctx->ok) {
        if (ctx->out.data != NULL) {
            lxb_dom_document_destroy_text(doc, ctx->out.data);
        }

        lexbor_free(ctx->refs);

        if (out_len != NULL) {
            *out_len = 0;
        }

        return NULL;
    }

    an_normalize(&ctx->out);

    result = ctx->out.data;

    if (out_len != NULL) {
        *out_len = ctx->out.length;
    }

    ctx->out.data = NULL;
    ctx->out.length = 0;

    lexbor_free(ctx->refs);

    return result;
}

lxb_char_t *
lxb_html_element_accessible_name(lxb_dom_element_t *element, size_t *len)
{
    an_ctx_t ctx;
    an_role_t role;
    size_t value_len;
    const lxb_char_t *value;
    lxb_dom_document_t *doc;

    if (element == NULL) {
        if (len != NULL) {
            *len = 0;
        }

        return NULL;
    }

    doc = lxb_dom_interface_node(element)->owner_document;

    memset(&ctx, 0, sizeof(an_ctx_t));
    ctx.target = element;
    ctx.ok = true;

    if (lexbor_str_init(&ctx.out, doc->text, 1) == NULL) {
        if (len != NULL) {
            *len = 0;
        }

        return NULL;
    }

    role = an_explicit_role(element);

    if (role == AN_ROLE_NONE && !an_is_named_control(element)) {
        /* Unsupported root: still return a releaseable empty string. */
        return an_finish(&ctx, len);
    }

    /* 1. aria-labelledby wins when at least one IDREF resolves. */
    if (an_labelledby(element, &ctx) && ctx.ok && an_has_visible_text(&ctx)) {
        return an_finish(&ctx, len);
    }

    if (!ctx.ok) {
        return an_finish(&ctx, len);
    }

    /* Resolved references produced nothing: keep trying lower-priority names. */

    /* 2. Non-empty aria-label. */
    value = an_attr(element, "aria-label", 10, &value_len);
    if (value != NULL && value_len != 0) {
        an_append_data(&ctx, value, value_len);
        return an_finish(&ctx, len);
    }

    /* 3. Native name. */
    an_native_name(element, role, &ctx);
    if (!ctx.ok) {
        return an_finish(&ctx, len);
    }

    if (an_has_visible_text(&ctx)) {
        return an_finish(&ctx, len);
    }

    /* 4. title attribute as the last resort. */
    value = an_attr(element, "title", 5, &value_len);
    if (value != NULL && value_len != 0) {
        an_append_data(&ctx, value, value_len);
    }

    return an_finish(&ctx, len);
}
