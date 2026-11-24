#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dmalloc.h"
#include "debug_tok.h"
int main(void) {
  char *p, *q, *origin;
  DMALLOC_SIZE size, total;
  unsigned int line;
  unsigned long mark, seen;
  unsigned flags;
  dmalloc_debug_setup("debug=0");
  flags=dmalloc_debug_current();
  p=malloc(96); if(!p) return 11;
  memset(p,0x42,96);
  mark=dmalloc_mark();
  if(!dmalloc_examine(p,&size,&total,&origin,&line,0,0,&seen)||size!=96||total<size||!origin||!line||!seen) return 12;
  q=realloc(p,80); if(q!=p) return 13;
  if(!dmalloc_examine(q,&size,0,0,0,0,0,0)||size!=80) return 14;
  dmalloc_debug(flags|DMALLOC_DEBUG_REALLOC_COPY);
  p=realloc(q,160); if(!p||p==q) return 15;
  for(size=0;size<80;size++) if(p[size]!=0x42) return 16;
  dmalloc_debug(flags);
  if(dmalloc_mark()<=mark||dmalloc_verify(p)!=DMALLOC_VERIFY_NOERROR||dmalloc_verify(NULL)!=DMALLOC_VERIFY_NOERROR) return 17;
  free(p);
  if(dmalloc_verify(NULL)!=DMALLOC_VERIFY_NOERROR) return 18;
  puts("NATIVE_HEAP_COMPLETE allocation=96 in_place=80 moved=160 verify=healthy");
  return 0;
}
