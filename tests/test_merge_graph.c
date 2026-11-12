
#include "check.h"

#include <yaml.h>

#include <string.h>

/*
 * Tests for yaml_document_expand_merges().
 *
 * The cases are registered in the four groups precedence, graph,
 * errors and roundtrip.  Source node ids come from the freshly loaded
 * documents; provenance always refers to those source ids, while the
 * expanded document is navigated on its own node ids.
 */

static int
load_string(const char *text, yaml_parser_t *parser, yaml_document_t *doc)
{
    if (!yaml_parser_initialize(parser))
        return 0;
    yaml_parser_set_input_string(parser, (const unsigned char *)text,
            strlen(text));
    return yaml_parser_load(parser, doc);
}

static yaml_node_pair_t *
find_pair(yaml_document_t *doc, int map_id, const char *key)
{
    yaml_node_t *map = yaml_document_get_node(doc, map_id);
    yaml_node_pair_t *pair;
    size_t len;

    if (!map || map->type != YAML_MAPPING_NODE)
        return NULL;

    len = strlen(key);
    for (pair = map->data.mapping.pairs.start;
            pair < map->data.mapping.pairs.top; pair++) {
        yaml_node_t *k = yaml_document_get_node(doc, pair->key);
        if (k && k->type == YAML_SCALAR_NODE
                && k->data.scalar.length == len
                && memcmp(k->data.scalar.value, key, len) == 0)
            return pair;
    }
    return NULL;
}

static yaml_node_pair_t *
find_pair_n(yaml_document_t *doc, int map_id, const char *key, size_t len)
{
    yaml_node_t *map = yaml_document_get_node(doc, map_id);
    yaml_node_pair_t *pair;

    if (!map || map->type != YAML_MAPPING_NODE)
        return NULL;

    for (pair = map->data.mapping.pairs.start;
            pair < map->data.mapping.pairs.top; pair++) {
        yaml_node_t *k = yaml_document_get_node(doc, pair->key);
        if (k && k->type == YAML_SCALAR_NODE
                && k->data.scalar.length == len
                && memcmp(k->data.scalar.value, key, len) == 0)
            return pair;
    }
    return NULL;
}

static int
value_id(yaml_document_t *doc, int map_id, const char *key)
{
    yaml_node_pair_t *pair = find_pair(doc, map_id, key);
    return pair ? pair->value : 0;
}

static int
pair_index(yaml_document_t *doc, int map_id, const char *key)
{
    yaml_node_t *map = yaml_document_get_node(doc, map_id);
    yaml_node_pair_t *pair = find_pair(doc, map_id, key);

    if (!map || !pair)
        return -1;
    return (int)(pair - map->data.mapping.pairs.start);
}

static int
scalar_is(yaml_document_t *doc, int node_id, const char *text)
{
    yaml_node_t *node = yaml_document_get_node(doc, node_id);
    size_t len = strlen(text);

    return node && node->type == YAML_SCALAR_NODE
        && node->data.scalar.length == len
        && memcmp(node->data.scalar.value, text, len) == 0;
}

typedef struct {
    unsigned char *data;
    size_t length;
    size_t capacity;
} memory_buffer;

static int
memory_write(void *data, unsigned char *buffer, size_t size)
{
    memory_buffer *out = (memory_buffer *)data;

    if (out->length + size + 1 > out->capacity) {
        size_t new_capacity = out->capacity ? out->capacity : 256;
        unsigned char *new_data;
        while (new_capacity < out->length + size + 1)
            new_capacity *= 2;
        new_data = (unsigned char *)realloc(out->data, new_capacity);
        if (!new_data)
            return 0;
        out->data = new_data;
        out->capacity = new_capacity;
    }
    memcpy(out->data + out->length, buffer, size);
    out->length += size;
    out->data[out->length] = '\0';
    return 1;
}

static int
buffer_contains(const memory_buffer *out, const char *text)
{
    size_t len = strlen(text);
    size_t i;

    if (len > out->length)
        return 0;
    for (i = 0; i + len <= out->length; i++) {
        if (memcmp(out->data + i, text, len) == 0)
            return 1;
    }
    return 0;
}

/* Count plain merge keys left in a document, tolerating cycles. */
static int
count_merge_keys(yaml_document_t *doc)
{
    yaml_node_t *root = yaml_document_get_root_node(doc);
    int node_count;
    unsigned char *visited;
    int *stack;
    int stack_count = 0;
    int result = 0;

    if (!root)
        return 0;

    node_count = (int)(doc->nodes.top - doc->nodes.start);
    visited = (unsigned char *)calloc((size_t)node_count + 1, 1);
    stack = (int *)malloc((size_t)(node_count + 1) * sizeof(int));
    if (!visited || !stack) {
        free(visited);
        free(stack);
        return -1;
    }

    stack[stack_count++] = 1;
    while (stack_count > 0) {
        int id = stack[--stack_count];
        yaml_node_t *node;

        if (id <= 0 || visited[id])
            continue;
        visited[id] = 1;
        node = yaml_document_get_node(doc, id);
        if (!node)
            continue;

        if (node->type == YAML_SEQUENCE_NODE) {
            yaml_node_item_t *item;
            for (item = node->data.sequence.items.start;
                    item < node->data.sequence.items.top; item++)
                stack[stack_count++] = *item;
        }
        else if (node->type == YAML_MAPPING_NODE) {
            yaml_node_pair_t *pair;
            for (pair = node->data.mapping.pairs.start;
                    pair < node->data.mapping.pairs.top; pair++) {
                yaml_node_t *key = yaml_document_get_node(doc, pair->key);
                if (key && key->type == YAML_SCALAR_NODE
                        && key->data.scalar.length == 2
                        && memcmp(key->data.scalar.value, "<<", 2) == 0
                        && key->data.scalar.style
                            == YAML_PLAIN_SCALAR_STYLE)
                    result++;
                stack[stack_count++] = pair->key;
                stack[stack_count++] = pair->value;
            }
        }
    }

    free(visited);
    free(stack);
    return result;
}

