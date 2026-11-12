#include "kiss_fft.h"
#include "kiss_fftr.h"
#include "kiss_fftnd.h"
#include "alloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#define PI 3.14159265358979323846264338327950288
static void check(int ok, const char *msg) {
    if (!ok) { fprintf(stderr,"BASELINE_FAIL %s\n",msg); exit(2); }
}
static void complex_case(int n, int stride, int inverse) {
    kiss_fft_cpx *in=calloc((size_t)n*stride,sizeof(*in));
    kiss_fft_cpx *out=calloc((size_t)n,sizeof(*out));
    kiss_fft_cfg cfg=kiss_fft_alloc(n,inverse,0,0);
    check(in && out && cfg,"allocate complex");
    for(int j=0;j<n;j++) {
        in[j*stride].r=(kiss_fft_scalar)(((j*17)%29-14)/17.0);
        in[j*stride].i=(kiss_fft_scalar)(((j*7)%23-11)/13.0);
    }
    task_allocs=task_frees=0;
    kiss_fft_stride(cfg,in,out,stride);
    size_t allocations=task_allocs;
    double maxerr=0;
    for(int k=0;k<n;k++) {
        double re=0,im=0;
        for(int j=0;j<n;j++) {
            double a=(inverse?2:-2)*PI*j*k/n;
            re+=in[j*stride].r*cos(a)-in[j*stride].i*sin(a);
            im+=in[j*stride].r*sin(a)+in[j*stride].i*cos(a);
        }
        double err=hypot(out[k].r-re,out[k].i-im);
        if(err>maxerr)maxerr=err;
    }
    check(maxerr < (sizeof(kiss_fft_scalar)==8?1e-9:3e-4),"direct DFT mismatch");
    if(n==257) check(allocations>0,"expected current generic scratch allocation");
    printf("COMPLEX_DFT n=%d stride=%d inverse=%d max_error=%.9g execution_allocs=%zu\n",n,stride,inverse,maxerr,allocations);
    kiss_fft_free(cfg);free(in);free(out);
}
static void real_case(void) {
    const int n=514;
    kiss_fft_scalar in[n],back[n]; kiss_fft_cpx out[n/2+1];
    kiss_fftr_cfg f=kiss_fftr_alloc(n,0,0,0),r=kiss_fftr_alloc(n,1,0,0);
    check(f && r,"allocate real");
    for(int i=0;i<n;i++)in[i]=(kiss_fft_scalar)(0.5*cos(2*PI*37*i/n)+0.125);
    task_allocs=task_frees=0;
    kiss_fftr(f,in,out);
    size_t allocs=task_allocs;
    check(fabs(out[0].r-n*.125)<.001 && fabs(out[37].r-n*.25)<.001,"real tones");
    check(fabs(out[0].i)<1e-8 && fabs(out[n/2].i)<1e-8,"real endpoints");
    kiss_fftri(r,out,back);
    double err=0;
    for(int i=0;i<n;i++){double e=fabs(back[i]/n-in[i]);if(e>err)err=e;}
    check(err<3e-6 && allocs>0,"real inverse / current scratch");
    printf("REAL_PRIME_SUBPLAN n=%d max_error=%.9g forward_execution_allocs=%zu\n",n,err,allocs);
    kiss_fftr_free(f);kiss_fftr_free(r);
}
static void nd_case(void) {
    int dims[2]={3,257};const int n=771;
    kiss_fft_cpx in[n],out[n]; memset(in,0,sizeof(in));
    in[1*257+2].r=1;
    kiss_fftnd_cfg cfg=kiss_fftnd_alloc(dims,2,0,0,0);
    check(cfg!=0,"allocate ND");
    task_allocs=task_frees=0;
    kiss_fftnd(cfg,in,out);
    size_t allocs=task_allocs;double err=0;
    for(int i=0;i<3;i++)for(int j=0;j<257;j++) {
        double a=-2*PI*(i/3.0+j*2/257.0);
        double e=hypot(out[i*257+j].r-cos(a),out[i*257+j].i-sin(a));
        if(e>err)err=e;
    }
    check(err<2e-6 && allocs>0,"ND phase / current scratch");
    printf("ND_PRIME_AXIS dims=3x257 max_error=%.9g execution_allocs=%zu\n",err,allocs);
    free(cfg);
}
int main(void) {
    complex_case(120,1,0);complex_case(257,3,0);complex_case(257,2,1);
    real_case();nd_case();
    puts("BASELINE_EXISTING_PRIME_PATHS_OK");return 0;
}
