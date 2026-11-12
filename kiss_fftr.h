/*
 *  Copyright (c) 2003-2004, Mark Borgerding. All rights reserved.
 *  This file is part of KISS FFT - https://github.com/mborgerding/kissfft
 *
 *  SPDX-License-Identifier: BSD-3-Clause
 *  See COPYING file for more information.
 */

#ifndef KISS_FTR_H
#define KISS_FTR_H

#include "kiss_fft.h"
#ifdef __cplusplus
extern "C" {
#endif

    
/* 
 
 Real optimized version can save about 45% cpu time vs. complex fft of a real seq.

 
 
 */

typedef struct kiss_fftr_state *kiss_fftr_cfg;


kiss_fftr_cfg KISS_FFT_API kiss_fftr_alloc(int nfft,int inverse_fft,void * mem, size_t * lenmem);
/*
 nfft must be even

 If you don't care to allocate space, use mem = lenmem = NULL 
*/


void KISS_FFT_API kiss_fftr(kiss_fftr_cfg cfg,const kiss_fft_scalar *timedata,kiss_fft_cpx *freqdata);
/*
 input timedata has nfft scalar points
 output freqdata has nfft/2+1 complex points
*/

void KISS_FFT_API kiss_fftri(kiss_fftr_cfg cfg,const kiss_fft_cpx *freqdata,kiss_fft_scalar *timedata);
/*
 input freqdata has  nfft/2+1 complex points
 output timedata has nfft scalar points
 For floating-point builds, a kiss_fftr / kiss_fftri round trip returns
 nfft times the input; divide timedata by nfft to recover the input.
 Fixed-point builds scale internally and use a different convention.
*/

/*
 * kiss_fftr_work_size(cfg)
 *
 * Bytes of caller supplied workspace needed by kiss_fftr_work /
 * kiss_fftri_work for this plan.  Returns 0 for a NULL cfg.
 */
size_t KISS_FFT_API kiss_fftr_work_size(kiss_fftr_cfg cfg);

/*
 * kiss_fftr_work / kiss_fftri_work
 *
 * Same transforms as kiss_fftr / kiss_fftri, but all temporary storage
 * (including the buffer the classic entry points keep inside the plan)
 * comes from the caller supplied 'work' area of 'bytes' bytes (at least
 * kiss_fftr_work_size(cfg)).  No heap memory is allocated or freed and
 * the plan is not modified.  The usual in-place forms are supported:
 * timedata and freqdata may share one buffer of nfft+2 scalars.
 *
 * Return 0 on success, -1 on invalid arguments or insufficient workspace.
 */
int KISS_FFT_API kiss_fftr_work(kiss_fftr_cfg cfg,const kiss_fft_scalar *timedata,kiss_fft_cpx *freqdata,void *work,size_t bytes);
int KISS_FFT_API kiss_fftri_work(kiss_fftr_cfg cfg,const kiss_fft_cpx *freqdata,kiss_fft_scalar *timedata,void *work,size_t bytes);

#define kiss_fftr_free KISS_FFT_FREE

#ifdef __cplusplus
}
#endif
#endif