/*
 * precedence: local fields always win, sequence merges keep the first
 * source, multiple merges are processed left to right, and the winning
 * source pair is recorded through several merge levels.
 */

static void
test_precedence_basic(unit_context *c)
{
    static const char text[] =
        "base: &b {id: 7, unit: kg}\n"
        "row_before: {id: 3, <<: *b}\n"
        "row_after: {<<: *b, id: 9}\n";
    yaml_parser_t parser;
    yaml_document_t src;
    yaml_merge_result_t result;
    yaml_merge_error_t error;
    int base, row_before, row_after;
    int out_before, out_after, info_pair_count;
    const yaml_merge_pair_info_t *info;
    const yaml_merge_origin_t *origin;

    CHECK(c, load_string(text, &parser, &src));
    memset(&result, 0, sizeof(result));
    memset(&error, 0, sizeof(error));

    base = value_id(&src, 1, "base");
    row_before = value_id(&src, 1, "row_before");
    row_after = value_id(&src, 1, "row_after");
    CHECK(c, base && row_before && row_after);

    CHECK(c, yaml_document_expand_merges(&src, &result, &error) == 1);
    CHECK(c, error.type == YAML_MERGE_OK);

    out_before = value_id(&result.document, 1, "row_before");
    out_after = value_id(&result.document, 1, "row_after");
    CHECK(c, out_before && out_after);

    /* Local fields come first, merged fields after. */
    info = yaml_merge_result_get_mapping_info(&result, out_after);
    CHECK(c, info != NULL);
    info_pair_count = info->pair_count;
    CHECK(c, info_pair_count == 2);
    CHECK(c, scalar_is(&result.document,
                yaml_document_get_node(&result.document, out_after)
                    ->data.mapping.pairs.start[0].key, "id"));
    CHECK(c, scalar_is(&result.document,
                yaml_document_get_node(&result.document, out_after)
                    ->data.mapping.pairs.start[1].key, "unit"));
    CHECK(c, scalar_is(&result.document,
                value_id(&result.document, out_after, "id"), "9"));
    CHECK(c, scalar_is(&result.document,
                value_id(&result.document, out_after, "unit"), "kg"));

    /* The local id is sourced locally, whether before or after <<. */
    origin = &info->origins[0];
    CHECK(c, origin->mapping == row_after);
    CHECK(c, origin->pair == pair_index(&src, row_after, "id"));
    CHECK(c, origin->hop_count == 0);
    CHECK(c, origin->hops == NULL);

    info = yaml_merge_result_get_mapping_info(&result, out_before);
    CHECK(c, info != NULL && info->pair_count == 2);
    origin = &info->origins[0];
    CHECK(c, scalar_is(&result.document,
                value_id(&result.document, out_before, "id"), "3"));
    CHECK(c, origin->mapping == row_before);
    CHECK(c, origin->hop_count == 0);

    /* unit comes from base, with the single hop recorded. */
    info = yaml_merge_result_get_mapping_info(&result, out_after);
    origin = &info->origins[1];
    CHECK(c, origin->mapping == base);
    CHECK(c, origin->pair == pair_index(&src, base, "unit"));
    CHECK(c, origin->key == find_pair(&src, base, "unit")->key);
    CHECK(c, origin->value == find_pair(&src, base, "unit")->value);
    CHECK(c, origin->hop_count == 1);
    CHECK(c, origin->hops != NULL);
    CHECK(c, origin->hops[0].mapping == row_after);
    CHECK(c, origin->hops[0].pair == pair_index(&src, row_after, "<<"));
    CHECK(c, origin->hops[0].target == base);

    /* The inherited scalar keeps the source marks and style. */
    {
        yaml_node_t *src_value =
            yaml_document_get_node(&src, origin->value);
        yaml_node_t *out_value = yaml_document_get_node(&result.document,
                find_pair(&result.document, out_after, "unit")->value);
        CHECK(c, src_value->type == YAML_SCALAR_NODE);
        CHECK(c, out_value->start_mark.index == src_value->start_mark.index);
        CHECK(c, out_value->start_mark.line == src_value->start_mark.line);
        CHECK(c, out_value->start_mark.column
                == src_value->start_mark.column);
        CHECK(c, out_value->end_mark.index == src_value->end_mark.index);
        CHECK(c, out_value->data.scalar.style
                == src_value->data.scalar.style);
    }

    yaml_merge_result_delete(&result);
    yaml_document_delete(&src);
    yaml_parser_delete(&parser);
}

