/*
 *  Copyright (c) 2003-2010, Mark Borgerding. All rights reserved.
 *  This file is part of KISS FFT - https://github.com/mborgerding/kissfft
 *
 *  SPDX-License-Identifier: BSD-3-Clause
 *  See COPYING file for more information.
 */

#include <stdint.h>
#include "_kiss_fft_guts.h"
/* The guts header contains all the multiplication and addition macros that are defined for
 fixed or floating point complex numbers.  It also delares the kf_ internal functions.
 */

static void kf_bfly2(
        kiss_fft_cpx * Fout,
        const size_t fstride,
        const kiss_fft_cfg st,
        int m
        )
{
    kiss_fft_cpx * Fout2;
    kiss_fft_cpx * tw1 = st->twiddles;
    kiss_fft_cpx t;
    Fout2 = Fout + m;
    do{
        C_FIXDIV(*Fout,2); C_FIXDIV(*Fout2,2);

        C_MUL (t,  *Fout2 , *tw1);
        tw1 += fstride;
        C_SUB( *Fout2 ,  *Fout , t );
        C_ADDTO( *Fout ,  t );
        ++Fout2;
        ++Fout;
    }while (--m);
}

static void kf_bfly4(
        kiss_fft_cpx * Fout,
        const size_t fstride,
        const kiss_fft_cfg st,
        const size_t m
        )
{
    kiss_fft_cpx *tw1,*tw2,*tw3;
    kiss_fft_cpx scratch[6];
    size_t k=m;
    const size_t m2=2*m;
    const size_t m3=3*m;


    tw3 = tw2 = tw1 = st->twiddles;

    do {
        C_FIXDIV(*Fout,4); C_FIXDIV(Fout[m],4); C_FIXDIV(Fout[m2],4); C_FIXDIV(Fout[m3],4);

        C_MUL(scratch[0],Fout[m] , *tw1 );
        C_MUL(scratch[1],Fout[m2] , *tw2 );
        C_MUL(scratch[2],Fout[m3] , *tw3 );

        C_SUB( scratch[5] , *Fout, scratch[1] );
        C_ADDTO(*Fout, scratch[1]);
        C_ADD( scratch[3] , scratch[0] , scratch[2] );
        C_SUB( scratch[4] , scratch[0] , scratch[2] );
        C_SUB( Fout[m2], *Fout, scratch[3] );
        tw1 += fstride;
        tw2 += fstride*2;
        tw3 += fstride*3;
        C_ADDTO( *Fout , scratch[3] );

        if(st->inverse) {
            Fout[m].r = scratch[5].r - scratch[4].i;
            Fout[m].i = scratch[5].i + scratch[4].r;
            Fout[m3].r = scratch[5].r + scratch[4].i;
            Fout[m3].i = scratch[5].i - scratch[4].r;
        }else{
            Fout[m].r = scratch[5].r + scratch[4].i;
            Fout[m].i = scratch[5].i - scratch[4].r;
            Fout[m3].r = scratch[5].r - scratch[4].i;
            Fout[m3].i = scratch[5].i + scratch[4].r;
        }
        ++Fout;
    }while(--k);
}

static void kf_bfly3(
         kiss_fft_cpx * Fout,
         const size_t fstride,
         const kiss_fft_cfg st,
         size_t m
        )
{
     size_t k=m;
     const size_t m2 = 2*m;
     kiss_fft_cpx *tw1,*tw2;
     kiss_fft_cpx scratch[5];
     kiss_fft_cpx epi3;
     epi3 = st->twiddles[fstride*m];

     tw1=tw2=st->twiddles;

     do{
         C_FIXDIV(*Fout,3); C_FIXDIV(Fout[m],3); C_FIXDIV(Fout[m2],3);

         C_MUL(scratch[1],Fout[m] , *tw1);
         C_MUL(scratch[2],Fout[m2] , *tw2);

         C_ADD(scratch[3],scratch[1],scratch[2]);
         C_SUB(scratch[0],scratch[1],scratch[2]);
         tw1 += fstride;
         tw2 += fstride*2;

         Fout[m].r = Fout->r - HALF_OF(scratch[3].r);
         Fout[m].i = Fout->i - HALF_OF(scratch[3].i);

         C_MULBYSCALAR( scratch[0] , epi3.i );

         C_ADDTO(*Fout,scratch[3]);

         Fout[m2].r = Fout[m].r + scratch[0].i;
         Fout[m2].i = Fout[m].i - scratch[0].r;

         Fout[m].r -= scratch[0].i;
         Fout[m].i += scratch[0].r;

         ++Fout;
     }while(--k);
}

