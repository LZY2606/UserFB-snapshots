#include "tinyspline.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
int main(void){
 tsStatus status;tsBSpline a=ts_bspline_init(),b=ts_bspline_init(),x=ts_bspline_init(),y=ts_bspline_init();
 tsDeBoorNet net=ts_deboornet_init();
 const tsReal cp[]={0,0,0,1,2,0,2,-1,1,3,1,0};
 const tsReal ka[]={0,0,0,.25,1,1,1},kb[]={0,0,0,.75,1,1,1};
 assert(ts_bspline_new(4,3,2,TS_CLAMPED,&a,&status)==TS_SUCCESS);
 assert(ts_bspline_new(4,3,2,TS_CLAMPED,&b,&status)==TS_SUCCESS);
 assert(ts_bspline_set_control_points(&a,cp,&status)==TS_SUCCESS);
 assert(ts_bspline_set_control_points(&b,cp,&status)==TS_SUCCESS);
 assert(ts_bspline_set_knots(&a,ka,&status)==TS_SUCCESS);
 assert(ts_bspline_set_knots(&b,kb,&status)==TS_SUCCESS);
 assert(ts_bspline_align(&a,&b,1e-9,&x,&y,&status)==TS_SUCCESS);
 const tsReal* kx=ts_bspline_knots_ptr(&x);const tsReal* ky=ts_bspline_knots_ptr(&y);
 assert(fabs(kx[3]-.25)<1e-12&&fabs(ky[3]-.75)<1e-12);
 assert(ts_bspline_eval(&x,.5,&net,&status)==TS_SUCCESS);
 const tsReal* p=ts_deboornet_result_ptr(&net);assert(isfinite(p[0])&&isfinite(p[1])&&isfinite(p[2]));
 printf("ALIGN degree=%zu controls=%zu interior-a=%.2f interior-b=%.2f common-basis=no\n",ts_bspline_degree(&x),ts_bspline_num_control_points(&x),(double)kx[3],(double)ky[3]);
 printf("EVAL u=0.5 point=%.9f,%.9f,%.9f production=ok\n",(double)p[0],(double)p[1],(double)p[2]);
 ts_deboornet_free(&net);ts_bspline_free(&a);ts_bspline_free(&b);ts_bspline_free(&x);ts_bspline_free(&y);
}
