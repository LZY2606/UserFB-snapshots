#include "ebur128.h"
#include <math.h>
#include <stdio.h>
#include <assert.h>
int main(void) {
  double x[2048*2], tp, sp;
  for(int i=0;i<2048;i++) { x[2*i]=.5*sin(i*1.4+.2); x[2*i+1]=.2*cos(i*.7); }
  ebur128_state* s=ebur128_init(2,48000,EBUR128_MODE_TRUE_PEAK|EBUR128_MODE_M);
  assert(s); assert(ebur128_add_frames_double(s,x,2048)==EBUR128_SUCCESS);
  for(unsigned c=0;c<2;c++) {
    assert(ebur128_sample_peak(s,c,&sp)==EBUR128_SUCCESS);
    assert(ebur128_true_peak(s,c,&tp)==EBUR128_SUCCESS);
    assert(isfinite(tp)&&tp>=sp&&sp>.19&&sp<.51);
  }
  ebur128_destroy(&s); assert(!s);
  puts("EBUR_BASELINE_OK channels=2 frames=2048");
}