static void
test_precedence_sequences(unit_context *c)
{
    static const char text[] =
        "first: &f {w: 1, z: 2}\n"
        "second: &s {w: 9, q: 5}\n"
        "merged: {<<: [*f, *s], z: 7}\n"
        "multi: {<<: *f, <<: *s}\n";
    yaml_parser_t parser;
    yaml_document_t src;
    yaml_merge_result_t result;
    yaml_merge_error_t error;
    int first, second, merged, multi;
    int out_merged, out_multi;
    const yaml_merge_pair_info_t *info;
    yaml_node_t *map;

    CHECK(c, load_string(text, &parser, &src));
    memset(&result, 0, sizeof(result));
    memset(&error, 0, sizeof(error));

    first = value_id(&src, 1, "first");
    second = value_id(&src, 1, "second");
    merged = value_id(&src, 1, "merged");
    multi = value_id(&src, 1, "multi");
    CHECK(c, first && second && merged && multi);

    CHECK(c, yaml_document_expand_merges(&src, &result, &error) == 1);

    out_merged = value_id(&result.document, 1, "merged");
    out_multi = value_id(&result.document, 1, "multi");
    map = yaml_document_get_node(&result.document, out_merged);
    CHECK(c, map->data.mapping.pairs.top - map->data.mapping.pairs.start
            == 3);

    /* Local z first, then first-source w, then second-source q. */
    CHECK(c, scalar_is(&result.document,
                map->data.mapping.pairs.start[0].key, "z"));
    CHECK(c, scalar_is(&result.document,
                map->data.mapping.pairs.start[1].key, "w"));
    CHECK(c, scalar_is(&result.document,
                map->data.mapping.pairs.start[2].key, "q"));
    CHECK(c, scalar_is(&result.document,
                value_id(&result.document, out_merged, "z"), "7"));
    CHECK(c, scalar_is(&result.document,
                value_id(&result.document, out_merged, "w"), "1"));
    CHECK(c, scalar_is(&result.document,
                value_id(&result.document, out_merged, "q"), "5"));

    info = yaml_merge_result_get_mapping_info(&result, out_merged);
    CHECK(c, info->origins[1].mapping == first);   /* w from first */
    CHECK(c, info->origins[2].mapping == second);  /* q from second */
    CHECK(c, info->origins[1].hop_count == 1);
    CHECK(c, info->origins[1].hops[0].target == first);

    /* Two merges: the left merge source wins a shared key. */
    map = yaml_document_get_node(&result.document, out_multi);
    CHECK(c, map->data.mapping.pairs.top - map->data.mapping.pairs.start
            == 3);
    CHECK(c, scalar_is(&result.document,
                value_id(&result.document, out_multi, "w"), "1"));
    CHECK(c, scalar_is(&result.document,
                value_id(&result.document, out_multi, "z"), "2"));
    CHECK(c, scalar_is(&result.document,
                value_id(&result.document, out_multi, "q"), "5"));
    info = yaml_merge_result_get_mapping_info(&result, out_multi);
    CHECK(c, info->origins[0].mapping == first);   /* w from first */
    CHECK(c, info->origins[1].mapping == first);   /* z from first */
    CHECK(c, info->origins[2].mapping == second);  /* q from second */
    CHECK(c, info->origins[0].hop_count == 1);
    CHECK(c, info->origins[0].hops[0].target == first);

    yaml_merge_result_delete(&result);
    yaml_document_delete(&src);
    yaml_parser_delete(&parser);
}

static void
test_precedence_multilevel(unit_context *c)
{
    static const char text[] =
        "base: &b {id: 7, unit: kg}\n"
        "mid: &m {<<: *b, extra: x}\n"
        "top: {<<: *m, id: 1}\n";
    yaml_parser_t parser;
    yaml_document_t src;
    yaml_merge_result_t result;
    yaml_merge_error_t error;
    int base, mid, top, out_top;
    const yaml_merge_pair_info_t *info;
    const yaml_merge_origin_t *unit_origin;

    CHECK(c, load_string(text, &parser, &src));
    memset(&result, 0, sizeof(result));
    memset(&error, 0, sizeof(error));

    base = value_id(&src, 1, "base");
    mid = value_id(&src, 1, "mid");
    top = value_id(&src, 1, "top");
    CHECK(c, base && mid && top);

    CHECK(c, yaml_document_expand_merges(&src, &result, &error) == 1);
    out_top = value_id(&result.document, 1, "top");
    info = yaml_merge_result_get_mapping_info(&result, out_top);
    CHECK(c, info != NULL);
    CHECK(c, info->pair_count == 3);

    /* id stays local; extra stops at mid; unit is traced back to base. */
    CHECK(c, scalar_is(&result.document,
                value_id(&result.document, out_top, "id"), "1"));
    CHECK(c, scalar_is(&result.document,
                value_id(&result.document, out_top, "extra"), "x"));
    CHECK(c, scalar_is(&result.document,
                value_id(&result.document, out_top, "unit"), "kg"));

    CHECK(c, info->origins[0].mapping == top);
    CHECK(c, info->origins[0].hop_count == 0);
    CHECK(c, info->origins[1].mapping == mid);
    CHECK(c, info->origins[1].hop_count == 1);
    CHECK(c, info->origins[1].hops[0].mapping == top);
    CHECK(c, info->origins[1].hops[0].target == mid);

    unit_origin = &info->origins[2];
    CHECK(c, unit_origin->mapping == base);
    CHECK(c, unit_origin->pair == pair_index(&src, base, "unit"));
    CHECK(c, unit_origin->hop_count == 2);
    CHECK(c, unit_origin->hops[0].mapping == top);
    CHECK(c, unit_origin->hops[0].pair == pair_index(&src, top, "<<"));
    CHECK(c, unit_origin->hops[0].target == mid);
    CHECK(c, unit_origin->hops[1].mapping == mid);
    CHECK(c, unit_origin->hops[1].pair == pair_index(&src, mid, "<<"));
    CHECK(c, unit_origin->hops[1].target == base);

    yaml_merge_result_delete(&result);
    yaml_document_delete(&src);
    yaml_parser_delete(&parser);
}

/*
 * An explicit !!merge tag makes any scalar a merge key, and key
 * identity is tag plus bytes compared by length, so embedded NULs and
 * differing lengths never collapse two fields together.
 */

