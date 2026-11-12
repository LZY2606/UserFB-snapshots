#include <lexbor/html/html.h>
#include <lexbor/dom/interfaces/element.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void) {
 const char *html="<button id='go' aria-labelledby='n'>Fallback<span hidden>SECRET</span></button><span id='n'>Export</span>";
 lxb_html_document_t *d=lxb_html_document_create(); assert(d);
 assert(lxb_html_document_parse(d,(const lxb_char_t*)html,strlen(html))==LXB_STATUS_OK);
 lxb_dom_node_t *root=lxb_dom_interface_node(d);
 lxb_dom_node_t *button=lxb_dom_node_by_id(root,(const lxb_char_t*)"go",2); assert(button);
 size_t n=0; lxb_char_t *t=lxb_dom_node_text_content(button,&n);
 assert(n==14 && memcmp(t,"FallbackSECRET",n)==0);
 lxb_dom_node_t *label=lxb_dom_node_by_id(root,(const lxb_char_t*)"n",1); assert(label);
 t=lxb_dom_node_text_content(label,&n); assert(n==6 && memcmp(t,"Export",n)==0);
 assert(lxb_dom_element_set_attribute(lxb_dom_interface_element(label),(const lxb_char_t*)"aria-label",10,(const lxb_char_t*)"Save",4));
 puts("BASELINE_DOM_TEXT=FallbackSECRET LABEL_TEXT=Export MUTATION_OK");
 lxb_html_document_destroy(d); return 0;
}
