#include <oniguruma.h>
#include <stdio.h>
#include <string.h>
#define REQUIRE(x) do { if(!(x)) {fprintf(stderr,"BASE_FAIL line=%d %s\n",__LINE__,#x); return 1;} } while(0)
int main(void) {
  OnigEncoding enc=ONIG_ENCODING_ASCII;
  OnigRegex reg; OnigErrorInfo err;
  const char *p="(?<item>a|aa)+\\z";
  const OnigUChar *s=(const OnigUChar*)"aaaa";
  REQUIRE(onig_initialize(&enc,1)==ONIG_NORMAL);
  REQUIRE(onig_new(&reg,(const OnigUChar*)p,(const OnigUChar*)p+strlen(p),0,enc,ONIG_SYNTAX_ONIGURUMA,&err)==ONIG_NORMAL);
  OnigMatchParam *mp=onig_new_match_param(); OnigRegion *region=onig_region_new();
  REQUIRE(mp && region);
  REQUIRE(onig_set_retry_limit_in_match_of_match_param(mp,10000)==ONIG_NORMAL);
  REQUIRE(onig_set_retry_limit_in_search_of_match_param(mp,10000)==ONIG_NORMAL);
  REQUIRE(onig_match_with_param(reg,s,s+4,s,region,0,mp)==4);
  REQUIRE(region->num_regs==2 && region->beg[0]==0 && region->end[0]==4);
  REQUIRE(region->beg[1]==3 && region->end[1]==4);
  REQUIRE(onig_search_with_param(reg,s,s+4,s,s+4,region,0,mp)==0);
  REQUIRE(region->beg[0]==0 && region->end[0]==4 && region->beg[1]==3 && region->end[1]==4);
  puts("ONIG_BUDGET_BASELINE_OK match=4 search=0 capture=3:4");
  onig_free_match_param(mp); onig_region_free(region,1); onig_free(reg); onig_end();
  return 0;
}