static void
test_precedence_tag_and_length(unit_context *c)
{
    static const char tagged[] =
        "base: &bm {k: 1}\n"
        "row:\n"
        "  ? !!merge custom\n"
        "  : *bm\n";
    yaml_parser_t parser;
    yaml_document_t src;
    yaml_merge_result_t result;
    yaml_merge_error_t error;
    int row, out_row;
    const yaml_merge_pair_info_t *info;

    /* The explicit merge tag applies even though the key is not "<<". */
    CHECK(c, load_string(tagged, &parser, &src));
    memset(&result, 0, sizeof(result));
    memset(&error, 0, sizeof(error));
    row = value_id(&src, 1, "row");
    CHECK(c, row);
    CHECK(c, yaml_document_expand_merges(&src, &result, &error) == 1);
    CHECK(c, error.type == YAML_MERGE_OK);
    out_row = value_id(&result.document, 1, "row");
    CHECK(c, find_pair(&result.document, out_row, "custom") == NULL);
    CHECK(c, find_pair(&result.document, out_row, "k") != NULL);
    CHECK(c, scalar_is(&result.document,
                value_id(&result.document, out_row, "k"), "1"));
    info = yaml_merge_result_get_mapping_info(&result, out_row);
    CHECK(c, info != NULL && info->pair_count == 1);
    CHECK(c, info->origins[0].hop_count == 1);
    yaml_merge_result_delete(&result);
    yaml_document_delete(&src);
    yaml_parser_delete(&parser);

    /* Length-sensitive, NUL-aware key identity on a built document. */
    {
        static const yaml_char_t key_short[] = { 'a', 'b' };
        static const yaml_char_t key_long[] = { 'a', 'b', 0, 'c' };
        static const yaml_char_t merge_key[] = { '<', '<' };
        const yaml_char_t *str_tag = (const yaml_char_t *)YAML_STR_TAG;
        const yaml_char_t *map_tag = (const yaml_char_t *)YAML_MAP_TAG;
        yaml_document_t doc;
        int root, kshort, klong, mk, vlocal, vother, vbase, base, row_id;
        int out_map;
        yaml_node_pair_t *short_pair, *long_pair;

        memset(&result, 0, sizeof(result));
        memset(&error, 0, sizeof(error));
        CHECK(c, yaml_document_initialize(&doc, NULL, NULL, NULL, 1, 1));

        /* The root must be node id 1, so create it before its children. */
        root = yaml_document_add_mapping(&doc, map_tag,
                YAML_FLOW_MAPPING_STYLE);
        CHECK(c, root == 1);

        kshort = yaml_document_add_scalar(&doc, str_tag,
                key_short, 2, YAML_PLAIN_SCALAR_STYLE);
        klong = yaml_document_add_scalar(&doc, str_tag,
                key_long, 4, YAML_PLAIN_SCALAR_STYLE);
        mk = yaml_document_add_scalar(&doc, str_tag,
                merge_key, 2, YAML_PLAIN_SCALAR_STYLE);
        vlocal = yaml_document_add_scalar(&doc, str_tag,
                (const yaml_char_t *)"local", -1,
                YAML_PLAIN_SCALAR_STYLE);
        vother = yaml_document_add_scalar(&doc, str_tag,
                (const yaml_char_t *)"other", -1,
                YAML_PLAIN_SCALAR_STYLE);
        vbase = yaml_document_add_scalar(&doc, str_tag,
                (const yaml_char_t *)"base", -1,
                YAML_PLAIN_SCALAR_STYLE);

        base = yaml_document_add_mapping(&doc, map_tag,
                YAML_FLOW_MAPPING_STYLE);
        CHECK(c, yaml_document_append_mapping_pair(&doc, base, kshort,
                    vbase));

        row_id = yaml_document_add_mapping(&doc, map_tag,
                YAML_FLOW_MAPPING_STYLE);
        CHECK(c, yaml_document_append_mapping_pair(&doc, row_id, mk,
                    base));
        CHECK(c, yaml_document_append_mapping_pair(&doc, row_id, kshort,
                    vlocal));
        CHECK(c, yaml_document_append_mapping_pair(&doc, row_id, klong,
                    vother));

        CHECK(c, yaml_document_append_mapping_pair(&doc, root,
                    yaml_document_add_scalar(&doc, str_tag,
                        (const yaml_char_t *)"row", -1,
                        YAML_PLAIN_SCALAR_STYLE),
                    row_id));

        CHECK(c, yaml_document_expand_merges(&doc, &result, &error) == 1);
        CHECK(c, error.type == YAML_MERGE_OK);

        out_map = value_id(&result.document, 1, "row");
        CHECK(c, out_map);
        short_pair = find_pair_n(&result.document, out_map, "ab", 2);
        long_pair = find_pair_n(&result.document, out_map, "ab\0c", 4);
        CHECK(c, short_pair != NULL);
        CHECK(c, long_pair != NULL);
        CHECK(c, short_pair != long_pair);
        CHECK(c, scalar_is(&result.document, short_pair->value, "local"));
        CHECK(c, scalar_is(&result.document, long_pair->value, "other"));

        /* The base "ab" field lost to the local same-length field. */
        info = yaml_merge_result_get_mapping_info(&result, out_map);
        CHECK(c, info != NULL && info->pair_count == 2);
        CHECK(c, info->origins[0].mapping == row_id);
        CHECK(c, info->origins[0].hop_count == 0);
        CHECK(c, info->origins[1].mapping == row_id);

        yaml_merge_result_delete(&result);
        yaml_document_delete(&doc);
    }
}

