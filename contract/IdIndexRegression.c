/*
 Regression tests for the optional id index.

 Build/run through:  bash task_support/run.sh target
*/

#include "task_support/check.h"

#include <myhtml/api.h>

#include <string.h>

/* ------------------------------------------------------------------ */

struct fixture {
    myhtml_t      *myhtml;
    myhtml_tree_t *tree;
};

static int fixture_init(struct fixture *f, const char *html, size_t html_len)
{
    f->myhtml = myhtml_create();
    TASK_CHECK(f->myhtml != NULL);

    TASK_CHECK(myhtml_init(f->myhtml, MyHTML_OPTIONS_PARSE_MODE_SINGLE, 1, 0) == MyHTML_STATUS_OK);

    f->tree = myhtml_tree_create();
    TASK_CHECK(f->tree != NULL);

    TASK_CHECK(myhtml_tree_init(f->tree, f->myhtml) == MyHTML_STATUS_OK);
    TASK_CHECK(myhtml_parse(f->tree, MyENCODING_UTF_8, html, html_len) == MyHTML_STATUS_OK);

    return 0;
}

static int fixture_done(struct fixture *f)
{
    myhtml_tree_destroy(f->tree);
    myhtml_destroy(f->myhtml);

    return 0;
}

static myhtml_tree_node_t * node_by_attr(myhtml_tree_t *tree, const char *key,
                                         const char *value)
{
    myhtml_collection_t *c = myhtml_get_nodes_by_attribute_value(tree, NULL, NULL, false,
                                                                 key, strlen(key),
                                                                 value, strlen(value), NULL);

    myhtml_tree_node_t *node = NULL;

    if(c && c->length == 1)
        node = c->list[0];

    myhtml_collection_destroy(c);

    return node;
}

/* compare a query result against an explicit expected node sequence */
static int matches(myhtml_collection_t *c, myhtml_tree_node_t **expected, size_t count)
{
    size_t i;

    if(c == NULL || c->length != count)
        return 0;

    for(i = 0; i < count; i++) {
        if(c->list[i] != expected[i])
            return 0;
    }

    return 1;
}

/* query "value" and require exactly the given nodes, in this order */
static int query_matches(myhtml_tree_t *tree, const char *value,
                         myhtml_tree_node_t **expected, size_t count)
{
    myhtml_collection_t *c;
    mystatus_t status = MyHTML_STATUS_ERROR;
    int ok;

    c = myhtml_get_nodes_by_id(tree, NULL, value, strlen(value), &status);

    if(status != MyHTML_STATUS_OK) {
        myhtml_collection_destroy(c);
        return 0;
    }

    ok = matches(c, expected, count);
    myhtml_collection_destroy(c);

    return ok;
}

/* query "value" and require an empty but valid result */
static int query_empty(myhtml_tree_t *tree, const char *value, size_t value_len)
{
    myhtml_collection_t *c;
    mystatus_t status = MyHTML_STATUS_ERROR;
    int ok;

    c = myhtml_get_nodes_by_id(tree, NULL, value, value_len, &status);

    if(c == NULL || status != MyHTML_STATUS_OK)
        ok = 0;
    else
        ok = (c->length == 0);

    myhtml_collection_destroy(c);

    return ok;
}

/* ------------------------------------------------------------------ */

/*
 * BUCKETS: build-time scan, exact decoded bytes, order, duplicates.
 */

static const char *buckets_html =
    "<!DOCTYPE html>"
    "<html><head><title>x</title></head><body>"
    "<div id='dup' data-row='first'>"
      "<span id='inner-one' data-row='i1'>t</span>"
    "</div>"
    "<section id='dup' data-row='second'>"
      "<p id='inner-two' data-row='i2'>t</p>"
    "</section>"
    "<div id='esc&amp;aped' data-row='esc'>e</div>"
    "<div id='CaSe' data-row='case'>c</div>"
    "<div id=' spaced ' data-row='spaced'>s</div>"
    "<div id='' data-row='empty'>0</div>"
    "<div data-row='noid'>n</div>"
    "<div id='tree' data-row='tree-root'>"
      "<ul id='tree'>"
        "<li id='tree' data-row='tree-a'>a</li>"
        "<li data-row='tree-mid'>"
          "<span id='tree' data-row='tree-b'>b</span>"
        "</li>"
      "</ul>"
      "<div id='tree' data-row='tree-c'>c</div>"
    "</div>"
    "</body></html>";