static void kf_bfly5(
        kiss_fft_cpx * Fout,
        const size_t fstride,
        const kiss_fft_cfg st,
        int m
        )
{
    kiss_fft_cpx *Fout0,*Fout1,*Fout2,*Fout3,*Fout4;
    int u;
    kiss_fft_cpx scratch[13];
    kiss_fft_cpx * twiddles = st->twiddles;
    kiss_fft_cpx *tw;
    kiss_fft_cpx ya,yb;
    ya = twiddles[fstride*m];
    yb = twiddles[fstride*2*m];

    Fout0=Fout;
    Fout1=Fout0+m;
    Fout2=Fout0+2*m;
    Fout3=Fout0+3*m;
    Fout4=Fout0+4*m;

    tw=st->twiddles;
    for ( u=0; u<m; ++u ) {
        C_FIXDIV( *Fout0,5); C_FIXDIV( *Fout1,5); C_FIXDIV( *Fout2,5); C_FIXDIV( *Fout3,5); C_FIXDIV( *Fout4,5);
        scratch[0] = *Fout0;

        C_MUL(scratch[1] ,*Fout1, tw[u*fstride]);
        C_MUL(scratch[2] ,*Fout2, tw[2*u*fstride]);
        C_MUL(scratch[3] ,*Fout3, tw[3*u*fstride]);
        C_MUL(scratch[4] ,*Fout4, tw[4*u*fstride]);

        C_ADD( scratch[7],scratch[1],scratch[4]);
        C_SUB( scratch[10],scratch[1],scratch[4]);
        C_ADD( scratch[8],scratch[2],scratch[3]);
        C_SUB( scratch[9],scratch[2],scratch[3]);

        Fout0->r += scratch[7].r + scratch[8].r;
        Fout0->i += scratch[7].i + scratch[8].i;

        scratch[5].r = scratch[0].r + S_MUL(scratch[7].r,ya.r) + S_MUL(scratch[8].r,yb.r);
        scratch[5].i = scratch[0].i + S_MUL(scratch[7].i,ya.r) + S_MUL(scratch[8].i,yb.r);

        scratch[6].r =  S_MUL(scratch[10].i,ya.i) + S_MUL(scratch[9].i,yb.i);
        scratch[6].i = -S_MUL(scratch[10].r,ya.i) - S_MUL(scratch[9].r,yb.i);

        C_SUB(*Fout1,scratch[5],scratch[6]);
        C_ADD(*Fout4,scratch[5],scratch[6]);

        scratch[11].r = scratch[0].r + S_MUL(scratch[7].r,yb.r) + S_MUL(scratch[8].r,ya.r);
        scratch[11].i = scratch[0].i + S_MUL(scratch[7].i,yb.r) + S_MUL(scratch[8].i,ya.r);
        scratch[12].r = - S_MUL(scratch[10].i,yb.i) + S_MUL(scratch[9].i,ya.i);
        scratch[12].i = S_MUL(scratch[10].r,yb.i) - S_MUL(scratch[9].r,ya.i);

        C_ADD(*Fout2,scratch[11],scratch[12]);
        C_SUB(*Fout3,scratch[11],scratch[12]);

        ++Fout0;++Fout1;++Fout2;++Fout3;++Fout4;
    }
}

/* diagnostic hook: generic butterfly work actually executed */
static unsigned long long kf_op_generic_calls;
static unsigned long long kf_op_generic_radix_sum;
static unsigned long long kf_op_generic_madds;

void kiss_fft_op_count_reset(void)
{
    kf_op_generic_calls = 0;
    kf_op_generic_radix_sum = 0;
    kf_op_generic_madds = 0;
}

void kiss_fft_op_count_get(kiss_fft_op_count *out)
{
    if (!out)
        return;
    out->generic_calls = kf_op_generic_calls;
    out->generic_radix_sum = kf_op_generic_radix_sum;
    out->generic_madds = kf_op_generic_madds;
}

