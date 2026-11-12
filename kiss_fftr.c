/*
 *  Copyright (c) 2003-2004, Mark Borgerding. All rights reserved.
 *  This file is part of KISS FFT - https://github.com/mborgerding/kissfft
 *
 *  SPDX-License-Identifier: BSD-3-Clause
 *  See COPYING file for more information.
 */

#include "kiss_fftr.h"
#include "_kiss_fft_guts.h"

struct kiss_fftr_state{
    kiss_fft_cfg substate;
    kiss_fft_cpx * tmpbuf;
    kiss_fft_cpx * super_twiddles;
#ifdef USE_SIMD
    void * pad;
#endif
};

kiss_fftr_cfg kiss_fftr_alloc(int nfft,int inverse_fft,void * mem,size_t * lenmem)
{
	KISS_FFT_ALIGN_CHECK(mem)

    int i;
    kiss_fftr_cfg st = NULL;
    size_t subsize = 0, memneeded;

    if (nfft <= 0 || (nfft & 1)) {
        if (lenmem)
            *lenmem = 0;
        if (nfft > 0)
            KISS_FFT_ERROR("Real FFT optimization must be even.");
        return NULL;
    }
    nfft >>= 1;

    kiss_fft_alloc (nfft, inverse_fft, NULL, &subsize);
    if (subsize == 0) {
        if (lenmem)
            *lenmem = 0;
        return NULL;
    }
    memneeded = sizeof(struct kiss_fftr_state) + subsize + sizeof(kiss_fft_cpx) * ( nfft * 3 / 2);

    if (lenmem == NULL) {
        st = (kiss_fftr_cfg) KISS_FFT_MALLOC (memneeded);
    } else {
        if (*lenmem >= memneeded)
            st = (kiss_fftr_cfg) mem;
        *lenmem = memneeded;
    }
    if (!st)
        return NULL;

    st->substate = (kiss_fft_cfg) (st + 1); /*just beyond kiss_fftr_state struct */
    st->tmpbuf = (kiss_fft_cpx *) (((char *) st->substate) + subsize);
    st->super_twiddles = st->tmpbuf + nfft;
    if (kiss_fft_alloc(nfft, inverse_fft, st->substate, &subsize) != st->substate) {
        if (lenmem == NULL)
            KISS_FFT_FREE(st);
        if (lenmem)
            *lenmem = 0;
        return NULL;
    }

    for (i = 0; i < nfft/2; ++i) {
        double phase =
            -3.14159265358979323846264338327 * ((double) (i+1) / nfft + .5);
        if (inverse_fft)
            phase *= -1;
        kf_cexp (st->super_twiddles+i,phase);
    }
    return st;
}

/* shared forward transform; tmpbuf (ncfft cpx) and subwork
   (kf_work_size_out(substate) bytes) are supplied by the caller */
