#include <yara.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned top_matches;
static int callback(YR_SCAN_CONTEXT *ctx,int msg,void *data,void *user) {
 (void)ctx;(void)user;
 if(msg==CALLBACK_MSG_RULE_MATCHING && !strcmp(((YR_RULE*)data)->identifier,"top"))++top_matches;
 return CALLBACK_CONTINUE;
}
int main(int argc, char **argv) {
 assert(yr_initialize()==ERROR_SUCCESS);YR_COMPILER *c=NULL;YR_RULES *rules=NULL;
 assert(yr_compiler_create(&c)==ERROR_SUCCESS);
 const char *src="private rule base {strings:$a=\"alpha\" condition:$a} "
 "rule top {condition:base and (for any i in (1..3):(i==2))} "
 "rule missing {strings:$b=\"z\" condition:@b[2]==0}";
 assert(yr_compiler_add_string(c,src,NULL)==0);
 assert(yr_compiler_get_rules(c,&rules)==ERROR_SUCCESS);yr_compiler_destroy(c);
 if(argc==2 && !strcmp(argv[1],"--write-legacy")) assert(yr_rules_save(rules,"baseline/legacy.yarc")==ERROR_SUCCESS);
 YR_RULES *legacy=NULL;assert(yr_rules_load("baseline/legacy.yarc",&legacy)==ERROR_SUCCESS);
 yr_rules_destroy(rules);rules=legacy;
 YR_SCANNER *s=NULL;assert(yr_scanner_create(rules,&s)==ERROR_SUCCESS);
 yr_scanner_set_callback(s,callback,NULL);
 assert(yr_scanner_scan_mem(s,(const uint8_t*)"alpha",5)==ERROR_SUCCESS);assert(top_matches==1);
 assert(yr_scanner_scan_mem(s,(const uint8_t*)"plain",5)==ERROR_SUCCESS);assert(top_matches==1);
 yr_scanner_destroy(s);yr_rules_destroy(rules);assert(yr_finalize()==ERROR_SUCCESS);
 puts("YARA_BASELINE_OK rule_reference=1 quantified=1 negative=0 legacy=loaded");return 0;
}