/* perform the butterfly for one stage of a mixed radix FFT.
   scratch must point to at least p complex elements of caller supplied
   storage (no heap is touched here, except in OpenMP builds where each
   thread needs its own temporary). */
static void kf_bfly_generic(
        kiss_fft_cpx * Fout,
        const size_t fstride,
        const kiss_fft_cfg st,
        int m,
        int p,
        kiss_fft_cpx * scratch
        )
{
    int u,k,q1,q;
    kiss_fft_cpx * twiddles = st->twiddles;
    kiss_fft_cpx t;
    int Norig = st->nfft;

    if (p < 2)
        return; /* a radix-1 stage is a no-op */

#ifdef _OPENMP
    /* threads may share the plan; give each invocation its own scratch */
    scratch = (kiss_fft_cpx*)KISS_FFT_TMP_ALLOC(sizeof(kiss_fft_cpx)*p);
    if (scratch == NULL){
        KISS_FFT_ERROR("Memory allocation failed.");
        return;
    }
#endif

    kf_op_generic_calls += 1;
    kf_op_generic_radix_sum += (unsigned long long)p;
    kf_op_generic_madds += (unsigned long long)m * (unsigned long long)p * (unsigned long long)(p-1);

    for ( u=0; u<m; ++u ) {
        k=u;
        for ( q1=0 ; q1<p ; ++q1 ) {
            scratch[q1] = Fout[ k  ];
            C_FIXDIV(scratch[q1],p);
            k += m;
        }

        k=u;
        for ( q1=0 ; q1<p ; ++q1 ) {
            int twidx=0;
            Fout[ k ] = scratch[0];
            for (q=1;q<p;++q ) {
                twidx += fstride * k;
                if (twidx>=Norig) twidx-=Norig;
                C_MUL(t,scratch[q] , twiddles[twidx] );
                C_ADDTO( Fout[ k ] ,t);
            }
            k += m;
        }
    }
#ifdef _OPENMP
    KISS_FFT_TMP_FREE(scratch);
#endif
}

static
void kf_work(
        kiss_fft_cpx * Fout,
        const kiss_fft_cpx * f,
        const size_t fstride,
        int in_stride,
        int * factors,
        const kiss_fft_cfg st,
        kiss_fft_cpx * scratch
        )
{
    kiss_fft_cpx * Fout_beg=Fout;
    const int p=*factors++; /* the radix  */
    const int m=*factors++; /* stage's fft length/p */
    const kiss_fft_cpx * Fout_end = Fout + p*m;

#ifdef _OPENMP
    // use openmp extensions at the
    // top-level (not recursive)
    if (fstride==1 && p<=5 && m!=1)
    {
        int k;

        // execute the p different work units in different threads
#       pragma omp parallel for
        for (k=0;k<p;++k)
            kf_work( Fout +k*m, f+ fstride*in_stride*k,fstride*p,in_stride,factors,st,scratch);
        // all threads have joined by this point

        switch (p) {
            case 2: kf_bfly2(Fout,fstride,st,m); break;
            case 3: kf_bfly3(Fout,fstride,st,m); break;
            case 4: kf_bfly4(Fout,fstride,st,m); break;
            case 5: kf_bfly5(Fout,fstride,st,m); break;
            default: kf_bfly_generic(Fout,fstride,st,m,p,scratch); break;
        }
        return;
    }
#endif

    if (m==1) {
        do{
            *Fout = *f;
            f += fstride*in_stride;
        }while(++Fout != Fout_end );
    }else{
        do{
            // recursive call:
            // DFT of size m*p performed by doing
            // p instances of smaller DFTs of size m,
            // each one takes a decimated version of the input
            kf_work( Fout , f, fstride*p, in_stride, factors,st,scratch);
            f += fstride*in_stride;
        }while( (Fout += m) != Fout_end );
    }

    Fout=Fout_beg;

    // recombine the p smaller DFTs
    switch (p) {
        case 2: kf_bfly2(Fout,fstride,st,m); break;
        case 3: kf_bfly3(Fout,fstride,st,m); break;
        case 4: kf_bfly4(Fout,fstride,st,m); break;
        case 5: kf_bfly5(Fout,fstride,st,m); break;
        default: kf_bfly_generic(Fout,fstride,st,m,p,scratch); break;
    }
}

