/*
 * Copyright (C) 2026 Alexander Borisov
 *
 * Author: Alexander Borisov <borisov@lexbor.com>
 */

#include <lexbor/html/html.h>
#include <lexbor/html/accessible_name.h>

#include <stdio.h>
#include <string.h>


static void
show_name(lxb_dom_node_t *doc_root, const char *id)
{
    lxb_dom_node_t *node;
    lxb_dom_element_t *element;
    lxb_char_t *name;
    size_t len = 0;

    node = lxb_dom_node_by_id(doc_root, (const lxb_char_t *) id,
                              strlen(id));
    if (node == NULL) {
        printf("%s: element not found\n", id);
        return;
    }

    element = lxb_dom_interface_element(node);
    name = lxb_html_element_accessible_name(element, &len);
    if (name == NULL) {
        printf("%s: allocation failure\n", id);
        return;
    }

    printf("%s: '%.*s'\n", id, (int) len, (const char *) name);

    /* The returned buffer follows the destroy_text ownership convention. */
    lxb_dom_document_destroy_text(node->owner_document, name);
}

int
main(int argc, const char *argv[])
{
    static const char html[] =
        "<label for='login'>User name</label>"
        "<input id='login'>"
        "<button id='go' aria-labelledby='tip'>OK</button>"
        "<span id='tip' hidden>Submit the <b>form</b></span>"
        "<img id='pic' alt='A diagram'>";

    lxb_html_document_t *doc;

    (void) argc;
    (void) argv;

    doc = lxb_html_document_create();
    if (doc == NULL) {
        return EXIT_FAILURE;
    }

    if (lxb_html_document_parse(doc, (const lxb_char_t *) html,
                                sizeof(html) - 1) != LXB_STATUS_OK)
    {
        lxb_html_document_destroy(doc);
        return EXIT_FAILURE;
    }

    show_name(lxb_dom_interface_node(doc), "login");
    show_name(lxb_dom_interface_node(doc), "go");
    show_name(lxb_dom_interface_node(doc), "pic");

    lxb_html_document_destroy(doc);

    return EXIT_SUCCESS;
}
