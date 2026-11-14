#include "gc/gc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#ifdef GC_THREADS
#include "gc/gc_pthread_redirects.h"
static void *worker(void *arg) {
  size_t i; void **slots=(void **)arg;
  for(i=0;i<64;i++){ slots[i]=GC_malloc_atomic(48); if(!slots[i])abort(); memset(slots[i],0x6b,48); }
  return NULL;
}
#endif
static void check(int ok) { if(!ok) abort(); }
int main(void) {
  unsigned char *p,*q; uintptr_t old; size_t i;
  GC_INIT();
  p=(unsigned char *)GC_malloc(48); check(p!=NULL); memset(p,0x39,48);
  q=(unsigned char *)GC_realloc(p,49); check(q==p);
  q=(unsigned char *)GC_realloc(q,32768); check(q!=NULL && q!=p);
  for(i=0;i<48;i++) check(q[i]==0x39);
  GC_free(q);
  p=(unsigned char *)GC_malloc_uncollectable(48); check(p!=NULL);
  old=(uintptr_t)p; GC_free(p);
  p=(unsigned char *)GC_malloc_uncollectable(48); check((uintptr_t)p==old);
  check(GC_base(p+7)==p); GC_free(p);
#ifdef GC_THREADS
  { pthread_t t; void *slots[64]; check(pthread_create(&t,NULL,worker,slots)==0);
    check(pthread_join(t,NULL)==0);
    for(i=0;i<64;i++){ check(((unsigned char *)slots[i])[47]==0x6b); GC_free(slots[i]); }
  }
  puts("BDWGC_BASELINE_OK mode=tls reuse=1 realloc=move remote_free=64");
#else
  puts("BDWGC_BASELINE_OK mode=serial reuse=1 realloc=move");
#endif
  GC_gcollect();
  return 0;
}
