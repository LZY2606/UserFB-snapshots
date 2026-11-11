#include "mjs.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void) {
  struct mjs *m=mjs_create();
  mjs_val_t root=mjs_mk_object(m); mjs_own(m,&root);
  mjs_val_t child=mjs_mk_object(m);
  assert(mjs_set(m,root,"child",5,child)==MJS_OK);
  const char text[]="a long owned string with embedded\0NUL and suffix";
  mjs_val_t str=mjs_mk_string(m,text,sizeof(text)-1,1);
  assert(mjs_set(m,child,"s",1,str)==MJS_OK);
  assert(mjs_set(m,child,"back",4,root)==MJS_OK);
  mjs_gc(m,1);
  child=mjs_get(m,root,"child",5); str=mjs_get(m,child,"s",1);
  size_t len=0;const char *p=mjs_get_string(m,&str,&len);
  assert(p && len==sizeof(text)-1 && memcmp(p,text,len)==0);
  assert(mjs_get(m,child,"back",4)==root);
  assert(mjs_disown(m,&root)==1);mjs_destroy(m);
  puts("MJS_GRAPH_GC_BASELINE_OK");
}
