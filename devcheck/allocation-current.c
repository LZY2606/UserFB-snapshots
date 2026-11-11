/* MIT; starting-state witness of child failure becoming partial success. */
#include "fixture.h"
#include <stdlib.h>
typedef struct { int calls,live,fail; } allocation_state;
static void *probe_malloc(size_t n,void *user) {
 allocation_state *s=(allocation_state *)user; void *p; assert(s);
 if(++s->calls==s->fail) return NULL;
 p=malloc(n); if(p) s->live++; return p;
}
static void probe_free(void *p,void *user) {
 allocation_state *s=(allocation_state *)user; assert(s); if(p) { s->live--; free(p); }
}
#define STBTT_malloc(n,u) probe_malloc(n,u)
#define STBTT_free(p,u) probe_free(p,u)
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
int main(void) {
 cf_blob font; stbtt_fontinfo info; stbtt_vertex *v=NULL; allocation_state s={0,0,3}; int n;
 cf_make_font(&font,1); memset(&info,0,sizeof(info)); info.userdata=&s;
 assert(stbtt_InitFont(&info,font.bytes,0)); n=stbtt_GetGlyphShape(&info,3,&v);
 assert(n==4 && v!=NULL && s.calls==3 && s.live==1); stbtt_FreeShape(&info,v); assert(s.live==0);
 printf("PARTIAL_OOM_OBSERVED commands=%d allocations=%d live_after_free=%d\n",n,s.calls,s.live); return 0;
}