static int case_buckets_dup_order(void)
{
    struct fixture f;
    myhtml_tree_node_t *first, *second, *inner_one, *inner_two;

    if(fixture_init(&f, buckets_html, strlen(buckets_html)))
        return 1;

    first     = node_by_attr(f.tree, "data-row", "first");
    second    = node_by_attr(f.tree, "data-row", "second");
    inner_one = node_by_attr(f.tree, "data-row", "i1");
    inner_two = node_by_attr(f.tree, "data-row", "i2");

    TASK_CHECK(first && second && inner_one && inner_two);
    TASK_CHECK(myhtml_id_index_enable(f.tree) == MyHTML_STATUS_OK);

    /* every same-id node in actual DOM pre-order, each node once */
    {
        myhtml_tree_node_t *expect[] = {first, second};
        TASK_CHECK(query_matches(f.tree, "dup", expect, 2));
    }

    /* nested ids resolve to their own nodes */
    {
        myhtml_tree_node_t *expect[] = {inner_one};
        TASK_CHECK(query_matches(f.tree, "inner-one", expect, 1));
    }
    {
        myhtml_tree_node_t *expect[] = {inner_two};
        TASK_CHECK(query_matches(f.tree, "inner-two", expect, 1));
    }

    /* enable is idempotent and does not duplicate entries */
    TASK_CHECK(myhtml_id_index_enable(f.tree) == MyHTML_STATUS_OK);
    {
        myhtml_tree_node_t *expect[] = {first, second};
        TASK_CHECK(query_matches(f.tree, "dup", expect, 2));
    }

    /* ancestor, nested child and later sibling: pre-order, not the
     * order the bucket list happens to hold */
    {
        myhtml_tree_node_t *tree_root = node_by_attr(f.tree, "data-row", "tree-root");
        myhtml_tree_node_t *tree_a    = node_by_attr(f.tree, "data-row", "tree-a");
        myhtml_tree_node_t *tree_b    = node_by_attr(f.tree, "data-row", "tree-b");
        myhtml_tree_node_t *tree_c    = node_by_attr(f.tree, "data-row", "tree-c");
        myhtml_tree_node_t *tree_ul   = myhtml_node_child(tree_root);
        myhtml_tree_node_t *expect[]  = {tree_root, tree_ul, tree_a, tree_b, tree_c};

        TASK_CHECK(tree_root && tree_ul && tree_a && tree_b && tree_c);
        TASK_CHECK(myhtml_node_tag_id(tree_ul) == MyHTML_TAG_UL);
        TASK_CHECK(query_matches(f.tree, "tree", expect, 5));
    }

    return fixture_done(&f);
}

static int case_buckets_entities_and_case(void)
{
    struct fixture f;
    myhtml_tree_node_t *escaped, *mixed, *spaced;

    if(fixture_init(&f, buckets_html, strlen(buckets_html)))
        return 1;

    escaped = node_by_attr(f.tree, "data-row", "esc");
    mixed   = node_by_attr(f.tree, "data-row", "case");
    spaced  = node_by_attr(f.tree, "data-row", "spaced");

    TASK_CHECK(escaped && mixed && spaced);
    TASK_CHECK(myhtml_id_index_enable(f.tree) == MyHTML_STATUS_OK);

    /* comparison is against the decoded bytes, e.g. "esc&aped" */
    {
        myhtml_tree_node_t *expect[] = {escaped};
        TASK_CHECK(query_matches(f.tree, "esc&aped", expect, 1));
    }
    TASK_CHECK(query_empty(f.tree, "esc&amp;aped", 10));

    /* no case folding */
    {
        myhtml_tree_node_t *expect[] = {mixed};
        TASK_CHECK(query_matches(f.tree, "CaSe", expect, 1));
    }
    TASK_CHECK(query_empty(f.tree, "case", 4));
    TASK_CHECK(query_empty(f.tree, "CASE", 4));

    /* no whitespace folding */
    {
        myhtml_tree_node_t *expect[] = {spaced};
        TASK_CHECK(query_matches(f.tree, " spaced ", expect, 1));
    }
    TASK_CHECK(query_empty(f.tree, "spaced", 6));

    /* missing and empty ids never enter the index */
    TASK_CHECK(query_empty(f.tree, "", 0));
    TASK_CHECK(query_empty(f.tree, NULL, 0));

    /* unknown value yields an empty collection, not NULL */
    TASK_CHECK(query_empty(f.tree, "nope", 4));

    return fixture_done(&f);
}

