#include <myhtml/api.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do {if(!(x)) {fprintf(stderr,"baseline:%d: %s\n",__LINE__,#x);return 1;}} while(0)
static myhtml_collection_t *byid(myhtml_tree_t *t,const char *s) {
 return myhtml_get_nodes_by_attribute_value(t,NULL,NULL,false,"id",2,s,strlen(s),NULL);
}
static myhtml_collection_t *byattr(myhtml_tree_t *t,const char *key,const char *val) {
 return myhtml_get_nodes_by_attribute_value(t,NULL,NULL,false,key,strlen(key),val,strlen(val),NULL);
}
static int named(myhtml_tree_node_t *n,const char *s) {
 size_t len=0; const char *v=myhtml_attribute_value(myhtml_attribute_by_key(n,"data-row",8),&len);
 return v && len==strlen(s) && !memcmp(v,s,len);
}
int main(void) {
 myhtml_t *m=myhtml_create();CHECK(m);
 CHECK(myhtml_init(m,MyHTML_OPTIONS_PARSE_MODE_SINGLE,1,0)==MyHTML_STATUS_OK);
 myhtml_tree_t *t=myhtml_tree_create();CHECK(t);
 CHECK(myhtml_tree_init(t,m)==MyHTML_STATUS_OK);
 const char *html="<section data-box='a'><article id='same' data-row='a'>A</article></section><section data-box='b'><article id='same' data-row='b'>B</article></section>";
 CHECK(myhtml_parse(t,MyENCODING_UTF_8,html,strlen(html))==MyHTML_STATUS_OK);
 myhtml_collection_t *c=byid(t,"same");CHECK(c && c->length==2);
 CHECK(named(c->list[0],"a") && named(c->list[1],"b"));
 printf("PARSED duplicate_id_count=%zu order=a,b\n",c->length);
 myhtml_tree_node_t *a=c->list[0];myhtml_collection_destroy(c);
 c=byattr(t,"data-box","a");CHECK(c && c->length==1);myhtml_tree_node_t *boxa=c->list[0];myhtml_collection_destroy(c);
 c=byattr(t,"data-box","b");CHECK(c && c->length==1);myhtml_tree_node_t *boxb=c->list[0];myhtml_collection_destroy(c);
 myhtml_node_remove(boxa);myhtml_node_insert_after(boxb,boxa);
 c=byid(t,"same");CHECK(c && c->length==2 && named(c->list[0],"b") && c->list[1]==a);
 printf("MOVED current_DFS_order=b,a original_node=yes\n");myhtml_collection_destroy(c);
 myhtml_tree_attr_t *extra=myhtml_attribute_add(a,"id",2,"same",4,MyENCODING_UTF_8);CHECK(extra);
 c=byid(t,"same");CHECK(c && c->length==3 && c->list[1]==a && c->list[2]==a);
 printf("OLD_ATTRIBUTE_SCAN matches=%zu same_node_repeated=yes\n",c->length);myhtml_collection_destroy(c);
 myhtml_attribute_delete(t,a,extra);
 myhtml_node_remove(boxb);
 c=byid(t,"same");CHECK(c && c->length==1 && c->list[0]==a);
 printf("DETACHED subtree_hidden=yes remaining=a\n");myhtml_collection_destroy(c);
 myhtml_tree_destroy(t);myhtml_destroy(m);
 return 0;
}