/*  facbuf is populated by p1,m1,p2,m2, ...
    where
    p[i] * m[i] = m[i-1]
    m0 = n                  */
static
void kf_factor(int n,int * facbuf)
{
    int p=4;
    double floor_sqrt;
    floor_sqrt = floor( sqrt((double)n) );

    /*factor out powers of 4, powers of 2, then any remaining primes */
    do {
        while (n % p) {
            switch (p) {
                case 4: p = 2; break;
                case 2: p = 3; break;
                default: p += 2; break;
            }
            if (p > floor_sqrt)
                p = n;          /* no more factors, skip to end */
        }
        n /= p;
        *facbuf++ = p;
        *facbuf++ = n;
    } while (n > 1);
}

/* largest radix that will be served by kf_bfly_generic for this factor list */
static int kf_scan_max_radix(const int * factors)
{
    int maxp = 0;
    for (;;) {
        const int p = factors[0];
        const int m = factors[1];
        if (p > 5 && p > maxp)
            maxp = p;
        if (m <= 1)
            break;
        factors += 2;
    }
    return maxp;
}

/* smallest k >= n with only 2/3/5 factors, computed in 64 bits.
   returns 0 on success, -1 if the result does not fit in an int. */
static int kf_next_fast_size_i64(int64_t n, int * out)
{
    int64_t m;
    if (n < 1)
        n = 1;
    for (;; ++n) {
        if (n > INT32_MAX)
            return -1;
        m = n;
        while ( (m%2) == 0 ) m/=2;
        while ( (m%3) == 0 ) m/=3;
        while ( (m%5) == 0 ) m/=5;
        if (m<=1) {
            *out = (int)n;
            return 0;
        }
    }
}

#ifdef KISS_FFT_BLUESTEIN
/* nonzero if n has a prime factor >= 17, i.e. the classic mixed radix
   path would fall back to the quadratic generic butterfly */
static int kf_needs_bluestein(int n)
{
    static const int small_primes[] = { 2, 3, 5, 7, 11, 13 };
    size_t i;
    for (i=0;i<sizeof(small_primes)/sizeof(small_primes[0]);++i) {
        const int p = small_primes[i];
        while ( (n % p) == 0 )
            n /= p;
    }
    return n > 1;
}

/* accumulate size with overflow check; returns -1 on overflow */
static int kf_size_add(size_t * acc, size_t x)
{
    if (x > SIZE_MAX - *acc)
        return -1;
    *acc += x;
    return 0;
}

/*
 * Bluestein ("chirp-z") plan: an nfft-point DFT is evaluated as a cyclic
 * convolution of length conv_n >= 2*nfft-1, where conv_n has only 2/3/5
 * factors and is therefore handled by the regular fast path.
 *
 *   X[k] = chirp[k] * sum_n ( x[n] chirp[n] ) * conj(chirp[k-n])
 *   chirp[n] = exp( s * pi * i * n^2 / nfft ),  s = -1 forward, +1 inverse
 *
 * The kernel spectrum bfft holds FFT(conj(chirp), circularly wrapped)
 * prescaled by 1/conv_n, so the unnormalized inverse FFT inside the
 * convolution is compensated exactly once and the transform keeps the
 * usual kiss_fft convention: forward then inverse yields nfft * input.
 */
