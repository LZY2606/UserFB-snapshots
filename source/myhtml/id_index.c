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

#include "myhtml/id_index.h"
#include "myhtml/myhtml.h"

/* -------------------------------------------------------------------------
 * The index maps the decoded UTF-8 bytes of a node's first "id" attribute
 * value to every in-document node carrying that value. Each node is a
 * member of at most one bucket (node->id_index_entry), so a node never
 * appears twice in a result set.
 * ------------------------------------------------------------------------- */

struct myhtml_id_index_entry {
    myhtml_tree_node_t* node;

    struct myhtml_id_index_bucket* bucket;
    struct myhtml_id_index_entry* next;
    struct myhtml_id_index_entry* prev;
};

struct myhtml_id_index_bucket {
    char* value;
    size_t length;

    struct myhtml_id_index_entry* first;
    size_t count;

    struct myhtml_id_index_bucket* next;
};

struct myhtml_id_index {
    struct myhtml_id_index_bucket** table;
    size_t table_size;
    size_t buckets;
};

#define MyHTML_ID_INDEX_INITIAL_SIZE 128
#define MyHTML_ID_INDEX_MAX_LOAD     8

static size_t myhtml_id_index_hash(const char* value, size_t length, size_t table_size)
{
    /* FNV-1a over the raw decoded bytes */
    size_t hash = (size_t)2166136261u;
    size_t i;

    for(i = 0; i < length; i++) {
        hash ^= (unsigned char)value[i];
        hash *= (size_t)16777619u;
    }

    return hash % table_size;
}

static struct myhtml_id_index * myhtml_id_index_get(myhtml_tree_t* tree)
{
    if(tree == NULL)
        return NULL;

    return (struct myhtml_id_index *)tree->id_index;
}

static bool myhtml_id_index_node_in_scope(myhtml_tree_node_t* node, myhtml_tree_t* tree)
{
    myhtml_tree_node_t* root = tree->document;

    if(root == NULL)
        return false;

    while(node) {
        if(node == root)
            return true;

        node = node->parent;
    }

    return false;
}

/* the effective id is the value of the first attribute found by
 * myhtml_attribute_by_key(node, "id", 2); an empty value is not indexed */
static mycore_string_t * myhtml_id_index_node_value(myhtml_tree_node_t* node)
{
    myhtml_token_attr_t* attr;

    if(node == NULL || node->token == NULL)
        return NULL;

    attr = myhtml_attribute_by_key(node, "id", 2);
    if(attr == NULL || attr->value.data == NULL || attr->value.length == 0)
        return NULL;

    return &attr->value;
}

static mystatus_t myhtml_id_index_rehash(struct myhtml_id_index* index)
{
    size_t new_size = index->table_size << 1;
    struct myhtml_id_index_bucket** table;

    table = (struct myhtml_id_index_bucket**)
        mycore_calloc(new_size, sizeof(struct myhtml_id_index_bucket*));

    if(table == NULL)
        return MyHTML_STATUS_ERROR_MEMORY_ALLOCATION;

    for(size_t i = 0; i < index->table_size; i++) {
        struct myhtml_id_index_bucket* bucket = index->table[i];

        while(bucket) {
            struct myhtml_id_index_bucket* next = bucket->next;
            size_t hash_id = myhtml_id_index_hash(bucket->value, bucket->length, new_size);

            bucket->next = table[hash_id];
            table[hash_id] = bucket;

            bucket = next;
        }
    }

    mycore_free(index->table);
    index->table      = table;
    index->table_size = new_size;

    return MyHTML_STATUS_OK;
}

static struct myhtml_id_index_bucket * myhtml_id_index_bucket_find(struct myhtml_id_index* index,
                                                                   const char* value, size_t length)
{
    struct myhtml_id_index_bucket* bucket;
    size_t hash_id = myhtml_id_index_hash(value, length, index->table_size);

    for(bucket = index->table[hash_id]; bucket; bucket = bucket->next) {
        if(bucket->length == length && memcmp(bucket->value, value, length) == 0)
            return bucket;
    }

    return NULL;
}

