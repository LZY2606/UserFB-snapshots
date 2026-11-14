#include "fuzzy.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static void streamed(const unsigned char *data, size_t n, char *result, unsigned flags) {
  struct fuzzy_state *st=fuzzy_new(); size_t i=0; assert(st);
  while(i<n) { size_t step=(i%23)+1; if(step>n-i)step=n-i; assert(fuzzy_update(st,data+i,step)==0); i+=step; }
  assert(fuzzy_digest(st,result,flags)==0); fuzzy_free(st);
}
int main(void) {
  unsigned char data[65536], zero[65536]={0}; uint32_t x=7; size_t i;
  char a[FUZZY_MAX_RESULT],b[FUZZY_MAX_RESULT],c[FUZZY_MAX_RESULT];
  struct fuzzy_state *st,*clone; unsigned flags;
  for(i=0;i<sizeof(data);i++) {x=x*1664525u+1013904223u; data[i]=(unsigned char)(x>>24);}
  assert(fuzzy_hash_buf(data,sizeof(data),a)==0); streamed(data,sizeof(data),b,0);
  assert(strcmp(a,b)==0); printf("unmodified=%s\n",a);
  for(flags=0;flags<4;flags++) {
    st=fuzzy_new(); assert(st); assert(fuzzy_update(st,data,sizeof(data))==0);
    assert(fuzzy_digest(st,a,flags)==0); streamed(data,sizeof(data),b,flags);
    assert(strcmp(a,b)==0); assert(fuzzy_digest(st,c,flags)==0 && strcmp(a,c)==0);
    printf("flags=%u digest=%s\n",flags,a); fuzzy_free(st);
  }
  st=fuzzy_new(); assert(st); assert(fuzzy_update(st,data,12345)==0); clone=fuzzy_clone(st); assert(clone);
  assert(fuzzy_update(st,data+12345,sizeof(data)-12345)==0);
  assert(fuzzy_digest(st,a,0)==0);
  data[12345]^=0x5a;
  assert(fuzzy_update(clone,data+12345,sizeof(data)-12345)==0); assert(fuzzy_digest(clone,b,0)==0);
  streamed(data,sizeof(data),c,0); assert(strcmp(b,c)==0); assert(strcmp(a,b)!=0);
  fuzzy_free(st);fuzzy_free(clone);
  puts("SSDEEP_BASELINE_STREAM_AND_CLONE_OK flags=4");
  for(flags=0;flags<4;flags++){streamed(zero,sizeof(zero),a,flags); assert(strcmp(a,"3::")==0);}
  puts("SSDEEP_BASELINE_ZERO_OK length=65536 digest=3::");
  streamed(data,768,a,0); streamed(data,768,b,FUZZY_FLAG_NOTRUNC);
  assert(strcmp(a,b)!=0); printf("notrunc_default=%s notrunc_full=%s\n",a,b);
  memset(data,4,sizeof(data)); streamed(data,sizeof(data),a,0); streamed(data,sizeof(data),b,FUZZY_FLAG_ELIMSEQ);
  assert(strcmp(a,b)!=0); printf("repeated_default=%s repeated_elim=%s\n",a,b);
  puts("SSDEEP_BASELINE_FLAGS_DISTINCT_OK");
  puts("SSDEEP_BASELINE_OK target=editable-summary-not-provided");
  return 0;
}