static kiss_fft_cfg kf_bluestein_alloc(int nfft,int inverse_fft,void * mem,size_t * lenmem)
{
    kiss_fft_cfg st = NULL;
    kiss_fft_cfg sub_fwd;
    kiss_fft_cpx *btmp;
    char *ptr;
    size_t memneeded = 0, sz_fwd = 0, sz_inv = 0, sz_check;
    size_t chirp_off, bfft_off, fwd_off, inv_off;
    int conv_n = 0;
    int n;
    int64_t m0;
    kiss_fft_scalar inv_m;
    int allocated = 0;

    m0 = 2*(int64_t)nfft - 1;
    if (m0 > INT32_MAX)
        goto invalid;
    if (kf_next_fast_size_i64(m0, &conv_n) != 0)
        goto invalid;
    /* the byte sizes below must not wrap size_t (32-bit hosts) */
    if ((size_t)nfft > SIZE_MAX / sizeof(kiss_fft_cpx) ||
        (size_t)conv_n > SIZE_MAX / sizeof(kiss_fft_cpx))
        goto invalid;

    /* nested plans are smooth, so these queries cannot recurse back here */
    kiss_fft_alloc(conv_n, 0, NULL, &sz_fwd);
    kiss_fft_alloc(conv_n, 1, NULL, &sz_inv);
    if (sz_fwd == 0 || sz_inv == 0)
        goto invalid;
    if (sz_inv < (size_t)conv_n * sizeof(kiss_fft_cpx))
        goto invalid; /* cannot happen; the b sequence scratch lives there */

    if (kf_size_add(&memneeded, KISS_FFT_ALIGN_SIZE_UP(sizeof(struct kiss_fft_state))) != 0)
        goto invalid;
    chirp_off = memneeded;
    if (kf_size_add(&memneeded, sizeof(kiss_fft_cpx)*(size_t)nfft) != 0)
        goto invalid;
    bfft_off = memneeded;
    if (kf_size_add(&memneeded, sizeof(kiss_fft_cpx)*(size_t)conv_n) != 0)
        goto invalid;
    fwd_off = memneeded;
    if (kf_size_add(&memneeded, sz_fwd) != 0)
        goto invalid;
    inv_off = memneeded;
    if (kf_size_add(&memneeded, sz_inv) != 0)
        goto invalid;

    if (lenmem == NULL) {
        st = (kiss_fft_cfg)KISS_FFT_MALLOC(memneeded);
        allocated = 1;
    } else {
        if (mem != NULL && *lenmem >= memneeded)
            st = (kiss_fft_cfg)mem;
        *lenmem = memneeded;
    }
    if (!st) {
        if (allocated)
            KISS_FFT_FREE(st);
        return NULL;
    }

    ptr = (char*)st;
    st->nfft = nfft;
    st->inverse = inverse_fft;
    memset(st->factors, 0, sizeof(st->factors));
    st->max_radix = 0;
    st->bluestein = 1;
    st->conv_n = conv_n;
    st->chirp = (kiss_fft_cpx*)(ptr + chirp_off);
    st->bfft = (kiss_fft_cpx*)(ptr + bfft_off);
    st->sub_fwd = NULL;
    st->sub_inv = NULL;

    /* chirp factors; n^2 is reduced mod 2*nfft in 64 bits so the phase
       handed to the trig functions stays small and exact */
    for (n=0;n<nfft;++n) {
        const double pi=3.141592653589793238462643383279502884197169399375105820974944;
        int64_t sq = (int64_t)n * (int64_t)n;
        double phase = pi * (double)(sq % (2*(int64_t)nfft)) / (double)nfft;
        if (!inverse_fft)
            phase = -phase;
        kf_cexp(st->chirp + n, phase);
    }

    /* nested forward plan, then use the not-yet-initialized inverse plan
       storage as scratch for the b sequence (sz_inv >= conv_n cpx) */
    sub_fwd = kiss_fft_alloc(conv_n, 0, ptr + fwd_off, &sz_fwd);
    if (sub_fwd != (kiss_fft_cfg)(ptr + fwd_off))
        goto plan_fail;
    st->sub_fwd = sub_fwd;

    btmp = (kiss_fft_cpx*)(ptr + inv_off);
    memset(btmp, 0, sizeof(kiss_fft_cpx)*(size_t)conv_n);
    btmp[0].r = 1;
    btmp[0].i = 0;
    for (n=1;n<nfft;++n) {
        kiss_fft_cpx c;
        c.r = st->chirp[n].r;
        c.i = -st->chirp[n].i; /* conj(chirp) */
        btmp[n] = c;
        btmp[conv_n - n] = c;
    }
    kf_fft_exec(sub_fwd, btmp, st->bfft, 1, NULL);
    inv_m = (kiss_fft_scalar)(1.0 / (double)conv_n);
    for (n=0;n<conv_n;++n) {
        st->bfft[n].r *= inv_m;
        st->bfft[n].i *= inv_m;
    }

    sz_check = sz_inv;
    st->sub_inv = kiss_fft_alloc(conv_n, 1, ptr + inv_off, &sz_check);
    if (st->sub_inv != (kiss_fft_cfg)(ptr + inv_off))
        goto plan_fail;

    return st;

plan_fail:
    if (allocated)
        KISS_FFT_FREE(st);
    if (lenmem)
        *lenmem = 0;
    return NULL;

invalid:
    if (lenmem)
        *lenmem = 0;
    return NULL;
}

