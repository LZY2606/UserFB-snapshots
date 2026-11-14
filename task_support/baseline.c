#include "gumbo.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int live;
static void *allocate(void *ctx,size_t n){(void)ctx;void *p=malloc(n);if(p)++live;return p;}
static void release(void *ctx,void *p){(void)ctx;if(p){--live;free(p);}}
static int elements(GumboNode *n,GumboTag tag){
  int count=0;GumboVector *children=NULL;
  if(n->type==GUMBO_NODE_DOCUMENT)children=&n->v.document.children;
  else if(n->type==GUMBO_NODE_ELEMENT||n->type==GUMBO_NODE_TEMPLATE){
    count=n->v.element.tag==tag;children=&n->v.element.children;
  }
  if(children)for(unsigned i=0;i<children->length;i++)count+=elements(children->data[i],tag);
  return count;
}
static GumboNode *find_script(GumboNode *n){
  GumboVector *v=NULL;
  if(n->type==GUMBO_NODE_DOCUMENT)v=&n->v.document.children;
  else if(n->type==GUMBO_NODE_ELEMENT||n->type==GUMBO_NODE_TEMPLATE){
    if(n->v.element.tag==GUMBO_TAG_SCRIPT)return n;v=&n->v.element.children;
  }
  if(v)for(unsigned i=0;i<v->length;i++){GumboNode *p=find_script(v->data[i]);if(p)return p;}
  return NULL;
}
int main(void){
  const char input[]="<!doctype html><body><script data-key=outer>write_known_record()</script><p id=tail>tail</p>";
  GumboOptions options=kGumboDefaultOptions;options.allocator=allocate;options.deallocator=release;
  GumboOutput *out=gumbo_parse_with_options(&options,input,sizeof(input)-1);
  GumboNode *script=find_script(out->document);
  if(!script||elements(out->document,GUMBO_TAG_P)!=1||elements(out->document,GUMBO_TAG_SCRIPT)!=1)return 1;
  GumboText *text=&((GumboNode*)script->v.element.children.data[0])->v.text;
  if(strcmp(text->text,"write_known_record()")||script->v.element.original_tag.data!=strstr(input,"<script"))return 2;
  printf("BATCH_OK scripts=1 tail_paragraphs=1 generated_records=0 borrowed_source=1\n");
  gumbo_destroy_output(&options,out);if(live)return 3;
  const char nested[]="<!doctype html><script><!--<script></script><p>ghost</p>--></script><p>real</p>";
  out=gumbo_parse_with_options(&options,nested,sizeof(nested)-1);
  if(elements(out->document,GUMBO_TAG_P)!=1)return 4;
  printf("SCRIPT_LEX_OK real_paragraphs=1 ghost_paragraphs=0\n");
  gumbo_destroy_output(&options,out);if(live)return 5;
  printf("ALLOCATOR_OK live=0\n");return 0;
}
