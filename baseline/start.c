#include "ebur128.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
int main(void) {
  const unsigned n = 48000;
  float x[96000];
  for (unsigned i=0;i<n;i++) {
    x[2*i] = (float)(0.2*sin(6.283185307179586*997*i/n));
    x[2*i+1] = -x[2*i];
  }
  ebur128_state *a=ebur128_init(2,n,EBUR128_MODE_I);
  ebur128_state *b=ebur128_init(2,n,EBUR128_MODE_I|EBUR128_MODE_HISTOGRAM);
  assert(a && b);
  assert(!ebur128_add_frames_float(a,x,n));
  assert(!ebur128_add_frames_float(b,x,n));
  double la,lb,th;
  assert(!ebur128_loudness_global(a,&la));
  assert(!ebur128_loudness_global(b,&lb));
  assert(!ebur128_relative_threshold(a,&th));
  assert(isfinite(la) && fabs(la-lb)<0.1 && fabs(th-(la-10))<1e-7);
  ebur128_destroy(&a); ebur128_destroy(&b);
  puts("EBUR128_START_OK antiphase-source=finite exact-histogram=close");
  return 0;
}
