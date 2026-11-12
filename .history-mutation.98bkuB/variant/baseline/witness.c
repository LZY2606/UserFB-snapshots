#include <stdio.h>
#include <string.h>
#include "oniguruma.h"
#define CHECK(x) do {if(!(x)){fprintf(stderr,"deviation line=%d\n",__LINE__);return 2;}}while(0)
static int visits;
static int cb(int g,int b,int e,int level,int at,void *arg){
 (void)arg;(void)b;(void)e;(void)level; CHECK(g==0||g==1);
 CHECK(at==((visits<3 || visits==4)?ONIG_TRAVERSE_CALLBACK_AT_FIRST:ONIG_TRAVERSE_CALLBACK_AT_LAST));
 visits++;return 0;
}
int main(void){
 OnigSyntaxType syn;OnigErrorInfo ei;OnigRegex re;OnigEncoding enc=ONIG_ENCODING_ASCII;
 UChar *pat=(UChar*)"\\A(?@<p>\\((?:\\g<p>)*\\))\\z",*str=(UChar*)"(()())";
 CHECK(onig_initialize(&enc,1)==0);onig_copy_syntax(&syn,ONIG_SYNTAX_DEFAULT);
 onig_set_syntax_op2(&syn,onig_get_syntax_op2(&syn)|ONIG_SYN_OP2_ATMARK_CAPTURE_HISTORY);
 CHECK(onig_new(&re,pat,pat+strlen((char*)pat),0,enc,&syn,&ei)==0);
 OnigRegion *r=onig_region_new();CHECK(onig_search(re,str,str+6,str,str+6,r,0)==0);
 OnigCaptureTreeNode *root=onig_get_capture_tree(r);CHECK(root&&root->num_childs==1);
 OnigCaptureTreeNode *p=root->childs[0];CHECK(p->group==1&&p->beg==0&&p->end==6&&p->num_childs==2);
 CHECK(p->childs[0]->group==1&&p->childs[0]->beg==1&&p->childs[0]->end==3);
 CHECK(p->childs[1]->group==1&&p->childs[1]->beg==3&&p->childs[1]->end==5);
 CHECK(onig_capture_tree_traverse(r,ONIG_TRAVERSE_CALLBACK_AT_BOTH,cb,NULL)==0&&visits==8);
 onig_region_free(r,1);onig_free(re);onig_end();puts("BASELINE_HISTORY_NODES=4 VISITS=8");return 0;
}
