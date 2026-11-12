/*
 *  Copyright (c) 2003-2004, Mark Borgerding. All rights reserved.
 *  This file is part of KISS FFT - https://github.com/mborgerding/kissfft
 *
 *  SPDX-License-Identifier: BSD-3-Clause
 *  See COPYING file for more information.
 */

#include "kiss_fftnd.h"
#include "_kiss_fft_guts.h"
#include <stdint.h>

struct kiss_fftnd_state{
    int dimprod; /* dimsum would be mighty tasty right now */
    int ndims;
    int *dims;
    kiss_fft_cfg *states; /* cfg states for each dimension */
    kiss_fft_cpx * tmpbuf; /*buffer capable of hold the entire input */
};

/* round a section size up so the following section stays aligned for
   both kiss_fft_cpx and pointer sized data */
static size_t kfnd_section(size_t nbytes)
{
    size_t x = KISS_FFT_ALIGN_SIZE_UP(nbytes);
    size_t rem = x % sizeof(void*);
    if (rem)
        x += sizeof(void*) - rem;
    return x;
}

static int kfnd_size_add(size_t * acc, size_t x)
{
    if (x > SIZE_MAX - *acc)
        return -1;
    *acc += x;
    return 0;
}

kiss_fftnd_cfg kiss_fftnd_alloc(const int *dims,int ndims,int inverse_fft,void*mem,size_t*lenmem)
{
    KISS_FFT_ALIGN_CHECK(mem)

    kiss_fftnd_cfg st = NULL;
    int i;
    int dimprod=1;
    size_t memneeded = 0;
    char * ptr = NULL;

    if (!dims || ndims <= 0) {
        if (lenmem)
            *lenmem = 0;
        return NULL;
    }

    if (kfnd_size_add(&memneeded, kfnd_section(sizeof(struct kiss_fftnd_state))) != 0)
        goto overflow;
    if (kfnd_size_add(&memneeded, kfnd_section(sizeof(void*) * (size_t)ndims)) != 0) /* st->states */
        goto overflow;
    if (kfnd_size_add(&memneeded, kfnd_section(sizeof(int) * (size_t)ndims)) != 0) /* st->dims */
        goto overflow;

    for (i=0;i<ndims;++i) {
        size_t sublen=0;
        if (dims[i] <= 0 || dimprod > INT_MAX / dims[i])
            goto overflow;
        dimprod *= dims[i];
        kiss_fft_alloc (dims[i], inverse_fft, NULL, &sublen);
        if (sublen == 0)
            goto overflow; /* sub-plan rejected this length */
        if (kfnd_size_add(&memneeded, kfnd_section(sublen)) != 0) /* st->states[i] */
            goto overflow;
    }
    if ((size_t)dimprod > SIZE_MAX / sizeof(kiss_fft_cpx))
        goto overflow;
    if (kfnd_size_add(&memneeded, kfnd_section(sizeof(kiss_fft_cpx) * (size_t)dimprod)) != 0) /* st->tmpbuf */
        goto overflow;

    if (lenmem == NULL) {/* allocate for the caller*/
        ptr = (char *) KISS_FFT_MALLOC (memneeded);
    } else { /* initialize supplied buffer if big enough */
        if (*lenmem >= memneeded)
            ptr = (char *) mem;
        *lenmem = memneeded; /*tell caller how big struct is (or would be) */
    }
    if (!ptr)
        return NULL; /*malloc failed or buffer too small */

    st = (kiss_fftnd_cfg) ptr;
    st->dimprod = dimprod;
    st->ndims = ndims;
    ptr += kfnd_section(sizeof(struct kiss_fftnd_state));

    st->states = (kiss_fft_cfg *)ptr;
    ptr += kfnd_section(sizeof(void*) * (size_t)ndims);

    st->dims = (int*)ptr;
    ptr += kfnd_section(sizeof(int) * (size_t)ndims);

    st->tmpbuf = (kiss_fft_cpx*)ptr;
    ptr += kfnd_section(sizeof(kiss_fft_cpx) * (size_t)dimprod);

    for (i=0;i<ndims;++i) {
        size_t len = 0;
        st->dims[i] = dims[i];
        kiss_fft_alloc (st->dims[i], inverse_fft, NULL, &len);
        st->states[i] = kiss_fft_alloc (st->dims[i], inverse_fft, ptr,&len);
        if (st->states[i] != (kiss_fft_cfg)ptr) {
            if (lenmem == NULL)
                KISS_FFT_FREE(st);
            if (lenmem)
                *lenmem = 0;
            return NULL;
        }
        ptr += kfnd_section(len);
    }
    /*
Hi there!

If you're looking at this particular code, it probably means you've got a brain-dead bounds checker
that thinks the above code overwrites the end of the array.

It doesn't.

-- Mark

P.S.
The below code might give you some warm fuzzies and help convince you.
       */
    if ( (size_t)(ptr - (char*)st) != memneeded ) {
        fprintf(stderr,
                "################################################################################\n"
                "Internal error! Memory allocation miscalculation\n"
                "################################################################################\n"
               );
    }
    return st;

overflow:
    if (lenmem)
        *lenmem = 0;
    return NULL;
}