static int case_buckets_html_root(void)
{
    struct fixture f;
    const char *html = "<html id='root-id'><body><p id='leaf'>x</p></body></html>";
    myhtml_tree_node_t *html_root, *leaf;

    if(fixture_init(&f, html, strlen(html)))
        return 1;

    html_root = myhtml_tree_get_node_html(f.tree);
    leaf      = node_by_attr(f.tree, "id", "leaf");
    TASK_CHECK(html_root && leaf);
    TASK_CHECK(myhtml_id_index_enable(f.tree) == MyHTML_STATUS_OK);

    /* the html element itself is in scope */
    {
        myhtml_tree_node_t *expect[] = {html_root};
        TASK_CHECK(query_matches(f.tree, "root-id", expect, 1));
    }
    {
        myhtml_tree_node_t *expect[] = {leaf};
        TASK_CHECK(query_matches(f.tree, "leaf", expect, 1));
    }

    return fixture_done(&f);
}

static int case_buckets_without_enable(void)
{
    struct fixture f;
    const char *html = "<html><body><div id='x'>x</div></body></html>";
    mystatus_t status = MyHTML_STATUS_OK;
    myhtml_collection_t *c;

    if(fixture_init(&f, html, strlen(html)))
        return 1;

    /* disabled: NULL result and an error status */
    c = myhtml_get_nodes_by_id(f.tree, NULL, "x", 1, &status);
    TASK_CHECK(c == NULL);
    TASK_CHECK(status == MyHTML_STATUS_ERROR);

    /* disable is safe when never enabled, and repeatable */
    myhtml_id_index_disable(f.tree);
    myhtml_id_index_disable(f.tree);

    c = myhtml_get_nodes_by_id(f.tree, NULL, "x", 1, &status);
    TASK_CHECK(c == NULL && status == MyHTML_STATUS_ERROR);

    return fixture_done(&f);
}

/* ------------------------------------------------------------------ */

/*
 * MUTATIONS: attribute changes, duplicate promotion, moves, clone, delete.
 */

static int case_mutations_duplicate_promotion(void)
{
    struct fixture f;
    const char *html = "<html><body>"
                       "<div id='one' data-row='a'>a</div>"
                       "<div data-row='b'>b</div>"
                       "</body></html>";
    myhtml_tree_node_t *a, *b;

    if(fixture_init(&f, html, strlen(html)))
        return 1;

    a = node_by_attr(f.tree, "data-row", "a");
    b = node_by_attr(f.tree, "data-row", "b");
    TASK_CHECK(a && b);

    TASK_CHECK(myhtml_id_index_enable(f.tree) == MyHTML_STATUS_OK);

    /* add an id to b: it joins the "one" bucket after a */
    TASK_CHECK(myhtml_attribute_add(b, "id", 2, "one", 3, MyENCODING_UTF_8) != NULL);
    {
        myhtml_tree_node_t *expect[] = {a, b};
        TASK_CHECK(query_matches(f.tree, "one", expect, 2));
    }

    /* a second id on b is shadowed by its first one */
    TASK_CHECK(myhtml_attribute_add(b, "id", 2, "two", 3, MyENCODING_UTF_8) != NULL);
    TASK_CHECK(query_empty(f.tree, "two", 3));
    {
        myhtml_tree_node_t *expect[] = {a, b};
        TASK_CHECK(query_matches(f.tree, "one", expect, 2));
    }

    /* removing b's first id promotes the next one */
    myhtml_attribute_remove_by_key(b, "id", 2);
    {
        myhtml_tree_node_t *expect[] = {a};
        TASK_CHECK(query_matches(f.tree, "one", expect, 1));
    }
    {
        myhtml_tree_node_t *expect[] = {b};
        TASK_CHECK(query_matches(f.tree, "two", expect, 1));
    }

    /* attribute_remove of the (now promoted) explicit reference */
    {
        myhtml_tree_attr_t *two_attr = myhtml_attribute_by_key(b, "id", 2);
        TASK_CHECK(two_attr != NULL);
        TASK_CHECK(myhtml_attribute_remove(b, two_attr) == two_attr);
    }
    TASK_CHECK(query_empty(f.tree, "two", 3));

    /* a detached node that gains an id stays out until it is attached */
    myhtml_node_remove(b);
    TASK_CHECK(myhtml_attribute_add(b, "id", 2, "offline", 7, MyENCODING_UTF_8) != NULL);
    TASK_CHECK(query_empty(f.tree, "offline", 7));

    myhtml_node_append_child(myhtml_tree_get_node_html(f.tree), b);
    {
        myhtml_tree_node_t *expect[] = {b};
        TASK_CHECK(query_matches(f.tree, "offline", expect, 1));
    }

    return fixture_done(&f);
}