static void myhtml_id_index_bucket_unlink(struct myhtml_id_index* index,
                                          struct myhtml_id_index_bucket* bucket)
{
    size_t hash_id = myhtml_id_index_hash(bucket->value, bucket->length, index->table_size);
    struct myhtml_id_index_bucket** link = &index->table[hash_id];

    while(*link && *link != bucket)
        link = &(*link)->next;

    if(*link == bucket)
        *link = bucket->next;

    mycore_free(bucket->value);
    mycore_free(bucket);

    if(index->buckets)
        index->buckets--;
}

static void myhtml_id_index_entry_unlink(struct myhtml_id_index* index,
                                         struct myhtml_id_index_entry* entry)
{
    struct myhtml_id_index_bucket* bucket = entry->bucket;

    if(entry->prev)
        entry->prev->next = entry->next;
    else
        bucket->first = entry->next;

    if(entry->next)
        entry->next->prev = entry->prev;

    entry->node->id_index_entry = NULL;
    entry->bucket = NULL;
    entry->next = NULL;
    entry->prev = NULL;
    bucket->count--;

    if(bucket->count == 0)
        myhtml_id_index_bucket_unlink(index, bucket);

    mycore_free(entry);
}

static mystatus_t myhtml_id_index_entry_add(struct myhtml_id_index* index,
                                            myhtml_tree_node_t* node,
                                            const char* value, size_t length)
{
    struct myhtml_id_index_bucket* bucket;
    struct myhtml_id_index_entry* entry;
    size_t hash_id;

    bucket = myhtml_id_index_bucket_find(index, value, length);

    if(bucket == NULL) {
        if(index->buckets >= (index->table_size * MyHTML_ID_INDEX_MAX_LOAD)) {
            mystatus_t status = myhtml_id_index_rehash(index);

            if(status)
                return status;
        }

        bucket = (struct myhtml_id_index_bucket*)mycore_calloc(1, sizeof(struct myhtml_id_index_bucket));
        if(bucket == NULL)
            return MyHTML_STATUS_ERROR_MEMORY_ALLOCATION;

        bucket->value = (char*)mycore_malloc(length + 1);
        if(bucket->value == NULL) {
            mycore_free(bucket);
            return MyHTML_STATUS_ERROR_MEMORY_ALLOCATION;
        }

        memcpy(bucket->value, value, length);
        bucket->value[length] = '\0';
        bucket->length = length;

        hash_id = myhtml_id_index_hash(value, length, index->table_size);
        bucket->next = index->table[hash_id];
        index->table[hash_id] = bucket;

        index->buckets++;
    }

    entry = (struct myhtml_id_index_entry*)mycore_calloc(1, sizeof(struct myhtml_id_index_entry));
    if(entry == NULL)
        return MyHTML_STATUS_ERROR_MEMORY_ALLOCATION;

    entry->node   = node;
    entry->bucket = bucket;
    entry->next   = bucket->first;

    if(bucket->first)
        bucket->first->prev = entry;

    bucket->first = entry;
    bucket->count++;

    node->id_index_entry = entry;

    return MyHTML_STATUS_OK;
}

static void myhtml_id_index_node_detach(myhtml_tree_t* tree, myhtml_tree_node_t* node)
{
    struct myhtml_id_index* index = myhtml_id_index_get(tree);
    myhtml_tree_node_t* root = node;

    if(index == NULL)
        return;

    while(node) {
        if(node->id_index_entry)
            myhtml_id_index_entry_unlink(index, (struct myhtml_id_index_entry*)node->id_index_entry);

        if(node->child) {
            node = node->child;
        }
        else {
            while(node != root && node->next == NULL)
                node = node->parent;

            if(node == root)
                break;

            node = node->next;
        }
    }
}

static mystatus_t myhtml_id_index_node_add(myhtml_tree_t* tree, myhtml_tree_node_t* node)
{
    struct myhtml_id_index* index = myhtml_id_index_get(tree);
    mycore_string_t* value;

    value = myhtml_id_index_node_value(node);
    if(value == NULL)
        return MyHTML_STATUS_OK;

    return myhtml_id_index_entry_add(index, node, value->data, value->length);
}