/*
 * graph: shared aliases keep one node identity, ordinary cycles are
 * allowed, quoted "<<" stays an ordinary field and merge cycles fail.
 */

static void
test_graph_identity(unit_context *c)
{
    static const char text[] =
        "shared: &s {n: 1}\n"
        "word: &w hello\n"
        "holder:\n"
        "  a: *s\n"
        "  b: *s\n"
        "box: [*w, *w]\n"
        "loop: &c [*c]\n";
    yaml_parser_t parser;
    yaml_document_t src;
    yaml_merge_result_t result;
    yaml_merge_error_t error;
    int holder, box, loop, shared_src, word_src;
    int out_holder, out_box, out_loop, out_shared, out_word;
    yaml_node_t *src_scalar, *out_scalar;
    const unsigned char *borrowed_value;
    yaml_node_t *loop_node;

    CHECK(c, load_string(text, &parser, &src));
    memset(&result, 0, sizeof(result));
    memset(&error, 0, sizeof(error));

    holder = value_id(&src, 1, "holder");
    box = value_id(&src, 1, "box");
    loop = value_id(&src, 1, "loop");
    shared_src = value_id(&src, 1, "shared");
    word_src = value_id(&src, 1, "box");
    CHECK(c, holder && box && loop && shared_src && word_src);
    word_src = yaml_document_get_node(&src, box)
        ->data.sequence.items.start[0];

    src_scalar = yaml_document_get_node(&src, word_src);
    borrowed_value = src_scalar->data.scalar.value;

    CHECK(c, yaml_document_expand_merges(&src, &result, &error) == 1);

    out_holder = value_id(&result.document, 1, "holder");
    out_box = value_id(&result.document, 1, "box");
    out_loop = value_id(&result.document, 1, "loop");
    out_shared = value_id(&result.document, out_holder, "a");

    /* Aliases resolve to the same output node id. */
    CHECK(c, out_shared == value_id(&result.document, out_holder, "b"));
    CHECK(c, yaml_document_get_node(&result.document, out_box)
            ->data.sequence.items.start[0]
        == yaml_document_get_node(&result.document, out_box)
            ->data.sequence.items.start[1]);

    /* Container style and marks are preserved. */
    {
        yaml_node_t *src_map = yaml_document_get_node(&src, shared_src);
        yaml_node_t *out_map =
            yaml_document_get_node(&result.document, out_shared);
        CHECK(c, out_map->type == YAML_MAPPING_NODE);
        CHECK(c, out_map->data.mapping.style
                == src_map->data.mapping.style);
        CHECK(c, out_map->start_mark.index == src_map->start_mark.index);
        CHECK(c, out_map->end_mark.index == src_map->end_mark.index);
    }

    /* An ordinary value cycle survives: the sequence contains itself. */
    loop_node = yaml_document_get_node(&result.document, out_loop);
    CHECK(c, loop_node->type == YAML_SEQUENCE_NODE);
    CHECK(c, loop_node->data.sequence.items.start[0] == out_loop);

    /* The output owns its scalar bytes instead of borrowing them. */
    out_word = yaml_document_get_node(&result.document, out_box)
        ->data.sequence.items.start[0];
    out_scalar = yaml_document_get_node(&result.document, out_word);
    CHECK(c, out_scalar->data.scalar.value != borrowed_value);
    CHECK(c, out_scalar->data.scalar.length == 5);
    CHECK(c, memcmp(out_scalar->data.scalar.value, "hello", 5) == 0);
    CHECK(c, out_scalar->data.scalar.style == src_scalar->data.scalar.style);
    CHECK(c, strcmp((char *)out_scalar->tag,
                (char *)src_scalar->tag) == 0);

    /* The source must remain intact and readable after expansion. */
    CHECK(c, yaml_document_get_node(&src, shared_src) != NULL);
    CHECK(c, yaml_document_get_node(&src, loop)->type
            == YAML_SEQUENCE_NODE);

    /* Delete the input; the result must still be fully readable. */
    yaml_document_delete(&src);
    yaml_parser_delete(&parser);
    CHECK(c, scalar_is(&result.document,
                value_id(&result.document, out_shared, "n"), "1"));
    CHECK(c, memcmp(out_scalar->data.scalar.value, "hello", 5) == 0);
    CHECK(c, loop_node->data.sequence.items.start[0] == out_loop);

    /* The independent result must still be emittable after the input
     * is gone.  yaml_emitter_dump() consumes and empties the document,
     * after which yaml_merge_result_delete() is still safe. */
    {
        yaml_emitter_t emitter;
        memory_buffer emitted = { NULL, 0, 0 };
        CHECK(c, yaml_emitter_initialize(&emitter));
        yaml_emitter_set_output(&emitter, memory_write, &emitted);
        CHECK(c, yaml_emitter_open(&emitter));
        CHECK(c, yaml_emitter_dump(&emitter, &result.document));
        CHECK(c, yaml_emitter_close(&emitter));
        yaml_emitter_delete(&emitter);
        CHECK(c, emitted.length > 0);
        CHECK(c, buffer_contains(&emitted, "hello"));
        free(emitted.data);
    }

    yaml_merge_result_delete(&result);
}