static void kf_fftr_run(kiss_fftr_cfg st,const kiss_fft_scalar *timedata,kiss_fft_cpx *freqdata,kiss_fft_cpx *tmpbuf,kiss_fft_cpx *subwork)
{
    /* input buffer timedata is stored row-wise */
    int k,ncfft;
    kiss_fft_cpx fpnk,fpk,f1k,f2k,tw,tdc;

    ncfft = st->substate->nfft;

    /*perform the parallel fft of two real signals packed in real,imag*/
    kf_fft_exec( st->substate , (const kiss_fft_cpx*)timedata, tmpbuf, 1, subwork );
    /* The real part of the DC element of the frequency spectrum in tmpbuf
     * contains the sum of the even-numbered elements of the input time sequence
     * The imag part is the sum of the odd-numbered elements
     *
     * The sum of tdc.r and tdc.i is the sum of the input time sequence.
     *      yielding DC of input time sequence
     * The difference of tdc.r - tdc.i is the sum of the input (dot product) [1,-1,1,-1...
     *      yielding Nyquist bin of input time sequence
     */

    tdc.r = tmpbuf[0].r;
    tdc.i = tmpbuf[0].i;
    C_FIXDIV(tdc,2);
    CHECK_OVERFLOW_OP(tdc.r ,+, tdc.i);
    CHECK_OVERFLOW_OP(tdc.r ,-, tdc.i);
    freqdata[0].r = tdc.r + tdc.i;
    freqdata[ncfft].r = tdc.r - tdc.i;
#ifdef USE_SIMD
#ifdef HAVE_LASX
    freqdata[0].i = (__m256)(__lasx_xvreplgr2vr_w(0));
    freqdata[ncfft].i = freqdata[0].i;
#elif defined(HAVE_LSX)
    freqdata[0].i = (__m128)(__lsx_vreplgr2vr_w(0));
    freqdata[ncfft].i = freqdata[0].i;
#else
    freqdata[ncfft].i = freqdata[0].i = _mm_set1_ps(0);
#endif
#else
    freqdata[ncfft].i = freqdata[0].i = 0;
#endif

    for ( k=1;k <= ncfft/2 ; ++k ) {
        fpk    = tmpbuf[k];
        fpnk.r =   tmpbuf[ncfft-k].r;
        fpnk.i = - tmpbuf[ncfft-k].i;
        C_FIXDIV(fpk,2);
        C_FIXDIV(fpnk,2);

        C_ADD( f1k, fpk , fpnk );
        C_SUB( f2k, fpk , fpnk );
        C_MUL( tw , f2k , st->super_twiddles[k-1]);

        freqdata[k].r = HALF_OF(f1k.r + tw.r);
        freqdata[k].i = HALF_OF(f1k.i + tw.i);
        freqdata[ncfft-k].r = HALF_OF(f1k.r - tw.r);
        freqdata[ncfft-k].i = HALF_OF(tw.i - f1k.i);
    }
}

/* shared inverse transform; tmpbuf (ncfft cpx) and subwork
   (kf_work_size_out(substate) bytes) are supplied by the caller */
static void kf_fftri_run(kiss_fftr_cfg st,const kiss_fft_cpx *freqdata,kiss_fft_scalar *timedata,kiss_fft_cpx *tmpbuf,kiss_fft_cpx *subwork)
{
    /* input buffer timedata is stored row-wise */
    int k, ncfft;

    ncfft = st->substate->nfft;

    tmpbuf[0].r = freqdata[0].r + freqdata[ncfft].r;
    tmpbuf[0].i = freqdata[0].r - freqdata[ncfft].r;
    C_FIXDIV(tmpbuf[0],2);

    for (k = 1; k <= ncfft / 2; ++k) {
        kiss_fft_cpx fk, fnkc, fek, fok, tmp;
        fk = freqdata[k];
        fnkc.r = freqdata[ncfft - k].r;
        fnkc.i = -freqdata[ncfft - k].i;
        C_FIXDIV( fk , 2 );
        C_FIXDIV( fnkc , 2 );

        C_ADD (fek, fk, fnkc);
        C_SUB (tmp, fk, fnkc);
        C_MUL (fok, tmp, st->super_twiddles[k-1]);
        C_ADD (tmpbuf[k],     fek, fok);
        C_SUB (tmpbuf[ncfft - k], fek, fok);
#ifdef USE_SIMD
#ifdef HAVE_LASX
        __m256 neg_one = (__m256)__lasx_xvreplgr2vr_w(0xBF800000); // -1.0f
        tmpbuf[ncfft - k].i = __lasx_xvfmul_s(tmpbuf[ncfft - k].i, neg_one);
#elif defined(HAVE_LSX)
        __m128 neg_one = (__m128)__lsx_vreplgr2vr_w(0xBF800000); // -1.0f
        tmpbuf[ncfft - k].i = __lsx_vfmul_s(tmpbuf[ncfft - k].i, neg_one);
#else
        tmpbuf[ncfft - k].i *= _mm_set1_ps(-1.0);
#endif
#else
        tmpbuf[ncfft - k].i *= -1;
#endif
    }
    kf_fft_exec (st->substate, tmpbuf, (kiss_fft_cpx *) timedata, 1, subwork);
}

