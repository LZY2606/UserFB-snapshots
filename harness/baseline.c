#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "gumbo.h"
static unsigned seen;
static void walk(GumboNode *n){
 if(n->type==GUMBO_NODE_TEXT){
  if(strstr(n->v.text.text,"A&B")){assert(n->v.text.original_text.length>=7);++seen;}
  return;
 }
 if(n->type!=GUMBO_NODE_ELEMENT && n->type!=GUMBO_NODE_DOCUMENT && n->type!=GUMBO_NODE_TEMPLATE)return;
 GumboVector *v=n->type==GUMBO_NODE_DOCUMENT?&n->v.document.children:&n->v.element.children;
 for(unsigned i=0;i<v->length;i++)walk(v->data[i]);
}
int main(void){
 const char *html="<p>A&amp;B <b>x</b></p><table>outside<tr><td>cell</td></tr></table>";
 GumboOutput *o=gumbo_parse(html);assert(o&&o->root);walk(o->root);assert(seen==1);
 gumbo_destroy_output(&kGumboDefaultOptions,o);puts("GUMBO_BASELINE_OK entity-and-tree");
}