static void kf_bluestein_exec(
        kiss_fft_cfg st,
        const kiss_fft_cpx * fin,
        kiss_fft_cpx * fout,
        int in_stride,
        kiss_fft_cpx * work
        )
{
    const int nfft = st->nfft;
    const int conv_n = st->conv_n;
    kiss_fft_cpx * buf1 = work;             /* conv_n */
    kiss_fft_cpx * buf2 = work + conv_n;    /* conv_n */
    int n;

    /* chirp-modulated input, zero padded to the convolution length */
    for (n=0;n<nfft;++n) {
        C_MUL(buf1[n], fin[(size_t)n*in_stride], st->chirp[n]);
    }
    memset(buf1 + nfft, 0, sizeof(kiss_fft_cpx)*(size_t)(conv_n - nfft));

    /* A = FFT(a); the nested plans are 2/3/5-smooth and need no work area */
    kf_fft_exec(st->sub_fwd, buf1, buf2, 1, NULL);

    /* circular convolution in the frequency domain */
    for (n=0;n<conv_n;++n) {
        kiss_fft_cpx t;
        C_MUL(t, buf2[n], st->bfft[n]);
        buf2[n] = t;
    }

    /* conv = IFFT(A .* bfft); bfft carries the 1/conv_n scaling */
    kf_fft_exec(st->sub_inv, buf2, buf1, 1, NULL);

    /* final chirp demodulation */
    for (n=0;n<nfft;++n) {
        C_MUL(fout[n], st->chirp[n], buf1[n]);
    }
}
#endif /* KISS_FFT_BLUESTEIN */

/*
 *
 * User-callable function to allocate all necessary storage space for the fft.
 *
 * The return value is a contiguous block of memory, allocated with malloc.  As such,
 * It can be freed with free(), rather than a kiss_fft-specific function.
 * */
kiss_fft_cfg kiss_fft_alloc(int nfft,int inverse_fft,void * mem,size_t * lenmem )
{
    KISS_FFT_ALIGN_CHECK(mem)

    kiss_fft_cfg st=NULL;
    size_t memneeded;

    if (nfft <= 0) {
        if (lenmem)
            *lenmem = 0;
        return NULL;
    }

#ifdef KISS_FFT_BLUESTEIN
    if (kf_needs_bluestein(nfft))
        return kf_bluestein_alloc(nfft, inverse_fft, mem, lenmem);
#endif

    // check for overflow condition {memneeded > SIZE_MAX}.
    if ((size_t)nfft >= (SIZE_MAX - 2*sizeof(struct kiss_fft_state))/sizeof(kiss_fft_cpx)) {
        if (lenmem)
            *lenmem = 0;
        return NULL;
    }

    memneeded = KISS_FFT_ALIGN_SIZE_UP(sizeof(struct kiss_fft_state)
        + sizeof(kiss_fft_cpx)*(nfft-1)); /* twiddle factors*/

    if ( lenmem==NULL ) {
        st = ( kiss_fft_cfg)KISS_FFT_MALLOC( memneeded );
    }else{
        if (mem != NULL && *lenmem >= memneeded)
            st = (kiss_fft_cfg)mem;
        *lenmem = memneeded;
    }
    if (st) {
        int i;
        st->nfft=nfft;
        st->inverse = inverse_fft;
#ifdef KISS_FFT_BLUESTEIN
        st->bluestein = 0;
        st->conv_n = 0;
        st->chirp = NULL;
        st->bfft = NULL;
        st->sub_fwd = NULL;
        st->sub_inv = NULL;
#endif

        for (i=0;i<nfft;++i) {
            const double pi=3.141592653589793238462643383279502884197169399375105820974944;
            double phase = -2*pi*i / nfft;
            if (st->inverse)
                phase *= -1;
            kf_cexp(st->twiddles+i, phase );
        }

        kf_factor(nfft,st->factors);
        st->max_radix = kf_scan_max_radix(st->factors);
    }
    return st;
}

