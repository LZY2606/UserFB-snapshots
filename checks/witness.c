#include <oniguruma.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void) {
  const char *p="(a.{0,2}bc|a.{0,2}de)";
  const unsigned char *s=(const unsigned char*)"aaaaaaade";
  OnigRegex reg; OnigErrorInfo err; OnigRegion *region;
  OnigEncoding enc=ONIG_ENCODING_ASCII;
  assert(onig_initialize(&enc,1)==0);
  assert(onig_new(&reg,(const OnigUChar*)p,(const OnigUChar*)p+strlen(p),ONIG_OPTION_NONE,enc,ONIG_SYNTAX_ONIGURUMA,&err)==0);
  region=onig_region_new(); assert(region);
  int pos=onig_search(reg,s,s+strlen((const char*)s),s,s+strlen((const char*)s),region,ONIG_OPTION_NONE);
  assert(pos==4 && region->beg[1]==4 && region->end[1]==9);
  printf("ONIG_ALTERNATION_CAPTURE_OK begin=%d end=%d\n",region->beg[1],region->end[1]);
  onig_region_free(region,1); onig_free(reg); onig_end();
}
