#include <yaml.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void) {
 const unsigned char text[]="base: &b {id: 7, unit: kg}\nrow: {<<: *b, id: 9}\nloop: &c [*c]\n";
 yaml_parser_t p; yaml_document_t d;
 assert(yaml_parser_initialize(&p));
 yaml_parser_set_input_string(&p,text,sizeof(text)-1);
 assert(yaml_parser_load(&p,&d));
 yaml_node_t *root=yaml_document_get_root_node(&d);
 assert(root && root->type==YAML_MAPPING_NODE);
 yaml_node_pair_t *r=root->data.mapping.pairs.start;
 assert(root->data.mapping.pairs.top-r==3);
 int base=r[0].value,row=r[1].value,loop=r[2].value;
 yaml_node_t *n=yaml_document_get_node(&d,row);
 yaml_node_pair_t *pairs=n->data.mapping.pairs.start;
 assert(n->data.mapping.pairs.top-pairs==2);
 yaml_node_t *key=yaml_document_get_node(&d,pairs[0].key);
 assert(key->data.scalar.length==2 && !memcmp(key->data.scalar.value,"<<",2));
 assert(pairs[0].value==base);
 n=yaml_document_get_node(&d,loop);
 assert(n->data.sequence.items.start[0]==loop);
 printf("BASELINE alias-identity merge-retained ordinary-cycle OK\n");
 yaml_document_delete(&d); yaml_parser_delete(&p);
 return 0;
}
