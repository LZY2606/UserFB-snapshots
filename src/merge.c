
#include "yaml_private.h"

/*
 * Merge key expansion.
 *
 * The work is split in four passes:
 *
 *  1. Deep-copy every reachable source node into the output document.
 *     The traversal is iterative and tolerates ordinary alias cycles,
 *     including cycles through sequences and plain values.
 *  2. For every source mapping, classify its pairs, validate the keys
 *     and merge values, and record merge dependencies (edges created
 *     only by << entries).
 *  3. Run a three-color search on the merge-dependency graph alone; a
 *     back edge is a merge cycle.
 *  4. Expand the mappings in post-order, appending output pairs and
 *     building the provenance of every output pair.
 *
 * The source document is read only and is never modified.
 */

#define YAML_MERGE_KEY_TEXT     "<<"
#define YAML_MERGE_KEY_LENGTH   2

/* Colors of the merge-dependency graph search. */
enum merge_color_e {
    MERGE_WHITE = 0,
    MERGE_GRAY,
    MERGE_BLACK
};

typedef struct {
    /* The source pair index of a << entry. */
    int merge_pair;
    /* Source mapping ids referenced by the entry, in precedence order. */
    int *targets;
    int target_count;
    int target_cap;
} merge_entry_t;

typedef struct {
    /* << entries, in left-to-right pair order. */
    merge_entry_t *entries;
    int entry_count;
    int entry_cap;
    /* Source pair indices of local (non-merge) fields, in order. */
    int *local_pairs;
    int local_count;
    int local_pair_cap;
    /* Source key node ids of the local fields. */
    int *local_keys;
    int local_key_cap;
    /* Expanded field records, filled during pass 4. */
    /* The source key id of every expanded field, for precedence. */
    int *seen_keys;
    int seen_count;
    int seen_cap;
    int color;
} map_build_t;

typedef struct {
    /* The source key node id, used for tag-and-bytes key identity. */
    int src_key;
    /* The output key and value node ids. */
    int key_out;
    int value_out;
    /* The source pair that actually wins this field. */
    int origin_mapping;
    int origin_pair;
    int origin_key;
    int origin_value;
    /* The merge hops crossed to reach the winning pair. */
    int hop_count;
    yaml_merge_hop_t *hops;
} merge_field_t;

typedef struct {
    yaml_error_type_t error;
} merge_context_t;

static int
merge_grow(void **buffer, int *capacity, int need, size_t element_size)
{
    int new_capacity;
    void *new_buffer;

    if (need <= *capacity)
        return 1;

    new_capacity = *capacity ? *capacity : 8;
    while (new_capacity < need) {
        if (new_capacity > INT_MAX / 2)
            return 0;
        new_capacity *= 2;
    }

    new_buffer = yaml_realloc(*buffer, (size_t)new_capacity * element_size);
    if (!new_buffer)
        return 0;

    *buffer = new_buffer;
    *capacity = new_capacity;
    return 1;
}

static int
merge_scalar_equal(const yaml_node_t *a, const yaml_node_t *b)
{
    if (a->type != YAML_SCALAR_NODE || b->type != YAML_SCALAR_NODE)
        return 0;
    if (strcmp((char *)a->tag, (char *)b->tag) != 0)
        return 0;
    if (a->data.scalar.length != b->data.scalar.length)
        return 0;
    return memcmp(a->data.scalar.value, b->data.scalar.value,
                a->data.scalar.length) == 0;
}

static int
merge_is_merge_key(const yaml_node_t *key)
{
    if (key->type != YAML_SCALAR_NODE)
        return 0;

    /* An explicit merge tag always marks a merge key. */
    if (strcmp((char *)key->tag, YAML_MERGE_TAG) == 0)
        return 1;

    /* A plain << with the default string tag is a merge key; a quoted
     * "<<" keeps the string tag and is an ordinary field. */
    if (strcmp((char *)key->tag, YAML_DEFAULT_SCALAR_TAG) == 0
            && key->data.scalar.style == YAML_PLAIN_SCALAR_STYLE
            && key->data.scalar.length == YAML_MERGE_KEY_LENGTH
            && memcmp(key->data.scalar.value, YAML_MERGE_KEY_TEXT,
                    YAML_MERGE_KEY_LENGTH) == 0)
        return 1;

    return 0;
}