static void
test_graph_quoted_key(unit_context *c)
{
    static const char text[] =
        "base: &bq {\"<<\": {x: 1}, y: 2}\n"
        "row: {<<: *bq}\n"
        "plain: {\"<<\": 5}\n";
    yaml_parser_t parser;
    yaml_document_t src;
    yaml_merge_result_t result;
    yaml_merge_error_t error;
    int base, row, plain, out_row, out_plain;
    yaml_node_t *row_map, *plain_map, *key_node, *quoted_value;
    const yaml_merge_pair_info_t *info;

    CHECK(c, load_string(text, &parser, &src));
    memset(&result, 0, sizeof(result));
    memset(&error, 0, sizeof(error));

    base = value_id(&src, 1, "base");
    row = value_id(&src, 1, "row");
    plain = value_id(&src, 1, "plain");
    CHECK(c, base && row && plain);

    CHECK(c, yaml_document_expand_merges(&src, &result, &error) == 1);

    out_row = value_id(&result.document, 1, "row");
    out_plain = value_id(&result.document, 1, "plain");
    row_map = yaml_document_get_node(&result.document, out_row);
    plain_map = yaml_document_get_node(&result.document, out_plain);

    /* The quoted "<<" is inherited as a normal field, alongside y. */
    CHECK(c, row_map->data.mapping.pairs.top
            - row_map->data.mapping.pairs.start == 2);
    CHECK(c, find_pair(&result.document, out_row, "<<") != NULL);
    CHECK(c, scalar_is(&result.document,
                value_id(&result.document, out_row, "y"), "2"));
    quoted_value = yaml_document_get_node(&result.document,
            find_pair(&result.document, out_row, "<<")->value);
    CHECK(c, quoted_value->type == YAML_MAPPING_NODE);
    CHECK(c, scalar_is(&result.document,
                value_id(&result.document,
                    find_pair(&result.document, out_row, "<<")->value,
                    "x"), "1"));

    /* The double-quoted style is preserved on the key. */
    key_node = yaml_document_get_node(&result.document,
            plain_map->data.mapping.pairs.start[0].key);
    CHECK(c, key_node->data.scalar.length == 2);
    CHECK(c, memcmp(key_node->data.scalar.value, "<<", 2) == 0);
    CHECK(c, key_node->data.scalar.style
            == YAML_DOUBLE_QUOTED_SCALAR_STYLE);
    CHECK(c, scalar_is(&result.document,
                plain_map->data.mapping.pairs.start[0].value, "5"));

    /* The quoted field is reported as a local, hop-less origin. */
    info = yaml_merge_result_get_mapping_info(&result, out_plain);
    CHECK(c, info != NULL && info->pair_count == 1);
    CHECK(c, info->origins[0].mapping == plain);
    CHECK(c, info->origins[0].hop_count == 0);

    yaml_merge_result_delete(&result);
    yaml_document_delete(&src);
    yaml_parser_delete(&parser);
}

static void
test_graph_merge_cycle(unit_context *c)
{
    static const char mutual[] =
        "a: &ma\n"
        "  inner: &mb\n"
        "    <<: *ma\n"
        "    kb: 2\n"
        "  <<: *mb\n"
        "  ka: 1\n";
    static const char self[] =
        "self: &s {<<: *s}\n";
    yaml_parser_t parser;
    yaml_document_t src;
    yaml_merge_result_t result;
    yaml_merge_error_t error;
    int a, b;

    /* Mutual merge dependency cycle.  The nested layout keeps every
     * alias pointing at an already declared anchor. */
    CHECK(c, load_string(mutual, &parser, &src));
    memset(&result, 0, sizeof(result));
    memset(&error, 0, sizeof(error));
    a = value_id(&src, 1, "a");
    b = value_id(&src, a, "inner");
    CHECK(c, a && b);

    CHECK(c, yaml_document_expand_merges(&src, &result, &error) == 0);
    CHECK(c, error.type == YAML_MERGE_CYCLE_ERROR);
    CHECK(c, error.mapping == a || error.mapping == b);
    CHECK(c, error.node == a || error.node == b);
    CHECK(c, result.document.nodes.start == NULL);
    CHECK(c, result.mapping_info == NULL);
    yaml_merge_result_delete(&result);   /* Safe on an empty result. */
    yaml_document_delete(&src);
    yaml_parser_delete(&parser);

    /* Direct self-merge cycle, with a NULL error object. */
    CHECK(c, load_string(self, &parser, &src));
    memset(&result, 0, sizeof(result));
    CHECK(c, yaml_document_expand_merges(&src, &result, NULL) == 0);
    CHECK(c, result.document.nodes.start == NULL);
    yaml_merge_result_delete(&result);
    yaml_document_delete(&src);
    yaml_parser_delete(&parser);
}

/*
 * errors: non-scalar keys, duplicate local keys and bad merge values
 * are rejected without a partial result and without touching the
 * input.
 */