static mystatus_t myhtml_id_index_node_attach(myhtml_tree_t* tree, myhtml_tree_node_t* node)
{
    myhtml_tree_node_t* root = node;
    mystatus_t status;

    while(node) {
        if(node->id_index_entry == NULL) {
            status = myhtml_id_index_node_add(tree, node);

            if(status)
                return status;
        }

        if(node->child) {
            node = node->child;
        }
        else {
            while(node != root && node->next == NULL)
                node = node->parent;

            if(node == root)
                break;

            node = node->next;
        }
    }

    return MyHTML_STATUS_OK;
}

static void myhtml_id_index_free(struct myhtml_id_index* index)
{
    if(index == NULL)
        return;

    for(size_t i = 0; i < index->table_size; i++) {
        struct myhtml_id_index_bucket* bucket = index->table[i];

        while(bucket) {
            struct myhtml_id_index_bucket* next_bucket = bucket->next;
            struct myhtml_id_index_entry* entry = bucket->first;

            while(entry) {
                struct myhtml_id_index_entry* next_entry = entry->next;

                if(entry->node)
                    entry->node->id_index_entry = NULL;

                mycore_free(entry);
                entry = next_entry;
            }

            mycore_free(bucket->value);
            mycore_free(bucket);
            bucket = next_bucket;
        }
    }

    mycore_free(index->table);
    mycore_free(index);
}

mystatus_t myhtml_id_index_enable(myhtml_tree_t* tree)
{
    struct myhtml_id_index* index;
    myhtml_tree_node_t* node;
    myhtml_tree_node_t* root;
    size_t table_size = MyHTML_ID_INDEX_INITIAL_SIZE;
    mystatus_t status;

    if(tree == NULL)
        return MyHTML_STATUS_ERROR;

    if(tree->id_index)
        return MyHTML_STATUS_OK;

    index = (struct myhtml_id_index*)mycore_calloc(1, sizeof(struct myhtml_id_index));
    if(index == NULL)
        return MyHTML_STATUS_ERROR_MEMORY_ALLOCATION;

    index->table = (struct myhtml_id_index_bucket**)
        mycore_calloc(table_size, sizeof(struct myhtml_id_index_bucket*));

    if(index->table == NULL) {
        mycore_free(index);
        return MyHTML_STATUS_ERROR_MEMORY_ALLOCATION;
    }

    index->table_size = table_size;
    index->buckets    = 0;
    tree->id_index    = index;

    /* one scan of the current document; later updates touch only the
     * affected subtree and the affected buckets */
    root = node = tree->document;

    while(node) {
        status = myhtml_id_index_node_add(tree, node);

        if(status) {
            tree->id_index = NULL;
            myhtml_id_index_free(index);
            return status;
        }

        if(node->child) {
            node = node->child;
        }
        else {
            while(node != root && node->next == NULL)
                node = node->parent;

            if(node == root)
                break;

            node = node->next;
        }
    }

    return MyHTML_STATUS_OK;
}

void myhtml_id_index_disable(myhtml_tree_t* tree)
{
    struct myhtml_id_index* index;

    if(tree == NULL || tree->id_index == NULL)
        return;

    index = (struct myhtml_id_index*)tree->id_index;
    tree->id_index = NULL;

    myhtml_id_index_free(index);
}

/* structural hooks */

void myhtml_id_index_on_attach(myhtml_tree_node_t* node)
{
    myhtml_tree_t* tree;

    if(node == NULL || node->tree == NULL)
        return;

    tree = node->tree;

    if(tree->id_index == NULL)
        return;

    if(!myhtml_id_index_node_in_scope(node, tree))
        return;

    myhtml_id_index_node_attach(tree, node);
}

void myhtml_id_index_on_detach(myhtml_tree_node_t* node)
{
    myhtml_tree_t* tree;

    if(node == NULL || node->tree == NULL)
        return;

    tree = node->tree;

    if(tree->id_index == NULL)
        return;

    myhtml_id_index_node_detach(tree, node);
}