static int case_mutations_attribute_delete(void)
{
    struct fixture f;
    const char *html = "<html><body><div id='keep'>k</div><div id='drop'>d</div></body></html>";
    myhtml_tree_node_t *keep, *drop;
    myhtml_tree_attr_t *drop_attr;

    if(fixture_init(&f, html, strlen(html)))
        return 1;

    keep = node_by_attr(f.tree, "id", "keep");
    drop = node_by_attr(f.tree, "id", "drop");
    TASK_CHECK(keep && drop);

    TASK_CHECK(myhtml_id_index_enable(f.tree) == MyHTML_STATUS_OK);

    drop_attr = myhtml_attribute_by_key(drop, "id", 2);
    TASK_CHECK(drop_attr != NULL);

    myhtml_attribute_delete(f.tree, drop, drop_attr);

    {
        myhtml_tree_node_t *expect[] = {keep};
        TASK_CHECK(query_matches(f.tree, "keep", expect, 1));
    }
    TASK_CHECK(query_empty(f.tree, "drop", 4));

    return fixture_done(&f);
}

static int case_mutations_move_order(void)
{
    struct fixture f;
    const char *html =
        "<html><body>"
        "<div id='box-a'><article id='same' data-row='a'>A</article></div>"
        "<div id='box-b'><article id='same' data-row='b'>B</article></div>"
        "</body></html>";
    myhtml_tree_node_t *box_a, *box_b, *a, *b;

    if(fixture_init(&f, html, strlen(html)))
        return 1;

    box_a = node_by_attr(f.tree, "id", "box-a");
    box_b = node_by_attr(f.tree, "id", "box-b");
    a     = node_by_attr(f.tree, "data-row", "a");
    b     = node_by_attr(f.tree, "data-row", "b");
    TASK_CHECK(box_a && box_b && a && b);

    TASK_CHECK(myhtml_id_index_enable(f.tree) == MyHTML_STATUS_OK);
    {
        myhtml_tree_node_t *expect[] = {a, b};
        TASK_CHECK(query_matches(f.tree, "same", expect, 2));
    }

    /* move box-a after box-b: order becomes b, a; pointers unchanged */
    myhtml_node_remove(box_a);
    myhtml_node_insert_after(box_b, box_a);

    {
        myhtml_tree_node_t *expect[] = {b, a};
        TASK_CHECK(query_matches(f.tree, "same", expect, 2));
    }

    /* insert box-b before box-a: pre-order is b, a again */
    myhtml_node_remove(box_b);
    myhtml_node_insert_before(box_a, box_b);

    {
        myhtml_tree_node_t *expect[] = {b, a};
        TASK_CHECK(query_matches(f.tree, "same", expect, 2));
    }

    /* nest box-b inside box-a: a ancestors b, so a comes first */
    myhtml_node_remove(box_b);
    myhtml_node_append_child(box_a, box_b);

    {
        myhtml_tree_node_t *expect[] = {a, b};
        TASK_CHECK(query_matches(f.tree, "same", expect, 2));
    }

    return fixture_done(&f);
}