static void
merge_set_error(yaml_merge_error_t *error, yaml_merge_error_type_t type,
        int mapping, int pair, int node, int node2,
        const yaml_node_t *mark_node, const yaml_node_t *end_node)
{
    if (!error)
        return;

    memset(error, 0, sizeof(yaml_merge_error_t));
    error->type = type;
    error->mapping = mapping;
    error->pair = pair;
    error->node = node;
    error->node2 = node2;
    if (mark_node)
        error->mark = mark_node->start_mark;
    if (end_node)
        error->end_mark = end_node->end_mark;
}

/* Deep-copy a scalar node, preserving bytes (by length), tag, style
 * and marks.  Returns the new node id, or 0 on allocation failure. */

static int
merge_add_scalar(merge_context_t *ctx, yaml_document_t *out,
        const yaml_node_t *src)
{
    yaml_char_t *tag_copy = NULL;
    yaml_char_t *value_copy = NULL;
    yaml_node_t node;
    int id;

    tag_copy = yaml_strdup(src->tag);
    if (!tag_copy)
        goto error;

    value_copy = YAML_MALLOC(src->data.scalar.length + 1);
    if (!value_copy)
        goto error;
    memcpy(value_copy, src->data.scalar.value, src->data.scalar.length);
    value_copy[src->data.scalar.length] = '\0';

    SCALAR_NODE_INIT(node, tag_copy, value_copy, src->data.scalar.length,
            src->data.scalar.style, src->start_mark, src->end_mark);
    if (!PUSH(ctx, out->nodes, node))
        goto error;

    id = (int)(out->nodes.top - out->nodes.start);
    return id;

error:
    yaml_free(tag_copy);
    yaml_free(value_copy);
    ctx->error = YAML_MEMORY_ERROR;
    return 0;
}

static int
merge_add_container(merge_context_t *ctx, yaml_document_t *out,
        const yaml_node_t *src)
{
    yaml_char_t *tag_copy = NULL;
    void *inner_start = NULL;
    yaml_node_t node;
    int id;

    tag_copy = yaml_strdup(src->tag);
    if (!tag_copy)
        goto error;

    memset(&node, 0, sizeof(node));

    if (src->type == YAML_SEQUENCE_NODE) {
        struct {
            yaml_node_item_t *start;
            yaml_node_item_t *end;
            yaml_node_item_t *top;
        } items = { NULL, NULL, NULL };
        if (!STACK_INIT(ctx, items, yaml_node_item_t*))
            goto error;
        inner_start = items.start;
        SEQUENCE_NODE_INIT(node, tag_copy, items.start, items.end,
                src->data.sequence.style, src->start_mark, src->end_mark);
    }
    else {
        struct {
            yaml_node_pair_t *start;
            yaml_node_pair_t *end;
            yaml_node_pair_t *top;
        } pairs = { NULL, NULL, NULL };
        if (!STACK_INIT(ctx, pairs, yaml_node_pair_t*))
            goto error;
        inner_start = pairs.start;
        MAPPING_NODE_INIT(node, tag_copy, pairs.start, pairs.end,
                src->data.mapping.style, src->start_mark, src->end_mark);
    }

    if (!PUSH(ctx, out->nodes, node)) {
        /* The node is not in the document yet, so free its stack. */
        yaml_free(inner_start);
        goto error;
    }

    id = (int)(out->nodes.top - out->nodes.start);
    return id;

error:
    yaml_free(tag_copy);
    ctx->error = YAML_MEMORY_ERROR;
    return 0;
}

static void
merge_free_fields(merge_field_t *fields, int count)
{
    for (int i = 0; i < count; i++)
        yaml_free(fields[i].hops);
    yaml_free(fields);
}

static void
merge_free_builds(map_build_t **builds, int node_count)
{
    if (!builds)
        return;
    for (int i = 1; i <= node_count; i++) {
        map_build_t *build = builds[i];
        if (!build)
            continue;
        for (int e = 0; e < build->entry_count; e++)
            yaml_free(build->entries[e].targets);
        yaml_free(build->entries);
        yaml_free(build->local_pairs);
        yaml_free(build->local_keys);
        yaml_free(build->seen_keys);
        yaml_free(build);
    }
    yaml_free(builds);
}