static void
test_errors_keys(unit_context *c)
{
    static const char non_scalar[] =
        "root:\n"
        "  ? [complex, key]\n"
        "  : value\n";
    static const char duplicate[] =
        "dup: {a: 1, a: 2}\n";
    yaml_parser_t parser;
    yaml_document_t src;
    yaml_merge_result_t result;
    yaml_merge_error_t error;
    yaml_node_t *map;
    int root, dup, pair_count;

    CHECK(c, load_string(non_scalar, &parser, &src));
    memset(&result, 0, sizeof(result));
    memset(&error, 0, sizeof(error));
    root = value_id(&src, 1, "root");
    CHECK(c, root);
    CHECK(c, yaml_document_expand_merges(&src, &result, &error) == 0);
    CHECK(c, error.type == YAML_MERGE_NON_SCALAR_KEY_ERROR);
    CHECK(c, error.mapping == root);
    CHECK(c, error.pair == 0);
    CHECK(c, result.document.nodes.start == NULL);
    /* Input untouched. */
    map = yaml_document_get_node(&src, root);
    CHECK(c, map->type == YAML_MAPPING_NODE);
    CHECK(c, map->data.mapping.pairs.top - map->data.mapping.pairs.start
            == 1);
    yaml_merge_result_delete(&result);
    yaml_document_delete(&src);
    yaml_parser_delete(&parser);

    CHECK(c, load_string(duplicate, &parser, &src));
    memset(&result, 0, sizeof(result));
    memset(&error, 0, sizeof(error));
    dup = value_id(&src, 1, "dup");
    pair_count = (int)(yaml_document_get_node(&src, dup)
            ->data.mapping.pairs.top
        - yaml_document_get_node(&src, dup)->data.mapping.pairs.start);
    CHECK(c, pair_count == 2);
    CHECK(c, yaml_document_expand_merges(&src, &result, &error) == 0);
    CHECK(c, error.type == YAML_MERGE_DUPLICATE_KEY_ERROR);
    CHECK(c, error.mapping == dup);
    CHECK(c, error.pair == 1);
    CHECK(c, result.document.nodes.start == NULL);
    CHECK(c, (int)(yaml_document_get_node(&src, dup)
                ->data.mapping.pairs.top
            - yaml_document_get_node(&src, dup)
                ->data.mapping.pairs.start) == pair_count);
    yaml_merge_result_delete(&result);
    yaml_document_delete(&src);
    yaml_parser_delete(&parser);
}

static void
test_errors_merge_values(unit_context *c)
{
    static const char scalar_value[] =
        "bad: {<<: 5}\n";
    static const char sequence_value[] =
        "bad: {<<: [{a: 1}, nope]}\n";
    yaml_parser_t parser;
    yaml_document_t src;
    yaml_merge_result_t result;
    yaml_merge_error_t error;
    int bad, scalar_item;

    CHECK(c, load_string(scalar_value, &parser, &src));
    memset(&result, 0, sizeof(result));
    memset(&error, 0, sizeof(error));
    bad = value_id(&src, 1, "bad");
    CHECK(c, bad);
    CHECK(c, yaml_document_expand_merges(&src, &result, &error) == 0);
    CHECK(c, error.type == YAML_MERGE_INVALID_VALUE_ERROR);
    CHECK(c, error.mapping == bad);
    CHECK(c, error.pair == 0);
    CHECK(c, result.document.nodes.start == NULL);
    yaml_merge_result_delete(&result);
    yaml_document_delete(&src);
    yaml_parser_delete(&parser);

    CHECK(c, load_string(sequence_value, &parser, &src));
    memset(&result, 0, sizeof(result));
    memset(&error, 0, sizeof(error));
    bad = value_id(&src, 1, "bad");
    scalar_item = yaml_document_get_node(&src,
            find_pair(&src, bad, "<<")->value)
        ->data.sequence.items.start[1];
    CHECK(c, yaml_document_expand_merges(&src, &result, &error) == 0);
    CHECK(c, error.type == YAML_MERGE_INVALID_VALUE_ERROR);
    CHECK(c, error.node == scalar_item);
    CHECK(c, result.document.nodes.start == NULL);
    yaml_merge_result_delete(&result);
    yaml_document_delete(&src);
    yaml_parser_delete(&parser);
}

/*
 * roundtrip: an expanded document is emitted and reloaded, and the
 * reloaded graph keeps the expanded fields, alias identity, cycles,
 * styles, tags and document directives.
 */

static void
test_roundtrip_graph(unit_context *c)
{
    static const char text[] =
        "shared: &s {n: 1}\n"
        "base: &b {id: 7, unit: kg}\n"
        "row: {<<: *b, id: 9}\n"
        "holder: {a: *s, b: *s}\n"
        "loop: &lp [*lp]\n"
        "quoted: {\"<<\": 5}\n";
    yaml_parser_t parser;
    yaml_document_t src, reloaded;
    yaml_merge_result_t result;
    yaml_merge_error_t error;
    yaml_emitter_t emitter;
    memory_buffer output = { NULL, 0, 0 };
    int row, holder, loop;
    int out_row, re_row, re_holder, re_loop;

    CHECK(c, load_string(text, &parser, &src));
    memset(&result, 0, sizeof(result));
    memset(&error, 0, sizeof(error));
    row = value_id(&src, 1, "row");
    holder = value_id(&src, 1, "holder");
    loop = value_id(&src, 1, "loop");
    CHECK(c, row && holder && loop);

    CHECK(c, yaml_document_expand_merges(&src, &result, &error) == 1);
    CHECK(c, count_merge_keys(&result.document) == 0);

    out_row = value_id(&result.document, 1, "row");
    CHECK(c, scalar_is(&result.document,
                value_id(&result.document, out_row, "id"), "9"));
    CHECK(c, scalar_is(&result.document,
                value_id(&result.document, out_row, "unit"), "kg"));

    CHECK(c, yaml_emitter_initialize(&emitter));
    yaml_emitter_set_output(&emitter, memory_write, &output);
    CHECK(c, yaml_emitter_open(&emitter));
    CHECK(c, yaml_emitter_dump(&emitter, &result.document));
    CHECK(c, yaml_emitter_close(&emitter));
    yaml_emitter_delete(&emitter);
    CHECK(c, output.length > 0);
    yaml_merge_result_delete(&result);
    yaml_document_delete(&src);
    yaml_parser_delete(&parser);

    /* Reload the emitted text and check the cleaned graph. */
    CHECK(c, load_string((const char *)output.data, &parser, &reloaded));
    CHECK(c, count_merge_keys(&reloaded) == 0);

    re_row = value_id(&reloaded, 1, "row");
    re_holder = value_id(&reloaded, 1, "holder");
    re_loop = value_id(&reloaded, 1, "loop");
    CHECK(c, re_row && re_holder && re_loop);
    CHECK(c, scalar_is(&reloaded, value_id(&reloaded, re_row, "id"), "9"));
    CHECK(c, scalar_is(&reloaded, value_id(&reloaded, re_row, "unit"),
                "kg"));
    CHECK(c, find_pair(&reloaded, re_row, "<<") == NULL);
    CHECK(c, find_pair(&reloaded, value_id(&reloaded, 1, "quoted"),
                "<<") != NULL);

    /* Alias identity and the ordinary cycle survive dump/load. */
    CHECK(c, value_id(&reloaded, re_holder, "a")
            == value_id(&reloaded, re_holder, "b"));
    CHECK(c, yaml_document_get_node(&reloaded, re_loop)
            ->data.sequence.items.start[0] == re_loop);

    yaml_document_delete(&reloaded);
    yaml_parser_delete(&parser);
    free(output.data);
}