static int case_mutations_detach_with_descendants(void)
{
    struct fixture f;
    const char *html =
        "<html><body>"
        "<div id='outer'><span id='middle'><b id='leaf-node'>x</b></span></div>"
        "<div id='other'>o</div>"
        "</body></html>";
    myhtml_tree_node_t *outer, *middle, *leaf_node, *other;

    if(fixture_init(&f, html, strlen(html)))
        return 1;

    outer     = node_by_attr(f.tree, "id", "outer");
    middle    = node_by_attr(f.tree, "id", "middle");
    leaf_node = node_by_attr(f.tree, "id", "leaf-node");
    other     = node_by_attr(f.tree, "id", "other");
    TASK_CHECK(outer && middle && leaf_node && other);

    TASK_CHECK(myhtml_id_index_enable(f.tree) == MyHTML_STATUS_OK);

    /* detaching the parent takes the whole subtree out */
    myhtml_node_remove(outer);
    TASK_CHECK(query_empty(f.tree, "outer", 5));
    TASK_CHECK(query_empty(f.tree, "middle", 6));
    TASK_CHECK(query_empty(f.tree, "leaf-node", 9));
    {
        myhtml_tree_node_t *expect[] = {other};
        TASK_CHECK(query_matches(f.tree, "other", expect, 1));
    }

    /* putting it back brings the subtree back with the same pointers */
    myhtml_node_append_child(myhtml_tree_get_node_html(f.tree), outer);
    {
        myhtml_tree_node_t *expect[] = {outer};
        TASK_CHECK(query_matches(f.tree, "outer", expect, 1));
    }
    {
        myhtml_tree_node_t *expect[] = {middle};
        TASK_CHECK(query_matches(f.tree, "middle", expect, 1));
    }
    {
        myhtml_tree_node_t *expect[] = {leaf_node};
        TASK_CHECK(query_matches(f.tree, "leaf-node", expect, 1));
    }

    return fixture_done(&f);
}

static int case_mutations_clone_deep(void)
{
    struct fixture f;
    const char *html =
        "<html><body><div id='src'><span id='src-child'>c</span></div></body></html>";
    myhtml_tree_node_t *src, *src_child, *clone, *clone_child;
    myhtml_collection_t *c;

    if(fixture_init(&f, html, strlen(html)))
        return 1;

    src       = node_by_attr(f.tree, "id", "src");
    src_child = node_by_attr(f.tree, "id", "src-child");
    TASK_CHECK(src && src_child);

    TASK_CHECK(myhtml_id_index_enable(f.tree) == MyHTML_STATUS_OK);

    clone = myhtml_node_clone_deep(f.tree, src);
    TASK_CHECK(clone != NULL && clone != src);

    /* a clone that is not attached yet is not indexed */
    c = myhtml_get_nodes_by_id(f.tree, NULL, "src", 3, NULL);
    TASK_CHECK(c && c->length == 1 && c->list[0] == src);
    myhtml_collection_destroy(c);

    /* attaching the clone makes both ids visible in document order */
    myhtml_node_append_child(myhtml_tree_get_node_html(f.tree), clone);

    c = myhtml_get_nodes_by_id(f.tree, NULL, "src", 3, NULL);
    TASK_CHECK(c && c->length == 2 && c->list[0] == src && c->list[1] == clone);
    myhtml_collection_destroy(c);

    clone_child = myhtml_node_child(clone);
    TASK_CHECK(clone_child != NULL && clone_child != src_child);

    c = myhtml_get_nodes_by_id(f.tree, NULL, "src-child", 9, NULL);
    TASK_CHECK(c && c->length == 2 && c->list[0] == src_child && c->list[1] == clone_child);
    myhtml_collection_destroy(c);

    return fixture_done(&f);
}

static int case_mutations_leaf_delete(void)
{
    struct fixture f;
    const char *html =
        "<html><body><div id='parent'><i id='doomed'>x</i></div><div id='stay'>s</div></body></html>";
    myhtml_tree_node_t *doomed, *parent, *stay;

    if(fixture_init(&f, html, strlen(html)))
        return 1;

    doomed = node_by_attr(f.tree, "id", "doomed");
    parent = node_by_attr(f.tree, "id", "parent");
    stay   = node_by_attr(f.tree, "id", "stay");
    TASK_CHECK(doomed && parent && stay);

    TASK_CHECK(myhtml_id_index_enable(f.tree) == MyHTML_STATUS_OK);

    myhtml_node_delete(doomed);

    TASK_CHECK(query_empty(f.tree, "doomed", 6));
    {
        myhtml_tree_node_t *expect[] = {parent};
        TASK_CHECK(query_matches(f.tree, "parent", expect, 1));
    }
    {
        myhtml_tree_node_t *expect[] = {stay};
        TASK_CHECK(query_matches(f.tree, "stay", expect, 1));
    }

    return fixture_done(&f);
}