void myhtml_id_index_on_attribute_changed(myhtml_tree_node_t* node)
{
    myhtml_tree_t* tree;

    if(node == NULL || node->tree == NULL)
        return;

    tree = node->tree;

    if(tree->id_index == NULL)
        return;

    if(node->id_index_entry)
        myhtml_id_index_entry_unlink((struct myhtml_id_index*)tree->id_index,
                                     (struct myhtml_id_index_entry*)node->id_index_entry);

    if(!myhtml_id_index_node_in_scope(node, tree))
        return;

    /* a failed re-add only loses this node from the index; the attribute
     * change itself succeeded and the status is not part of this API */
    (void)myhtml_id_index_node_add(tree, node);
}

void myhtml_id_index_on_node_free(myhtml_tree_node_t* node)
{
    myhtml_tree_t* tree;

    if(node == NULL || node->tree == NULL)
        return;

    tree = node->tree;

    if(tree->id_index == NULL || node->id_index_entry == NULL)
        return;

    myhtml_id_index_entry_unlink((struct myhtml_id_index*)tree->id_index,
                                 (struct myhtml_id_index_entry*)node->id_index_entry);
}

/* query */

static size_t myhtml_id_index_node_depth(myhtml_tree_node_t* node)
{
    size_t depth = 0;

    while(node) {
        depth++;
        node = node->parent;
    }

    return depth;
}

static int myhtml_id_index_order_compare(const void* one, const void* two)
{
    myhtml_tree_node_t* a = *(myhtml_tree_node_t* const*)one;
    myhtml_tree_node_t* b = *(myhtml_tree_node_t* const*)two;
    myhtml_tree_node_t* a_origin = a;
    size_t depth_a = myhtml_id_index_node_depth(a);
    size_t depth_b = myhtml_id_index_node_depth(b);

    if(a == b)
        return 0;

    /* lift the deeper node so both sit at the same depth */
    while(depth_a > depth_b) {
        a = a->parent;
        depth_a--;
    }

    while(depth_b > depth_a) {
        b = b->parent;
        depth_b--;
    }

    if(a == b) {
        /* one is an ancestor of the other; the ancestor comes first */
        return (a_origin == a) ? -1 : 1;
    }

    /* climb together until the parent is the common one */
    while(a && b && a->parent != b->parent) {
        a = a->parent;
        b = b->parent;
    }

    if(a == NULL || b == NULL)
        return 0;

    /* compare sibling position along the prev links */
    for(; a; a = a->prev) {
        if(a == b)
            return 1; /* b is an earlier sibling, so a comes after it */
    }

    return -1;
}

myhtml_collection_t * myhtml_get_nodes_by_id(myhtml_tree_t* tree, myhtml_collection_t* collection,
                                             const char* value, size_t value_len, mystatus_t* status)
{
    struct myhtml_id_index* index;
    struct myhtml_id_index_bucket* bucket;
    struct myhtml_id_index_entry* entry;
    size_t count;

    if(tree == NULL || tree->id_index == NULL) {
        if(status)
            *status = MyHTML_STATUS_ERROR;

        return NULL;
    }

    index = (struct myhtml_id_index*)tree->id_index;

    if(collection == NULL) {
        collection = myhtml_collection_create(1, status);

        if(collection == NULL)
            return NULL;
    }
    else {
        /* the caller owns the collection; only its length is reset */
        collection->length = 0;
    }

    if(status)
        *status = MyHTML_STATUS_OK;

    if(value == NULL || value_len == 0)
        return collection;

    bucket = myhtml_id_index_bucket_find(index, value, value_len);
    if(bucket == NULL || bucket->count == 0)
        return collection;

    if(myhtml_collection_check_size(collection, bucket->count, 0) != MyHTML_STATUS_OK) {
        if(status)
            *status = MyHTML_STATUS_ERROR_MEMORY_ALLOCATION;

        return NULL;
    }

    count = 0;

    for(entry = bucket->first; entry; entry = entry->next)
        collection->list[count++] = entry->node;

    qsort(collection->list, count, sizeof(myhtml_tree_node_t*), myhtml_id_index_order_compare);

    collection->length = count;

    return collection;
}
