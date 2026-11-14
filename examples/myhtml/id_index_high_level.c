/*
 Copyright (C) 2015-2017 Alexander Borisov

 This library is free software; you can redistribute it and/or
 modify it under the terms of the GNU Lesser General Public
 License as published by the Free Software Foundation; either
 version 2.1 of the License, or (at your option) any later version.

 This library is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 Lesser General Public License for more details.

 You should have received a copy of the GNU Lesser General Public
 License along with this library; if not, write to the Free Software
 Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA

 Author: lex.borisov@gmail.com (Alexander Borisov)
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <myhtml/api.h>

#include "example.h"

/*
 * Collect records by a repeated id and keep every original pointer in
 * document order. The caller decides whether the first hit wins or a
 * duplicate should be flagged.
 */

static const char *records_html =
    "<!DOCTYPE html>"
    "<html><head><title>records</title></head><body>"
    "<section id='row-7' data-row='first' class='record'>first copy</section>"
    "<section id='row-7' data-row='second' class='record'>edited copy</section>"
    "<section id='row-9' data-row='third' class='record'>unique</section>"
    "</body></html>";

int main(void)
{
    mystatus_t status;

    /* basic init */
    myhtml_t* myhtml = myhtml_create();
    if(myhtml == NULL)
        return EXIT_FAILURE;

    if(myhtml_init(myhtml, MyHTML_OPTIONS_PARSE_MODE_SINGLE, 1, 0) != MyHTML_STATUS_OK) {
        myhtml_destroy(myhtml);
        return EXIT_FAILURE;
    }

    /* init tree */
    myhtml_tree_t* tree = myhtml_tree_create();
    if(tree == NULL) {
        myhtml_destroy(myhtml);
        return EXIT_FAILURE;
    }

    if(myhtml_tree_init(tree, myhtml) != MyHTML_STATUS_OK) {
        myhtml_tree_destroy(tree);
        myhtml_destroy(myhtml);
        return EXIT_FAILURE;
    }

    /* parse html */
    if(myhtml_parse(tree, MyENCODING_UTF_8, records_html, strlen(records_html)) != MyHTML_STATUS_OK) {
        myhtml_tree_destroy(tree);
        myhtml_destroy(myhtml);
        return EXIT_FAILURE;
    }

    /*
     * Build the id index once. Without this the query below returns NULL
     * with MyHTML_STATUS_ERROR; the attribute based queries keep working.
     */
    if(myhtml_id_index_enable(tree) != MyHTML_STATUS_OK) {
        myhtml_tree_destroy(tree);
        myhtml_destroy(myhtml);
        return EXIT_FAILURE;
    }

    /* query the bucket for one id value */
    myhtml_collection_t *collection = myhtml_get_nodes_by_id(tree, NULL, "row-7", 5, &status);

    if(collection == NULL || status != MyHTML_STATUS_OK) {
        fprintf(stderr, "id query failed: %u\n", (unsigned)status);
    }
    else {
        printf("row-7 matches: " MyCORE_FMT_Z "\n", collection->length);

        for(size_t i = 0; i < collection->length; i++) {
            myhtml_tree_node_t *node = collection->list[i];
            size_t value_length = 0;

            const char *row = myhtml_attribute_value(
                myhtml_attribute_by_key(node, "data-row", 8), &value_length);

            printf("  [%s%s] %.*s\n", (i == 0 ? "first" : "duplicate"), "",
                   (int)value_length, row ? row : "");
        }
    }

    /*
     * Reuse the same collection for another value: the old length is reset,
     * the collection itself stays owned by us.
     */
    collection = myhtml_get_nodes_by_id(tree, collection, "row-9", 5, &status);

    if(collection != NULL && status == MyHTML_STATUS_OK)
        printf("row-9 matches: " MyCORE_FMT_Z "\n", collection->length);

    myhtml_collection_destroy(collection);

    /* changes made through the public API keep the index in sync */
    {
        myhtml_collection_t *rows = myhtml_get_nodes_by_attribute_value(tree, NULL, NULL, false,
                                                                        "data-row", 8,
                                                                        "second", 6, NULL);

        if(rows && rows->length == 1) {
            myhtml_tree_node_t *second = rows->list[0];

            myhtml_node_remove(second);
            myhtml_node_append_child(myhtml_tree_get_node_html(tree), second);

            collection = myhtml_get_nodes_by_id(tree, NULL, "row-7", 5, &status);
            printf("row-7 after move (still two): " MyCORE_FMT_Z "\n",
                   collection ? collection->length : (size_t)0);

            myhtml_collection_destroy(collection);
        }

        myhtml_collection_destroy(rows);
    }

    /* release resources */
    myhtml_id_index_disable(tree);
    myhtml_tree_destroy(tree);
    myhtml_destroy(myhtml);

    return EXIT_SUCCESS;
}