struct callback_probe {
    unsigned inserted;
    unsigned removed;
};

static void probe_insert(myhtml_tree_t *tree, myhtml_tree_node_t *node, void *ctx)
{
    struct callback_probe *probe = (struct callback_probe*)ctx;

    (void)tree;
    (void)node;
    probe->inserted++;
}

static void probe_remove(myhtml_tree_t *tree, myhtml_tree_node_t *node, void *ctx)
{
    struct callback_probe *probe = (struct callback_probe*)ctx;

    (void)tree;
    (void)node;
    probe->removed++;
}

static int case_mutations_callbacks_preserved(void)
{
    struct fixture f;
    const char *html =
        "<html><body><div id='a'>a</div><div id='b'>b</div></body></html>";
    struct callback_probe probe = {0, 0};
    myhtml_tree_node_t *a, *b;

    if(fixture_init(&f, html, strlen(html)))
        return 1;

    a = node_by_attr(f.tree, "id", "a");
    b = node_by_attr(f.tree, "id", "b");
    TASK_CHECK(a && b);

    TASK_CHECK(myhtml_id_index_enable(f.tree) == MyHTML_STATUS_OK);

    myhtml_callback_tree_node_insert_set(f.tree, probe_insert, &probe);
    myhtml_callback_tree_node_remove_set(f.tree, probe_remove, &probe);

    myhtml_node_remove(a);
    TASK_CHECK(probe.removed == 1);

    myhtml_node_append_child(myhtml_tree_get_node_html(f.tree), a);
    TASK_CHECK(probe.inserted == 1);

    /* the index follows the same moves and the callbacks keep firing */
    myhtml_node_remove(b);
    myhtml_node_insert_after(a, b);
    TASK_CHECK(probe.removed == 2 && probe.inserted == 2);

    {
        myhtml_tree_node_t *expect[] = {a, b};
        TASK_CHECK(query_matches(f.tree, "a", expect, 1));
    }
    {
        myhtml_tree_node_t *expect[] = {b};
        TASK_CHECK(query_matches(f.tree, "b", expect, 1));
    }

    return fixture_done(&f);
}

/* ------------------------------------------------------------------ */

/*
 * LIFETIME: collection reuse, clean/reparse, destroy, tree isolation.
 */

static int case_lifetime_collection_reuse(void)
{
    struct fixture f;
    const char *html = "<html><body><div id='x'>1</div><div id='y'>2</div></body></html>";
    myhtml_tree_node_t *x, *y;
    mystatus_t status = MyHTML_STATUS_ERROR;
    myhtml_collection_t *c;
    myhtml_collection_t *same;

    if(fixture_init(&f, html, strlen(html)))
        return 1;

    x = node_by_attr(f.tree, "id", "x");
    y = node_by_attr(f.tree, "id", "y");
    TASK_CHECK(x && y);

    TASK_CHECK(myhtml_id_index_enable(f.tree) == MyHTML_STATUS_OK);

    c = myhtml_get_nodes_by_id(f.tree, NULL, "x", 1, &status);
    TASK_CHECK(c && c->length == 1 && c->list[0] == x && status == MyHTML_STATUS_OK);

    /* reusing a caller collection resets the old length but keeps ownership */
    same = myhtml_get_nodes_by_id(f.tree, c, "y", 1, &status);
    TASK_CHECK(same == c && c->length == 1 && c->list[0] == y && status == MyHTML_STATUS_OK);

    same = myhtml_get_nodes_by_id(f.tree, c, "missing", 7, &status);
    TASK_CHECK(same == c && c->length == 0 && status == MyHTML_STATUS_OK);

    /* the index does not borrow the caller's value buffer */
    {
        char value[2] = {'x', '\0'};
        myhtml_collection_t *tmp = myhtml_get_nodes_by_id(f.tree, NULL, value, 1, &status);
        TASK_CHECK(tmp && tmp->length == 1 && tmp->list[0] == x);

        value[0] = 'y';
        TASK_CHECK(tmp->length == 1 && tmp->list[0] == x);
        myhtml_collection_destroy(tmp);
    }

    myhtml_collection_destroy(c);

    return fixture_done(&f);
}