/* bytes of caller supplied work area needed for an out-of-place execution */
size_t kf_work_size_out(kiss_fft_cfg st)
{
    if (!st)
        return 0;
#ifdef KISS_FFT_BLUESTEIN
    if (st->bluestein)
        return 2*(size_t)st->conv_n*sizeof(kiss_fft_cpx);
#endif
    return (size_t)st->max_radix*sizeof(kiss_fft_cpx);
}

size_t kiss_fft_work_size(kiss_fft_cfg cfg)
{
    if (!cfg)
        return 0;
#ifdef KISS_FFT_BLUESTEIN
    if (cfg->bluestein)
        return 2*(size_t)cfg->conv_n*sizeof(kiss_fft_cpx);
#endif
    /* worst case is an in-place call: full copy buffer plus generic scratch */
    return ((size_t)cfg->nfft + (size_t)cfg->max_radix)*sizeof(kiss_fft_cpx);
}

/* execute with a caller supplied work area; see _kiss_fft_guts.h */
void kf_fft_exec(kiss_fft_cfg st,const kiss_fft_cpx *fin,kiss_fft_cpx *fout,int in_stride,kiss_fft_cpx *work)
{
#ifdef KISS_FFT_BLUESTEIN
    if (st->bluestein) {
        kf_bluestein_exec(st, fin, fout, in_stride, work);
        return;
    }
#endif
    if (fin == fout) {
        //NOTE: this is not really an in-place FFT algorithm.
        //It just performs an out-of-place FFT into a work buffer
        kiss_fft_cpx * tmpbuf = work;
        kiss_fft_cpx * scratch = work + st->nfft;
        kf_work(tmpbuf,fin,1,in_stride, st->factors,st,scratch);
        memcpy(fout,tmpbuf,sizeof(kiss_fft_cpx)*st->nfft);
    }else{
        kf_work( fout, fin, 1,in_stride, st->factors,st,work );
    }
}

int kiss_fft_stride_work(kiss_fft_cfg cfg,const kiss_fft_cpx *fin,kiss_fft_cpx *fout,int in_stride,void *work,size_t bytes)
{
    size_t need;
    if (!cfg || !fin || !fout || in_stride <= 0)
        return -1;
    need = kiss_fft_work_size(cfg);
    if (bytes < need)
        return -1;
    if (need && !work)
        return -1;
    kf_fft_exec(cfg, fin, fout, in_stride, (kiss_fft_cpx*)work);
    return 0;
}

void kiss_fft_stride(kiss_fft_cfg st,const kiss_fft_cpx *fin,kiss_fft_cpx *fout,int in_stride)
{
    size_t worksz;
    kiss_fft_cpx * work;

    if (fin == NULL || fout == NULL){
        KISS_FFT_ERROR("NULL buffer.");
        return;
    }

    /* prepare the temporary workspace for this call in one shot */
    worksz = (fin == fout) ? kiss_fft_work_size(st) : kf_work_size_out(st);
    if (worksz == 0) {
        kf_fft_exec(st, fin, fout, in_stride, NULL);
        return;
    }
    work = (kiss_fft_cpx*)KISS_FFT_TMP_ALLOC(worksz);
    if (work == NULL){
        KISS_FFT_ERROR("Memory allocation error.");
        return;
    }
    kf_fft_exec(st, fin, fout, in_stride, work);
    KISS_FFT_TMP_FREE(work);
}

void kiss_fft(kiss_fft_cfg cfg,const kiss_fft_cpx *fin,kiss_fft_cpx *fout)
{
    kiss_fft_stride(cfg,fin,fout,1);
}


void kiss_fft_cleanup(void)
{
    // nothing needed any more
}

int kiss_fft_next_fast_size(int n)
{
    if (n <= 0) /* nothing to factor; avoid infinite loop for n<=0 */
        return 1;
    while(1) {
        int m=n;
        while ( (m%2) == 0 ) m/=2;
        while ( (m%3) == 0 ) m/=3;
        while ( (m%5) == 0 ) m/=5;
        if (m<=1)
            break; /* n is completely factorable by twos, threes, and fives */
        n++;
    }
    return n;
}
