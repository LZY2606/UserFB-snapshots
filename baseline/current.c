#include "rpmalloc.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
static void *remote;
static void *worker(void *unused) {
 (void)unused; rpmalloc_thread_initialize();rpfree(remote);
 rpmalloc_thread_finalize();return NULL;
}
int main(void) {
 assert(rpmalloc_initialize(NULL)==0);rpmalloc_thread_initialize();
 unsigned char *p=rpmalloc(73);assert(p);memset(p,0x52,73);
 p=rprealloc(p,4096);assert(p);for(int i=0;i<73;i++) assert(p[i]==0x52);
 assert(rpmalloc_usable_size(p)>=4096);rpfree(p);
 void *a=rpaligned_alloc(256,500);assert(a && !((uintptr_t)a%256));rpfree(a);
 remote=rpmalloc(64);assert(remote);pthread_t t;
 assert(!pthread_create(&t,NULL,worker,NULL));assert(!pthread_join(t,NULL));
 rpmalloc_thread_collect();p=rpmalloc(64);assert(p);rpfree(p);
 rpmalloc_thread_finalize();rpmalloc_finalize();
 puts("RPMALLOC_BASELINE_OK realloc=73 aligned=256 remote=joined");return 0;
}