/* largest out-of-place work area needed by any of the per-dimension plans */
static size_t kf_fftnd_subwork_size(const kiss_fftnd_cfg st)
{
    int k;
    size_t mx = 0;
    for (k=0;k<st->ndims;++k) {
        size_t s = kf_work_size_out(st->states[k]);
        if (s > mx)
            mx = s;
    }
    return mx;
}

/*
 This works by tackling one dimension at a time.

 In effect,
 Each stage starts out by reshaping the matrix into a DixSi 2d matrix.
 A Di-sized fft is taken of each column, transposing the matrix as it goes.

Here's a 3-d example:
Take a 2x3x4 matrix, laid out in memory as a contiguous buffer
 [ [ [ a b c d ] [ e f g h ] [ i j k l ] ]
   [ [ m n o p ] [ q r s t ] [ u v w x ] ] ]

Stage 0 ( D=2): treat the buffer as a 2x12 matrix
   [ [a b ... k l]
     [m n ... w x] ]

   FFT each column with size 2.
   Transpose the matrix at the same time using kiss_fft_stride.

   [ [ a+m a-m ]
     [ b+n b-n]
     ...
     [ k+w k-w ]
     [ l+x l-x ] ]

   Note fft([x y]) == [x+y x-y]

Stage 1 ( D=3) treats the buffer (the output of stage D=2) as an 3x8 matrix,
   [ [ a+m a-m b+n b-n c+o c-o d+p d-p ]
     [ e+q e-q f+r f-r g+s g-s h+t h-t ]
     [ i+u i-u j+v j-v k+w k-w l+x l-x ] ]

   And perform FFTs (size=3) on each of the columns as above, transposing
   the matrix as it goes.  The output of stage 1 is
       (Legend: ap = [ a+m e+q i+u ]
                am = [ a-m e-q i-u ] )

   [ [ sum(ap) fft(ap)[0] fft(ap)[1] ]
     [ sum(am) fft(am)[0] fft(am)[1] ]
     [ sum(bp) fft(bp)[0] fft(bp)[1] ]
     [ sum(bm) fft(bm)[0] fft(bm)[1] ]
     [ sum(cp) fft(cp)[0] fft(cp)[1] ]
     [ sum(cm) fft(cm)[0] fft(cm)[1] ]
     [ sum(dp) fft(dp)[0] fft(dp)[1] ]
     [ sum(dm) fft(dm)[0] fft(dm)[1] ]  ]

Stage 2 ( D=4) treats this buffer as a 4*6 matrix,
   [ [ sum(ap) fft(ap)[0] fft(ap)[1] sum(am) fft(am)[0] fft(am)[1] ]
     [ sum(bp) fft(bp)[0] fft(bp)[1] sum(bm) fft(bm)[0] fft(bm)[1] ]
     [ sum(cp) fft(cp)[0] fft(cp)[1] sum(cm) fft(cm)[0] fft(cm)[1] ]
     [ sum(dp) fft(dp)[0] fft(dp)[1] sum(dm) fft(dm)[0] fft(dm)[1] ]  ]

   Then FFTs each column, transposing as it goes.

   The resulting matrix is the 3d FFT of the 2x3x4 input matrix.

   Note as a sanity check that the first element of the final
   stage's output (DC term) is
   sum( [ sum(ap) sum(bp) sum(cp) sum(dp) ] )
   , i.e. the summation of all 24 input elements.

   tmpbuf (dimprod cpx) and subwork (kf_fftnd_subwork_size bytes) are
   supplied by the caller so the plan itself stays read-only here.
*/
static void kf_fftnd_run(kiss_fftnd_cfg st,const kiss_fft_cpx *fin,kiss_fft_cpx *fout,kiss_fft_cpx *tmpbuf,kiss_fft_cpx *subwork)
{
    int i,k;
    const kiss_fft_cpx * bufin=fin;
    kiss_fft_cpx * bufout;

    /*arrange it so the last bufout == fout*/
    if ( st->ndims & 1 ) {
        bufout = fout;
        if (fin==fout) {
            memcpy( tmpbuf, fin, sizeof(kiss_fft_cpx) * (size_t)st->dimprod );
            bufin = tmpbuf;
        }
    }else
        bufout = tmpbuf;

    for ( k=0; k < st->ndims; ++k) {
        int curdim = st->dims[k];
        int stride = st->dimprod / curdim;

        for ( i=0 ; i<stride ; ++i )
            kf_fft_exec( st->states[k], bufin+i , bufout+i*curdim, stride, subwork );

        /*toggle back and forth between the two buffers*/
        if (bufout == tmpbuf){
            bufout = fout;
            bufin = tmpbuf;
        }else{
            bufout = tmpbuf;
            bufin = fout;
        }
    }
}