YAML_DECLARE(int)
yaml_document_expand_merges(const yaml_document_t *document,
        yaml_merge_result_t *result, yaml_merge_error_t *error)
{
    merge_context_t ctx;
    int node_count;
    int *id_map = NULL;
    int *stack = NULL;
    int stack_count = 0;
    int stack_cap = 0;
    /* Working stacks of the merge-graph searches (passes 3 and 4). */
    int *dfs_color = NULL;
    int *dfs_cursor = NULL;
    int dfs_color_cap = 0;
    int dfs_cursor_cap = 0;
    map_build_t **builds = NULL;
    yaml_merge_pair_info_t *mapping_info = NULL;
    int mapping_info_count = 0;
    int document_initialized = 0;
    int ok = 0;

    assert(document);   /* Non-NULL source document is expected. */
    assert(result);     /* Non-NULL result object is expected. */

    ctx.error = YAML_NO_ERROR;
    memset(result, 0, sizeof(yaml_merge_result_t));
    if (error)
        memset(error, 0, sizeof(yaml_merge_error_t));

    node_count = (int)(document->nodes.top - document->nodes.start);

    /* yaml_document_initialize() deep copies the version and tag
     * directives, so the result owns its directive memory. */
    if (!yaml_document_initialize(&result->document,
                document->version_directive,
                document->tag_directives.start,
                document->tag_directives.end,
                document->start_implicit, document->end_implicit))
        goto memory_error;
    document_initialized = 1;
    result->document.start_mark = document->start_mark;
    result->document.end_mark = document->end_mark;

    if (node_count == 0) {
        ok = 1;
        goto done;
    }

    id_map = (int *)yaml_malloc((size_t)(node_count + 1) * sizeof(int));
    builds = (map_build_t **)yaml_malloc(
            (size_t)(node_count + 1) * sizeof(map_build_t *));
    if (!id_map || !builds)
        goto memory_error;
    memset(id_map, 0, (size_t)(node_count + 1) * sizeof(int));
    memset(builds, 0, (size_t)(node_count + 1) * sizeof(map_build_t *));

    /* Pass 1: create one output node per reachable source node. */
    if (!merge_grow((void **)&stack, &stack_cap, 1, sizeof(int)))
        goto memory_error;
    stack[stack_count++] = 1;

    while (stack_count > 0) {
        int src_id = stack[--stack_count];
        yaml_node_t *src;
        int out_id;

        if (id_map[src_id] != 0)
            continue;   /* Reached again through an alias: keep identity. */

        src = document->nodes.start + src_id - 1;
        if (src->type == YAML_SCALAR_NODE)
            out_id = merge_add_scalar(&ctx, &result->document, src);
        else
            out_id = merge_add_container(&ctx, &result->document, src);
        if (!out_id)
            goto memory_error;
        id_map[src_id] = out_id;

        if (src->type == YAML_SEQUENCE_NODE) {
            yaml_node_item_t *item;
            for (item = src->data.sequence.items.start;
                    item < src->data.sequence.items.top; item++) {
                if (id_map[*item] == 0) {
                    if (!merge_grow((void **)&stack, &stack_cap,
                                stack_count + 1, sizeof(int)))
                        goto memory_error;
                    stack[stack_count++] = *item;
                }
            }
        }
        else if (src->type == YAML_MAPPING_NODE) {
            yaml_node_pair_t *pair;
            for (pair = src->data.mapping.pairs.start;
                    pair < src->data.mapping.pairs.top; pair++) {
                if (id_map[pair->key] == 0) {
                    if (!merge_grow((void **)&stack, &stack_cap,
                                stack_count + 1, sizeof(int)))
                        goto memory_error;
                    stack[stack_count++] = pair->key;
                }
                if (id_map[pair->value] == 0) {
                    if (!merge_grow((void **)&stack, &stack_cap,
                                stack_count + 1, sizeof(int)))
                        goto memory_error;
                    stack[stack_count++] = pair->value;
                }
            }
        }
    }

    /* Every reachable node now has an output id.  Fill the output
     * sequences; alias identity is preserved because ids are shared. */
    for (int src_id = 1; src_id <= node_count; src_id++) {
        yaml_node_t *src = document->nodes.start + src_id - 1;
        if (src->type != YAML_SEQUENCE_NODE)
            continue;
        if (id_map[src_id] == 0)
            continue;   /* Unreachable from the root: not copied. */
        for (yaml_node_item_t *item = src->data.sequence.items.start;
                item < src->data.sequence.items.top; item++) {
            if (!yaml_document_append_sequence_item(&result->document,
                        id_map[src_id], id_map[*item]))
                goto memory_error;
        }
    }

    mapping_info_count =
        (int)(result->document.nodes.top - result->document.nodes.start) + 1;
    mapping_info = (yaml_merge_pair_info_t *)yaml_malloc(
            (size_t)mapping_info_count * sizeof(yaml_merge_pair_info_t));
    if (!mapping_info)
        goto memory_error;
    memset(mapping_info, 0,
            (size_t)mapping_info_count * sizeof(yaml_merge_pair_info_t));

    /* Pass 2: classify and validate every source mapping. */
    for (int src_id = 1; src_id <= node_count; src_id++) {
        yaml_node_t *map_node = document->nodes.start + src_id - 1;
        map_build_t *build;
        int pair_count;

        if (map_node->type != YAML_MAPPING_NODE)
            continue;
        if (id_map[src_id] == 0)
            continue;   /* Unreachable from the root: not copied. */

        build = (map_build_t *)yaml_malloc(sizeof(map_build_t));
        if (!build)
            goto memory_error;
        memset(build, 0, sizeof(map_build_t));
        builds[src_id] = build;
        pair_count = (int)(map_node->data.mapping.pairs.top
                - map_node->data.mapping.pairs.start);

        for (int p = 0; p < pair_count; p++) {
            yaml_node_pair_t *pair =
                &map_node->data.mapping.pairs.start[p];
            yaml_node_t *key_node =
                document->nodes.start + pair->key - 1;
            yaml_node_t *value_node;

            if (key_node->type != YAML_SCALAR_NODE) {
                merge_set_error(error, YAML_MERGE_NON_SCALAR_KEY_ERROR,
                        src_id, p, pair->key, 0, key_node, key_node);
                goto failed;
            }

            if (!merge_is_merge_key(key_node)) {
                /* Local field: reject duplicate local keys. */
                for (int k = 0; k < build->local_count; k++) {
                    yaml_node_t *seen =
                        document->nodes.start + build->local_keys[k] - 1;
                    if (merge_scalar_equal(seen, key_node)) {
                        merge_set_error(error,
                                YAML_MERGE_DUPLICATE_KEY_ERROR,
                                src_id, p, pair->key,
                                build->local_keys[k], key_node, key_node);
                        goto failed;
                    }
                }
                if (!merge_grow((void **)&build->local_pairs,
                            &build->local_pair_cap,
                            build->local_count + 1, sizeof(int))
                        || !merge_grow((void **)&build->local_keys,
                            &build->local_key_cap,
                            build->local_count + 1, sizeof(int)))
                    goto memory_error;
                build->local_pairs[build->local_count] = p;
                build->local_keys[build->local_count] = pair->key;
                build->local_count++;
                continue;
            }

            /* Merge entry: a mapping or a sequence of mappings. */
            value_node = document->nodes.start + pair->value - 1;
            {
                merge_entry_t entry;
                memset(&entry, 0, sizeof(entry));
                entry.merge_pair = p;

                if (value_node->type == YAML_MAPPING_NODE) {
                    if (!merge_grow((void **)&entry.targets,
                                &entry.target_cap, 1, sizeof(int))) {
                        yaml_free(entry.targets);
                        goto memory_error;
                    }
                    entry.targets[entry.target_count++] = pair->value;
                }
                else if (value_node->type == YAML_SEQUENCE_NODE) {
                    int count = (int)(value_node->data.sequence.items.top
                            - value_node->data.sequence.items.start);
                    if (!merge_grow((void **)&entry.targets,
                                &entry.target_cap,
                                count ? count : 1, sizeof(int))) {
                        yaml_free(entry.targets);
                        goto memory_error;
                    }
                    for (int t = 0; t < count; t++) {
                        int item = value_node->data.sequence.items.start[t];
                        yaml_node_t *item_node =
                            document->nodes.start + item - 1;
                        if (item_node->type != YAML_MAPPING_NODE) {
                            merge_set_error(error,
                                    YAML_MERGE_INVALID_VALUE_ERROR,
                                    src_id, p, item, 0,
                                    item_node, value_node);
                            yaml_free(entry.targets);
                            goto failed;
                        }
                        entry.targets[entry.target_count++] = item;
                    }
                }
                else {
                    merge_set_error(error, YAML_MERGE_INVALID_VALUE_ERROR,
                            src_id, p, pair->value, 0,
                            value_node, value_node);
                    goto failed;
                }

                if (!merge_grow((void **)&build->entries, &build->entry_cap,
                            build->entry_count + 1,
                            sizeof(merge_entry_t))) {
                    yaml_free(entry.targets);
                    goto memory_error;
                }
                build->entries[build->entry_count++] = entry;
            }
        }
    }

    /* Pass 3: detect cycles in the merge-dependency graph.  The
     * iterative search carries an explicit edge cursor per node. */
    {
        int top = 0;

        for (int start = 1; start <= node_count; start++) {
            map_build_t *start_build = builds[start];
            if (!start_build || start_build->color != MERGE_WHITE)
                continue;

            if (!merge_grow((void **)&dfs_color, &dfs_color_cap, 1,
                        sizeof(int))
                    || !merge_grow((void **)&dfs_cursor, &dfs_cursor_cap, 1,
                        sizeof(int)))
                goto memory_error;
            dfs_color[top] = start;
            dfs_cursor[top] = 0;
            start_build->color = MERGE_GRAY;
            top++;

            while (top > 0) {
                int cur = dfs_color[top - 1];
                map_build_t *cur_build = builds[cur];
                int edge_count = 0;

                /* Flatten the merge entries into an edge list view. */
                for (int e = 0; e < cur_build->entry_count; e++)
                    edge_count += cur_build->entries[e].target_count;

                if (dfs_cursor[top - 1] >= edge_count) {
                    cur_build->color = MERGE_BLACK;
                    top--;
                    continue;
                }

                /* Resolve the nth edge. */
                {
                    int edge_index = dfs_cursor[top - 1]++;
                    int next = 0;
                    for (int e = 0; e < cur_build->entry_count; e++) {
                        if (edge_index
                                < cur_build->entries[e].target_count) {
                            next = cur_build->entries[e]
                                .targets[edge_index];
                            break;
                        }
                        edge_index -=
                            cur_build->entries[e].target_count;
                    }

                    if (builds[next]->color == MERGE_GRAY) {
                        yaml_node_t *next_node =
                            document->nodes.start + next - 1;
                        merge_set_error(error, YAML_MERGE_CYCLE_ERROR,
                                cur, -1, next, cur,
                                next_node, next_node);
                        goto failed;
                    }
                    if (builds[next]->color == MERGE_WHITE) {
                        builds[next]->color = MERGE_GRAY;
                        if (!merge_grow((void **)&dfs_color, &dfs_color_cap,
                                    top + 1, sizeof(int))
                                || !merge_grow((void **)&dfs_cursor,
                                    &dfs_cursor_cap, top + 1, sizeof(int)))
                            goto memory_error;
                        dfs_color[top] = next;
                        dfs_cursor[top] = 0;
                        top++;
                    }
                }
            }
        }
    }

    /* Pass 4: expand in post-order of the merge-dependency graph.  The
     * merge-graph search buffers are reused, starting at depth zero. */
    {
        int top = 0;

        /* Colors are reused: WHITE means not expanded, GRAY on the
         * stack, BLACK fully expanded. */
        for (int i = 1; i <= node_count; i++)
            if (builds[i])
                builds[i]->color = MERGE_WHITE;

        for (int start = 1; start <= node_count; start++) {
            map_build_t *start_build = builds[start];
            if (!start_build || start_build->color != MERGE_WHITE)
                continue;

            if (!merge_grow((void **)&dfs_color, &dfs_color_cap, 1, sizeof(int))
                    || !merge_grow((void **)&dfs_cursor, &dfs_cursor_cap, 1,
                        sizeof(int)))
                goto memory_error;
            dfs_color[top] = start;
            dfs_cursor[top] = 0;
            start_build->color = MERGE_GRAY;
            top++;

            while (top > 0) {
                int cur = dfs_color[top - 1];
                map_build_t *build = builds[cur];
                yaml_node_t *map_node =
                    document->nodes.start + cur - 1;
                int edge_count = 0;

                for (int e = 0; e < build->entry_count; e++)
                    edge_count += build->entries[e].target_count;

                if (dfs_cursor[top - 1] < edge_count) {
                    int edge_index = dfs_cursor[top - 1]++;
                    int next = 0;
                    for (int e = 0; e < build->entry_count; e++) {
                        if (edge_index
                                < build->entries[e].target_count) {
                            next = build->entries[e]
                                .targets[edge_index];
                            break;
                        }
                        edge_index -= build->entries[e].target_count;
                    }
                    if (builds[next]->color == MERGE_WHITE) {
                        builds[next]->color = MERGE_GRAY;
                        if (!merge_grow((void **)&dfs_color, &dfs_color_cap,
                                    top + 1, sizeof(int))
                                || !merge_grow((void **)&dfs_cursor,
                                    &dfs_cursor_cap, top + 1, sizeof(int)))
                            goto memory_error;
                        dfs_color[top] = next;
                        dfs_cursor[top] = 0;
                        top++;
                    }
                    continue;
                }

                /* All merge targets are expanded: build this mapping. */
                {
                    merge_field_t *fields = NULL;
                    int field_count = 0;
                    int field_cap = 0;

                    /* Local fields first, in original pair order. */
                    for (int li = 0; li < build->local_count; li++) {
                        int p = build->local_pairs[li];
                        yaml_node_pair_t *pair =
                            &map_node->data.mapping.pairs.start[p];
                        merge_field_t field;

                        if (!yaml_document_append_mapping_pair(
                                    &result->document, id_map[cur],
                                    id_map[pair->key],
                                    id_map[pair->value]))
                            goto memory_error;

                        if (!merge_grow((void **)&fields, &field_cap,
                                    field_count + 1,
                                    sizeof(merge_field_t))
                                || !merge_grow((void **)&build->seen_keys,
                                    &build->seen_cap,
                                    build->seen_count + 1, sizeof(int))) {
                            merge_free_fields(fields, field_count);
                            goto memory_error;
                        }

                        memset(&field, 0, sizeof(field));
                        field.src_key = pair->key;
                        field.key_out = id_map[pair->key];
                        field.value_out = id_map[pair->value];
                        field.origin_mapping = cur;
                        field.origin_pair = p;
                        field.origin_key = pair->key;
                        field.origin_value = pair->value;
                        fields[field_count++] = field;
                        build->seen_keys[build->seen_count++] = pair->key;
                    }

                    /* Merged fields, merge by merge, target by target,
                     * in the source field order. */
                    for (int ei = 0; ei < build->entry_count; ei++) {
                        int merge_pair =
                            build->entries[ei].merge_pair;
                        for (int ti = 0;
                                ti < build->entries[ei].target_count;
                                ti++) {
                            int target = build->entries[ei].targets[ti];
                            yaml_merge_pair_info_t *target_info =
                                &mapping_info[id_map[target]];

                            for (int fi = 0;
                                    fi < target_info->pair_count; fi++) {
                                yaml_merge_origin_t *inherited =
                                    &target_info->origins[fi];
                                yaml_node_t *key_node =
                                    document->nodes.start
                                    + inherited->key - 1;
                                int seen = 0;

                                for (int si = 0;
                                        si < build->seen_count; si++) {
                                    yaml_node_t *seen_node =
                                        document->nodes.start
                                        + build->seen_keys[si] - 1;
                                    if (merge_scalar_equal(seen_node,
                                                key_node)) {
                                        seen = 1;
                                        break;
                                    }
                                }
                                if (seen)
                                    continue;

                                yaml_merge_hop_t *new_hops =
                                    (yaml_merge_hop_t *)yaml_malloc(
                                        (size_t)(inherited->hop_count + 1)
                                        * sizeof(yaml_merge_hop_t));
                                merge_field_t field;
                                if (!new_hops) {
                                    merge_free_fields(fields, field_count);
                                    goto memory_error;
                                }
                                new_hops[0].mapping = cur;
                                new_hops[0].pair = merge_pair;
                                new_hops[0].target = target;
                                for (int hi = 0;
                                        hi < inherited->hop_count; hi++)
                                    new_hops[hi + 1] =
                                        inherited->hops[hi];

                                if (!yaml_document_append_mapping_pair(
                                            &result->document, id_map[cur],
                                            id_map[inherited->key],
                                            id_map[inherited->value])
                                        || !merge_grow((void **)&fields,
                                            &field_cap, field_count + 1,
                                            sizeof(merge_field_t))
                                        || !merge_grow(
                                            (void **)&build->seen_keys,
                                            &build->seen_cap,
                                            build->seen_count + 1,
                                            sizeof(int))) {
                                    yaml_free(new_hops);
                                    merge_free_fields(fields, field_count);
                                    goto memory_error;
                                }

                                memset(&field, 0, sizeof(field));
                                field.src_key = inherited->key;
                                field.key_out = id_map[inherited->key];
                                field.value_out = id_map[inherited->value];
                                field.origin_mapping = inherited->mapping;
                                field.origin_pair = inherited->pair;
                                field.origin_key = inherited->key;
                                field.origin_value = inherited->value;
                                field.hop_count = inherited->hop_count + 1;
                                field.hops = new_hops;
                                fields[field_count++] = field;
                                build->seen_keys[build->seen_count++] =
                                    inherited->key;
                            }
                        }
                    }

                    /* Publish the provenance, transferring hop
                     * ownership to the result. */
                    {
                        yaml_merge_pair_info_t *info =
                            &mapping_info[id_map[cur]];
                        info->pair_count = field_count;
                        info->origins =
                            (yaml_merge_origin_t *)yaml_malloc(
                                (size_t)(field_count ? field_count : 1)
                                * sizeof(yaml_merge_origin_t));
                        if (!info->origins) {
                            merge_free_fields(fields, field_count);
                            goto memory_error;
                        }
                        memset(info->origins, 0,
                                (size_t)field_count
                                * sizeof(yaml_merge_origin_t));
                        for (int fi = 0; fi < field_count; fi++) {
                            yaml_merge_origin_t *origin =
                                &info->origins[fi];
                            origin->mapping = fields[fi].origin_mapping;
                            origin->pair = fields[fi].origin_pair;
                            origin->key = fields[fi].origin_key;
                            origin->value = fields[fi].origin_value;
                            origin->hop_count = fields[fi].hop_count;
                            origin->hops = fields[fi].hops;
                            fields[fi].hops = NULL;
                        }
                    }
                    merge_free_fields(fields, field_count);
                    build->color = MERGE_BLACK;
                    top--;
                }
            }
        }
    }

    ok = 1;
    goto done;

memory_error:
    merge_set_error(error, YAML_MERGE_MEMORY_ERROR, 0, -1, 0, 0, NULL, NULL);

failed:
    ok = 0;

done:
    yaml_free(stack);
    yaml_free(dfs_color);
    yaml_free(dfs_cursor);
    merge_free_builds(builds, node_count);

    if (ok) {
        result->mapping_info = mapping_info;
        result->mapping_info_count = mapping_info_count;
        return 1;
    }

    /* Failure: release every provenance array and the partially built
     * document.  The source document was never touched. */
    if (mapping_info) {
        for (int i = 0; i < mapping_info_count; i++) {
            yaml_merge_pair_info_t *info = &mapping_info[i];
            if (info->origins) {
                for (int fi = 0; fi < info->pair_count; fi++)
                    yaml_free(info->origins[fi].hops);
                yaml_free(info->origins);
            }
        }
        yaml_free(mapping_info);
    }
    if (document_initialized)
        yaml_document_delete(&result->document);
    memset(result, 0, sizeof(yaml_merge_result_t));
    return 0;
}

YAML_DECLARE(const yaml_merge_pair_info_t *)
yaml_merge_result_get_mapping_info(const yaml_merge_result_t *result,
        int node_id)
{
    assert(result);     /* Non-NULL result object is expected. */

    if (node_id <= 0 || node_id >= result->mapping_info_count)
        return NULL;
    if (!result->mapping_info[node_id].origins)
        return NULL;
    return &result->mapping_info[node_id];
}

YAML_DECLARE(void)
yaml_merge_result_delete(yaml_merge_result_t *result)
{
    if (!result)
        return;

    if (result->mapping_info) {
        for (int i = 0; i < result->mapping_info_count; i++) {
            yaml_merge_pair_info_t *info = &result->mapping_info[i];
            if (info->origins) {
                for (int fi = 0; fi < info->pair_count; fi++)
                    yaml_free(info->origins[fi].hops);
                yaml_free(info->origins);
            }
        }
        yaml_free(result->mapping_info);
    }

    if (result->document.nodes.start)
        yaml_document_delete(&result->document);

    memset(result, 0, sizeof(yaml_merge_result_t));
}