static int case_lifetime_clean_and_enable_again(void)
{
    struct fixture f;
    const char *first  = "<html><body><div id='alpha'>a</div></body></html>";
    const char *second = "<html><body><div id='beta'>b</div></body></html>";
    myhtml_tree_node_t *alpha, *beta;
    mystatus_t status;

    if(fixture_init(&f, first, strlen(first)))
        return 1;

    alpha = node_by_attr(f.tree, "id", "alpha");
    TASK_CHECK(alpha != NULL);

    TASK_CHECK(myhtml_id_index_enable(f.tree) == MyHTML_STATUS_OK);
    {
        myhtml_tree_node_t *expect[] = {alpha};
        TASK_CHECK(query_matches(f.tree, "alpha", expect, 1));
    }

    /* clean drops the index; the tree is unenabled again */
    myhtml_tree_clean(f.tree);

    status = MyHTML_STATUS_OK;
    TASK_CHECK(myhtml_get_nodes_by_id(f.tree, NULL, "alpha", 5, &status) == NULL);
    TASK_CHECK(status == MyHTML_STATUS_ERROR);

    /* parsing again after a clean also leaves us unenabled */
    TASK_CHECK(myhtml_parse(f.tree, MyENCODING_UTF_8, second, strlen(second)) == MyHTML_STATUS_OK);

    status = MyHTML_STATUS_OK;
    TASK_CHECK(myhtml_get_nodes_by_id(f.tree, NULL, "beta", 4, &status) == NULL);
    TASK_CHECK(status == MyHTML_STATUS_ERROR);

    beta = node_by_attr(f.tree, "id", "beta");
    TASK_CHECK(beta != NULL);

    TASK_CHECK(myhtml_id_index_enable(f.tree) == MyHTML_STATUS_OK);
    {
        myhtml_tree_node_t *expect[] = {beta};
        TASK_CHECK(query_matches(f.tree, "beta", expect, 1));
    }
    TASK_CHECK(query_empty(f.tree, "alpha", 5));

    return fixture_done(&f);
}

static int case_lifetime_parse_twice_without_clean(void)
{
    struct fixture f;
    const char *first  = "<html><body><div id='one'>1</div></body></html>";
    const char *second = "<html><body><div id='two'>2</div><p id='two'>p</p></body></html>";
    myhtml_tree_node_t *two_a, *two_b;
    myhtml_collection_t *c;
    mystatus_t status;

    if(fixture_init(&f, first, strlen(first)))
        return 1;

    TASK_CHECK(myhtml_id_index_enable(f.tree) == MyHTML_STATUS_OK);

    /* a fresh parse implicitly cleans: the index must be dropped too */
    TASK_CHECK(myhtml_parse(f.tree, MyENCODING_UTF_8, second, strlen(second)) == MyHTML_STATUS_OK);

    status = MyHTML_STATUS_OK;
    TASK_CHECK(myhtml_get_nodes_by_id(f.tree, NULL, "two", 3, &status) == NULL);
    TASK_CHECK(status == MyHTML_STATUS_ERROR);

    c = myhtml_get_nodes_by_attribute_value(f.tree, NULL, NULL, false,
                                            "id", 2, "two", 3, NULL);
    TASK_CHECK(c && c->length == 2);
    two_a = c->list[0];
    two_b = c->list[1];
    myhtml_collection_destroy(c);

    TASK_CHECK(myhtml_id_index_enable(f.tree) == MyHTML_STATUS_OK);
    {
        myhtml_tree_node_t *expect[] = {two_a, two_b};
        TASK_CHECK(query_matches(f.tree, "two", expect, 2));
    }

    return fixture_done(&f);
}