size_t kiss_fftnd_work_size(kiss_fftnd_cfg cfg)
{
    if (!cfg)
        return 0;
    return (size_t)cfg->dimprod * sizeof(kiss_fft_cpx) + kf_fftnd_subwork_size(cfg);
}

int kiss_fftnd_work(kiss_fftnd_cfg cfg,const kiss_fft_cpx *fin,kiss_fft_cpx *fout,void *work,size_t bytes)
{
    size_t need;
    kiss_fft_cpx * tmpbuf;
    kiss_fft_cpx * subwork;
    if (!cfg || !fin || !fout)
        return -1;
    need = kiss_fftnd_work_size(cfg);
    if (bytes < need)
        return -1;
    if (need && !work)
        return -1;
    tmpbuf = (kiss_fft_cpx*)work;
    subwork = (kiss_fft_cpx*)((char*)work + (size_t)cfg->dimprod * sizeof(kiss_fft_cpx));
    kf_fftnd_run(cfg, fin, fout, tmpbuf, subwork);
    return 0;
}

void kiss_fftnd(kiss_fftnd_cfg st,const kiss_fft_cpx *fin,kiss_fft_cpx *fout)
{
    size_t ssz = kf_fftnd_subwork_size(st);
    kiss_fft_cpx * subwork = NULL;

    /* prepare the sub-transform workspace for this call in one shot */
    if (ssz) {
        subwork = (kiss_fft_cpx*)KISS_FFT_TMP_ALLOC(ssz);
        if (subwork == NULL) {
            KISS_FFT_ERROR("Memory allocation error.");
            return;
        }
    }
    kf_fftnd_run(st, fin, fout, st->tmpbuf, subwork);
    if (subwork)
        KISS_FFT_TMP_FREE(subwork);
}