static void
test_roundtrip_directives(unit_context *c)
{
    static const char text[] =
        "%YAML 1.2\n"
        "%TAG !e! tag:example,2026:\n"
        "---\n"
        "base: &b {n: !e!thing 4}\n"
        "row: {<<: *b, mark: !e!other 8}\n"
        "...\n";
    yaml_parser_t parser;
    yaml_document_t src, reloaded;
    yaml_merge_result_t result;
    yaml_merge_error_t error;
    yaml_emitter_t emitter;
    memory_buffer output = { NULL, 0, 0 };
    int row, out_row, re_row, n_node, mark_node;

    CHECK(c, load_string(text, &parser, &src));
    CHECK(c, src.version_directive != NULL);
    CHECK(c, src.version_directive->major == 1);
    CHECK(c, src.version_directive->minor == 2);
    CHECK(c, src.tag_directives.end - src.tag_directives.start == 1);

    memset(&result, 0, sizeof(result));
    memset(&error, 0, sizeof(error));
    CHECK(c, yaml_document_expand_merges(&src, &result, &error) == 1);

    /* Directives are owned by the expanded document. */
    CHECK(c, result.document.version_directive != NULL);
    CHECK(c, result.document.version_directive->minor == 2);
    CHECK(c, result.document.tag_directives.end
            - result.document.tag_directives.start == 1);
    CHECK(c, result.document.tag_directives.start[0].handle
            != src.tag_directives.start[0].handle);

    row = value_id(&src, 1, "row");
    out_row = value_id(&result.document, 1, "row");
    CHECK(c, row && out_row);
    mark_node = find_pair(&result.document, out_row, "mark")->value;
    CHECK(c, strcmp((char *)yaml_document_get_node(&result.document,
                    mark_node)->tag, "tag:example,2026:other") == 0);
    n_node = value_id(&result.document,
            value_id(&result.document, 1, "base"), "n");
    CHECK(c, strcmp((char *)yaml_document_get_node(&result.document,
                    n_node)->tag, "tag:example,2026:thing") == 0);

    CHECK(c, yaml_emitter_initialize(&emitter));
    yaml_emitter_set_output(&emitter, memory_write, &output);
    CHECK(c, yaml_emitter_open(&emitter));
    CHECK(c, yaml_emitter_dump(&emitter, &result.document));
    CHECK(c, yaml_emitter_close(&emitter));
    yaml_emitter_delete(&emitter);
    yaml_merge_result_delete(&result);
    yaml_document_delete(&src);
    yaml_parser_delete(&parser);

    CHECK(c, load_string((const char *)output.data, &parser, &reloaded));
    CHECK(c, reloaded.version_directive != NULL);
    CHECK(c, reloaded.version_directive->major == 1);
    CHECK(c, reloaded.version_directive->minor == 2);
    CHECK(c, reloaded.tag_directives.end
            - reloaded.tag_directives.start == 1);
    re_row = value_id(&reloaded, 1, "row");
    CHECK(c, scalar_is(&reloaded, value_id(&reloaded, re_row, "n"), "4"));
    CHECK(c, scalar_is(&reloaded, value_id(&reloaded, re_row, "mark"),
                "8"));
    CHECK(c, strcmp((char *)yaml_document_get_node(&reloaded,
                    find_pair(&reloaded, re_row, "mark")->value)->tag,
                "tag:example,2026:other") == 0);

    yaml_document_delete(&reloaded);
    yaml_parser_delete(&parser);
    free(output.data);
}

const unit_case unit_cases[] = {
    { "precedence", "basic_local_and_origin", test_precedence_basic },
    { "precedence", "sequences_and_multi_merge",
        test_precedence_sequences },
    { "precedence", "multilevel_hop_chain", test_precedence_multilevel },
    { "precedence", "explicit_tag_and_length_identity",
        test_precedence_tag_and_length },
    { "graph", "shared_identity_and_cycles", test_graph_identity },
    { "graph", "quoted_merge_key", test_graph_quoted_key },
    { "graph", "merge_dependency_cycle", test_graph_merge_cycle },
    { "errors", "non_scalar_and_duplicate_keys", test_errors_keys },
    { "errors", "invalid_merge_values", test_errors_merge_values },
    { "roundtrip", "emit_and_reload_graph", test_roundtrip_graph },
    { "roundtrip", "directives_tags_and_styles",
        test_roundtrip_directives },
};

const unsigned unit_case_count =
    (unsigned)(sizeof(unit_cases) / sizeof(unit_cases[0]));