static int case_lifetime_two_trees_isolated(void)
{
    struct fixture one, two;
    const char *html_one = "<html><body><div id='shared'>first tree</div></body></html>";
    const char *html_two = "<html><body><div id='shared'>second tree</div>"
                           "<div id='only-two'>2</div></body></html>";
    myhtml_tree_node_t *n1, *n2;
    mystatus_t status;

    if(fixture_init(&one, html_one, strlen(html_one)))
        return 1;

    n1 = node_by_attr(one.tree, "id", "shared");
    TASK_CHECK(n1 != NULL);
    TASK_CHECK(myhtml_id_index_enable(one.tree) == MyHTML_STATUS_OK);

    /* the second tree is independent and starts unenabled */
    two.myhtml = myhtml_create();
    TASK_CHECK(two.myhtml != NULL);
    TASK_CHECK(myhtml_init(two.myhtml, MyHTML_OPTIONS_PARSE_MODE_SINGLE, 1, 0) == MyHTML_STATUS_OK);
    two.tree = myhtml_tree_create();
    TASK_CHECK(two.tree != NULL);
    TASK_CHECK(myhtml_tree_init(two.tree, two.myhtml) == MyHTML_STATUS_OK);
    TASK_CHECK(myhtml_parse(two.tree, MyENCODING_UTF_8, html_two, strlen(html_two)) == MyHTML_STATUS_OK);

    status = MyHTML_STATUS_OK;
    TASK_CHECK(myhtml_get_nodes_by_id(two.tree, NULL, "shared", 6, &status) == NULL);
    TASK_CHECK(status == MyHTML_STATUS_ERROR);

    n2 = node_by_attr(two.tree, "id", "shared");
    TASK_CHECK(n2 != NULL && n2 != n1);

    TASK_CHECK(myhtml_id_index_enable(two.tree) == MyHTML_STATUS_OK);

    {
        myhtml_tree_node_t *expect[] = {n1};
        TASK_CHECK(query_matches(one.tree, "shared", expect, 1));
    }
    {
        myhtml_tree_node_t *expect[] = {n2};
        TASK_CHECK(query_matches(two.tree, "shared", expect, 1));
    }

    /* each value is only registered in its own tree */
    TASK_CHECK(query_empty(one.tree, "only-two", 8));

    /* disabling one tree leaves the other working */
    myhtml_id_index_disable(one.tree);
    status = MyHTML_STATUS_OK;
    TASK_CHECK(myhtml_get_nodes_by_id(one.tree, NULL, "shared", 6, &status) == NULL);
    TASK_CHECK(status == MyHTML_STATUS_ERROR);
    {
        myhtml_tree_node_t *expect[] = {n2};
        TASK_CHECK(query_matches(two.tree, "shared", expect, 1));
    }

    /* destroy of an enabled tree is clean */
    fixture_done(&two);

    return fixture_done(&one);
}

static int case_lifetime_destroy_enabled(void)
{
    struct fixture f;
    const char *html = "<html><body><div id='a'><div id='b'>x</div></div></body></html>";

    if(fixture_init(&f, html, strlen(html)))
        return 1;

    TASK_CHECK(myhtml_id_index_enable(f.tree) == MyHTML_STATUS_OK);

    /* destroying with a live index must not leak or touch freed members */
    myhtml_tree_destroy(f.tree);
    myhtml_destroy(f.myhtml);

    return 0;
}

/* ------------------------------------------------------------------ */

const struct task_case task_cases[] = {
    /* BUCKETS */
    {"BUCKETS",  "dup_order",           case_buckets_dup_order},
    {"BUCKETS",  "entities_and_case",   case_buckets_entities_and_case},
    {"BUCKETS",  "html_root",           case_buckets_html_root},
    {"BUCKETS",  "without_enable",      case_buckets_without_enable},

    /* MUTATIONS */
    {"MUTATIONS","duplicate_promotion", case_mutations_duplicate_promotion},
    {"MUTATIONS","attribute_delete",    case_mutations_attribute_delete},
    {"MUTATIONS","move_order",          case_mutations_move_order},
    {"MUTATIONS","detach_subtree",      case_mutations_detach_with_descendants},
    {"MUTATIONS","clone_deep",          case_mutations_clone_deep},
    {"MUTATIONS","leaf_delete",         case_mutations_leaf_delete},
    {"MUTATIONS","callbacks_preserved", case_mutations_callbacks_preserved},

    /* LIFETIME */
    {"LIFETIME", "collection_reuse",    case_lifetime_collection_reuse},
    {"LIFETIME", "clean_and_enable",    case_lifetime_clean_and_enable_again},
    {"LIFETIME", "parse_twice",         case_lifetime_parse_twice_without_clean},
    {"LIFETIME", "two_trees",           case_lifetime_two_trees_isolated},
    {"LIFETIME", "destroy_enabled",     case_lifetime_destroy_enabled}
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