size_t kiss_fftr_work_size(kiss_fftr_cfg cfg)
{
    if (!cfg)
        return 0;
    return (size_t)cfg->substate->nfft * sizeof(kiss_fft_cpx)
        + kf_work_size_out(cfg->substate);
}

int kiss_fftr_work(kiss_fftr_cfg cfg,const kiss_fft_scalar *timedata,kiss_fft_cpx *freqdata,void *work,size_t bytes)
{
    size_t need;
    kiss_fft_cpx * tmpbuf;
    kiss_fft_cpx * subwork;
    if (!cfg || !timedata || !freqdata)
        return -1;
    if (cfg->substate->inverse)
        return -1;/* The caller did not call the correct function */
    need = kiss_fftr_work_size(cfg);
    if (bytes < need)
        return -1;
    if (need && !work)
        return -1;
    tmpbuf = (kiss_fft_cpx*)work;
    subwork = (kiss_fft_cpx*)((char*)work + (size_t)cfg->substate->nfft * sizeof(kiss_fft_cpx));
    kf_fftr_run(cfg, timedata, freqdata, tmpbuf, subwork);
    return 0;
}

int kiss_fftri_work(kiss_fftr_cfg cfg,const kiss_fft_cpx *freqdata,kiss_fft_scalar *timedata,void *work,size_t bytes)
{
    size_t need;
    kiss_fft_cpx * tmpbuf;
    kiss_fft_cpx * subwork;
    if (!cfg || !freqdata || !timedata)
        return -1;
    if (cfg->substate->inverse == 0)
        return -1;/* The caller did not call the correct function */
    need = kiss_fftr_work_size(cfg);
    if (bytes < need)
        return -1;
    if (need && !work)
        return -1;
    tmpbuf = (kiss_fft_cpx*)work;
    subwork = (kiss_fft_cpx*)((char*)work + (size_t)cfg->substate->nfft * sizeof(kiss_fft_cpx));
    kf_fftri_run(cfg, freqdata, timedata, tmpbuf, subwork);
    return 0;
}

void kiss_fftr(kiss_fftr_cfg st,const kiss_fft_scalar *timedata,kiss_fft_cpx *freqdata)
{
    size_t ssz;
    kiss_fft_cpx * subwork = NULL;

    if ( st->substate->inverse) {
        KISS_FFT_ERROR("kiss fft usage error: improper alloc");
        return;/* The caller did not call the correct function */
    }

    /* prepare the sub-transform workspace for this call in one shot */
    ssz = kf_work_size_out(st->substate);
    if (ssz) {
        subwork = (kiss_fft_cpx*)KISS_FFT_TMP_ALLOC(ssz);
        if (subwork == NULL) {
            KISS_FFT_ERROR("Memory allocation error.");
            return;
        }
    }
    kf_fftr_run(st, timedata, freqdata, st->tmpbuf, subwork);
    if (subwork)
        KISS_FFT_TMP_FREE(subwork);
}

void kiss_fftri(kiss_fftr_cfg st,const kiss_fft_cpx *freqdata,kiss_fft_scalar *timedata)
{
    size_t ssz;
    kiss_fft_cpx * subwork = NULL;

    if (st->substate->inverse == 0) {
        KISS_FFT_ERROR("kiss fft usage error: improper alloc");
        return;/* The caller did not call the correct function */
    }

    ssz = kf_work_size_out(st->substate);
    if (ssz) {
        subwork = (kiss_fft_cpx*)KISS_FFT_TMP_ALLOC(ssz);
        if (subwork == NULL) {
            KISS_FFT_ERROR("Memory allocation error.");
            return;
        }
    }
    kf_fftri_run(st, freqdata, timedata, st->tmpbuf, subwork);
    if (subwork)
        KISS_FFT_TMP_FREE(subwork);
}
