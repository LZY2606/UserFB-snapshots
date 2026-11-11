#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ass.h"
static ASS_Track *track(ASS_Library *l) {
 ASS_Track *t=ass_new_track(l); assert(t);
 const char *h="[Script Info]\nScriptType: v4.00+\nPlayResX: 320\nPlayResY: 240\n[Events]\nFormat: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n";
 ass_process_codec_private(t,h,(int)strlen(h));return t;
}
static void packet(ASS_Track *t,const char *p) {ass_process_chunk(t,p,(int)strlen(p),100,100);}
int main(void) {
 ASS_Library *l=ass_library_init();assert(l);
 FILE *f=fopen("harness/SourceSans3-Regular.ttf","rb");assert(f);fseek(f,0,SEEK_END);long n=ftell(f);rewind(f);
 char *font=malloc(n);assert(font && fread(font,1,n,f)==(size_t)n);fclose(f);ass_add_font(l,"SourceSans3-Regular.ttf",font,(int)n);free(font);
 ASS_Track *a=track(l);
 packet(a,"10,0,Default,,0,0,0,,good");assert(a->n_events==1);
 packet(a,"10,0,Default,,0,0,0,,duplicate");assert(a->n_events==1);
 ass_prune_events(a,201);assert(a->n_events==0);
 packet(a,"10,0,Default,,0,0,0,,after-prune");assert(a->n_events==1);
 ASS_Track *b=track(l);packet(b,"1,0,Default,,0,0,0,,seed");
 packet(b,"11");assert(b->n_events==1);
 packet(b,"11,0,Default,,0,0,0,,valid");assert(b->n_events==1);
 ASS_Track *c=track(l);packet(c,"12,0,Default,,0,0,0,,old");
 packet(c,"12,0,Default,,0,0,0,,duplicate");
 ass_set_check_readorder(c,0);ass_prune_events(c,201);assert(c->n_events==0);
 ass_set_check_readorder(c,1);packet(c,"12,0,Default,,0,0,0,,new");assert(c->n_events==0);
 ASS_Track *d=track(l);packet(d,"15,0,Default,,0,0,0,,{\\an7\\pos(10,10)\\p1}m 0 0 l 30 0 30 20 0 20");
 ASS_Renderer *r=ass_renderer_init(l);assert(r);ass_set_frame_size(r,320,240);
 ass_set_fonts(r,NULL,"Source Sans 3",ASS_FONTPROVIDER_NONE,NULL,0);
 ASS_Image *img=ass_render_frame(r,d,150,NULL);assert(img && img->w>0 && img->h>0);
 puts("SEED_ASS_VECTOR_OK image=nonempty");
 ass_renderer_done(r);ass_free_track(d);
 puts("SEED_ASS_OK normal_reuse=1 malformed_reuse=0 toggle_prune_reuse=0");
 ass_free_track(a);ass_free_track(b);ass_free_track(c);ass_library_done(l);return 0;
}
