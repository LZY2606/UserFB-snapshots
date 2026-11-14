#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <lexbor/html/html.h>
#include <lexbor/dom/dom.h>
static unsigned structures, attributes;
static lxb_status_t inserted(lxb_dom_node_t *n) {(void)n; structures++; return LXB_STATUS_OK;}
static lxb_status_t removed(lxb_dom_node_t *n,lxb_dom_node_t *p) {(void)n;(void)p;structures++;return LXB_STATUS_OK;}
static lxb_status_t changed(lxb_dom_element_t *e,lxb_dom_attr_id_t n,const lxb_char_t *o,size_t z,const lxb_char_t *v,size_t l,lxb_ns_id_t s) {
    (void)e;(void)n;(void)o;(void)z;(void)v;(void)l;(void)s;attributes++;return LXB_STATUS_OK;
}
int main(void) {
    const char *html="<main><p data-key='old'>entry</p></main>";
    lxb_html_document_t *doc=lxb_html_document_create(); assert(doc);
    assert(lxb_html_document_parse(doc,(const lxb_char_t*)html,strlen(html))==LXB_STATUS_OK);
    lxb_dom_document_t *d=lxb_dom_interface_document(doc);
    lxb_dom_node_t *main=lxb_dom_interface_node(lxb_html_document_body_element(doc))->first_child;
    lxb_dom_node_t *p=main->first_child; lxb_dom_node_t *text=p->first_child;
    lxb_dom_document_mutation_cb_t nodes={.inserted=inserted,.removed=removed};
    lxb_dom_document_attr_mutation_cb_t attrs={.change=changed};
    d->mutation=&nodes; d->attr_mutation=&attrs;
    assert(lxb_dom_element_set_attribute(lxb_dom_interface_element(p),(const lxb_char_t*)"data-key",8,(const lxb_char_t*)"new",3));
    assert(lxb_dom_character_data_replace(lxb_dom_interface_character_data(text),(const lxb_char_t*)"changed",7,0,5)==LXB_STATUS_OK);
    lxb_dom_node_remove(p); lxb_dom_node_insert_child(main,p);
    assert(attributes==1 && structures==3); /* insertion callbacks visit p and its text */
    assert(lxb_dom_interface_character_data(text)->data.length==7);
    printf("BASELINE_OK text=%s attrs=%u structure=%u\n",lxb_dom_interface_character_data(text)->data.data,attributes,structures);
    size_t key_len = 0, plain_len = 0;
    const lxb_char_t *key = lxb_dom_element_get_attribute(lxb_dom_interface_element(p),
                                      (const lxb_char_t *) "data-key", 8, &key_len);
    lxb_char_t *plain = lxb_dom_node_text_content(p, &plain_len);
    assert(key && key_len == 3 && !memcmp(key, "new", 3));
    assert(plain && plain_len == 7 && !memcmp(plain, "changed", 7));
    printf("COLLECTION_PRIMITIVES key=%.*s text=%.*s\n",
           (int) key_len, key, (int) plain_len, plain);
    lxb_dom_document_destroy_text(d, plain);
    d->mutation=NULL; d->attr_mutation=NULL;
    lxb_dom_document_mutation_init(d);
    lxb_html_document_destroy(doc);
    return 0;
}
