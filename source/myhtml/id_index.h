/*
 Copyright (C) 2015-2017 Alexander Borisov

 This library is free software; you can redistribute it and/or
 modify it under the terms of the GNU Lesser General Public
 License as published by the Free Software Foundation; either
 version 2.1 of the License, or (at your option) any later version.

 This library is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 Lesser General Public License for more details.

 You should have received a copy of the GNU Lesser General Public
 License along with this library; if not, write to the Free Software
 Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

 Author: lex.borisov@gmail.com (Alexander Borisov)
*/

#ifndef MyHTML_ID_INDEX_H
#define MyHTML_ID_INDEX_H
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <myhtml/tree.h>

/* optional id index: the public entry points live in tree.h and api.h */

/*
 * Structural hooks used by the tree internals. They are no-ops while the
 * index is disabled, so the untouched parse path pays nothing.
 */
void myhtml_id_index_on_attach(myhtml_tree_node_t* node);
void myhtml_id_index_on_detach(myhtml_tree_node_t* node);
void myhtml_id_index_on_attribute_changed(myhtml_tree_node_t* node);
void myhtml_id_index_on_node_free(myhtml_tree_node_t* node);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MyHTML_ID_INDEX_H */
