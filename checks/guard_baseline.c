#define PORTABLEGL_IMPLEMENTATION
#include "portablegl.h"
#include <stdio.h>
#include <stdlib.h>
#define WIDTH 128
#define HEIGHT 128
static glContext the_Context;
static int checks;
#define PGL_EXPECT(x,msg) do { ++checks; if (!(x)) { fprintf(stderr,"GUARD_FAIL %s\n",msg); exit(1); } } while (0)
#include "testing/guard_band_draw.c"
int main(void) {
 void (*tests[])(int,char**,void*)={guard_band_line_inset,guard_band_shared_edge,guard_band_far_vert,guard_band_w_negative};
 for (unsigned i=0;i<sizeof(tests)/sizeof(tests[0]);++i) {
  pix_t* pixels=NULL;
  if (!init_glContext(&the_Context,&pixels,WIDTH,HEIGHT)) return 1;
  tests[i](0,NULL,NULL);
  if(glGetError()!=GL_NO_ERROR) return 1;
  free_glContext(&the_Context);
 }
 if (checks!=11) {fprintf(stderr,"guard check count %d\n",checks);return 1;}
 printf("UPSTREAM_GUARD_OK cases=4 checks=%d\n",checks); return 0;
}
