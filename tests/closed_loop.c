/*
 * tests/closed_loop.c
 *
 * Independent reference model and regression checks for the libgsm
 * RPE-LTP encoder: long term prediction (ltp), residual pulse excitation
 * (rpe) and the closed analysis/reconstruction loop (loop).
 *
 * Ground rules used throughout this file:
 *   - Expected values are computed by generic loops over the model
 *     equations using 64 bit intermediates, with explicit floor for
 *     arithmetic right shifts and explicit 16 bit clamping where the
 *     production code saturates.  No production LTP / RPE / quantizer /
 *     dequantizer routine is called to obtain an expected value, and
 *     none of the unrolled production loops is reproduced.
 *   - The only production routines used as reference are the front end
 *     of the loop group (preprocess, LPC analysis, short term analysis),
 *     which this suite deliberately does not model; they run on a
 *     separately created context so the model loop state stays private.
 *   - Every deviation is reported through fprintf() with the phase, the
 *     frame/subframe and the first field or sample that differs, then
 *     recorded with TASK_CHECK(0) so the fixed runner exits with 4.
 *
 * There is no main() here: task_support/driver.c is the runner.
 */

#include "task_support/check.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>

#include "private.h"		/* struct gsm_state, word, Gsm_Coder, tables */
#include "gsm.h"

/*
 * Every numerical deviation is reported here.  The fixed marker
 * CONTENT_MISMATCH lets tests/check_closed_loop_mutants.sh recognise a
 * detected behavioural change without parsing the detail line, which
 * carries the phase, the frame/subframe and the first field or sample
 * that differed.
 */
static void cl_mismatch(const char *detail_fmt, ...)
{
	va_list ap;

	fprintf(stderr, "CONTENT_MISMATCH\n");
	va_start(ap, detail_fmt);
	vfprintf(stderr, detail_fmt, ap);
	va_end(ap);
}

/* ------------------------------------------------------------------ */
/* Explicit fixed point primitives                                     */
/* ------------------------------------------------------------------ */

/* Arithmetic right shift == floor(v / 2^s), independent of the host's
 * implementation defined behaviour for negative >>. */
static int32_t cl_floor_shift(int64_t v, int s)
{
	int64_t d = (int64_t)1 << s;
	int64_t q = v / d;

	if (v % d != 0 && v < 0) q -= 1;	/* truncation -> floor */
	return (int32_t)q;
}

static int16_t cl_sat16(int64_t v)
{
	if (v < -32768) return -32768;
	if (v >  32767) return  32767;
	return (int16_t)v;
}

static int16_t cl_add(int16_t a, int16_t b)
{
	return cl_sat16((int64_t)a + (int64_t)b);
}

static int16_t cl_sub(int16_t a, int16_t b)
{
	return cl_sat16((int64_t)a - (int64_t)b);
}

/* gsm_mult semantics: truncating 15 bit product, clipped to word. */
static int16_t cl_mul_q15(int16_t a, int16_t b)
{
	if (a == MIN_WORD && b == MIN_WORD) return MAX_WORD;
	return cl_sat16(cl_floor_shift((int64_t)a * (int64_t)b, 15));
}

/* gsm_mult_r semantics: rounded 15 bit product, clipped to word. */
static int16_t cl_mul_r15(int16_t a, int16_t b)
{
	if (a == MIN_WORD && b == MIN_WORD) return MAX_WORD;
	return cl_sat16(cl_floor_shift((int64_t)a * (int64_t)b + 16384, 15));
}

/* gsm_norm: number of left shifts until bit 30 is set.  Written as a
 * plain loop instead of the production byte table. */
static int cl_norm30(int32_t a)
{
	int32_t v = a;
	int n = 0;

	if (v < 0) v = ~v;
	while (n < 31 && !(v & 0x40000000)) {
		v <<= 1;
		n++;
	}
	return n;
}

/* ------------------------------------------------------------------ */
/* Long term prediction model                                          */
/* ------------------------------------------------------------------ */

struct cl_ltp {
	int	Nc;		/* selected lag, 40..120                       */
	int	bc;		/* coded gain 0..3                             */
	int	scal;		/* residual scaling 0..6                       */
	int	ties;		/* lags sharing the winning positive maximum   */
	int	all_nonpos;	/* no positive cross correlation at all        */
	int32_t max_corr;	/* winning raw correlation, pre rescale         */
	int16_t	dpp[40];	/* predicted samples                           */
	int16_t	e[40];		/* residual after subtraction                  */
};

/*
 * d       current subframe short term residual, 40 samples
 * hist    pointer just past the previous reconstructed residual
 *         history: hist[-120] is dp[-120], hist[-1] is dp[-1].  This is
 *         the same convention as Gsm_Long_Term_Predictor's dp argument.
 *
 * The lag search runs over 40..120 inclusive, keeps the earliest lag on
 * a tie (strict "greater than" in the scan), and keeps lag 40 when no
 * positive correlation exists.
 */
static void cl_ltp_model(const int16_t *d, const int16_t *hist, struct cl_ltp *out)
{
	int32_t delta = 0;
	int32_t temp;
	int scal;
	int lam, k;
	int32_t max_corr;
	int nc;
	int ties;
	int64_t power;
	int bc;

	for (k = 0; k < 40; k++) {
		int32_t a = d[k] < 0 ? -(int32_t)d[k] : (int32_t)d[k];
		if (d[k] == MIN_WORD) a = MAX_WORD;	/* GSM_ABS */
		if (a > delta) delta = a;
	}

	/* dmax scaled by 2^16 then normalised; > 6 means "do not scale". */
	temp = 0;
	if (delta != 0) temp = cl_norm30(delta << 16);
	scal = temp > 6 ? 0 : 6 - temp;

	max_corr = 0;
	nc = 40;
	ties = 0;
	for (lam = 40; lam <= 120; lam++) {
		int64_t acc = 0;
		for (k = 0; k < 40; k++)
			acc += (int64_t)cl_floor_shift(d[k], scal) *
			       (int64_t)hist[k - lam];
		if (acc > max_corr) {
			max_corr = (int32_t)acc;
			nc = lam;
			ties = 1;
		} else if (acc == max_corr && acc > 0) {
			ties++;
		}
	}

	/* Correlation for that lag, doubled, then divided by 2^(6-scal). */
	power = 0;
	for (k = 0; k < 40; k++) {
		int32_t t = cl_floor_shift(hist[k - nc], 3);
		power += (int64_t)t * t;
	}
	power <<= 1;

	{
		int64_t lmax = (int64_t)max_corr << 1;
		lmax = cl_floor_shift(lmax, 6 - scal);
		if (lmax <= 0) {
			bc = 0;
		} else if (lmax >= power) {
			bc = 3;
		} else {
			int norm = cl_norm30((int32_t)power);
			int16_t R = cl_sat16(cl_floor_shift(lmax << norm, 16));
			int16_t S = cl_sat16(cl_floor_shift(power << norm, 16));
			static const int16_t DLB[4] = { 6554, 16384, 26214, 32767 };
			int i;
			bc = 3;
			for (i = 0; i <= 2; i++) {
				if (R <= cl_mul_q15(S, DLB[i])) {
					bc = i;
					break;
				}
			}
		}
	}

	out->Nc         = nc;
	out->bc         = bc;
	out->scal       = scal;
	out->ties       = ties;
	out->all_nonpos = max_corr <= 0;
	out->max_corr   = max_corr;

	{
		static const int16_t QLB[4] = { 3277, 11469, 21299, 32767 };
		for (k = 0; k < 40; k++) {
			out->dpp[k] = cl_mul_r15(QLB[bc], hist[k - nc]);
			out->e[k]   = cl_sub(d[k], out->dpp[k]);
		}
	}
}

/*
 * Compare the model against Gsm_Long_Term_Predictor, one field and one
 * sample at a time.  Every comparison is recorded with TASK_CHECK; the
 * first difference also produces a CONTENT_MISMATCH line with the phase
 * and the offending index.  Returns the number of mismatches (0 on
 * success).  hist uses the same "pointer past the window" convention as
 * both the model and the production dp argument.
 */
static int cl_ltp_check(const char *tag, const int16_t *d,
			const int16_t *hist, struct cl_ltp *ref)
{
	struct gsm_state S;
	word e[40], dpp[40], Nc, bc;
	int k, bad = 0;

	memset(&S, 0, sizeof(S));
	Gsm_Long_Term_Predictor(&S, (word *)d, (word *)hist,
				(word *)e, (word *)dpp, &Nc, &bc);
	cl_ltp_model(d, hist, ref);

	if (ref->Nc != (int)Nc) {
		cl_mismatch("PHASE ltp %s sub=0 field=Nc model=%d lib=%d\n",
			    tag, ref->Nc, (int)Nc);
		bad++;
	}
	if (ref->bc != (int)bc) {
		cl_mismatch("PHASE ltp %s sub=0 field=bc model=%d lib=%d\n",
			    tag, ref->bc, (int)bc);
		bad++;
	}
	for (k = 0; k < 40; k++) {
		if (ref->dpp[k] != dpp[k]) {
			cl_mismatch("PHASE ltp %s sub=0 field=dpp[%d] model=%d lib=%d\n",
				    tag, k, ref->dpp[k], dpp[k]);
			bad++;
		}
		if (ref->e[k] != e[k]) {
			cl_mismatch("PHASE ltp %s sub=0 field=e[%d] model=%d lib=%d\n",
				    tag, k, ref->e[k], e[k]);
			bad++;
		}
	}
	/* One recorded check per field and per sample. */
	TASK_CHECK(ref->Nc == (int)Nc);
	TASK_CHECK(ref->bc == (int)bc);
	for (k = 0; k < 40; k++) TASK_CHECK(ref->dpp[k] == dpp[k]);
	for (k = 0; k < 40; k++) TASK_CHECK(ref->e[k] == e[k]);
	return bad;
}

/* ------------------------------------------------------------------ */
/* RPE model                                                           */
/* ------------------------------------------------------------------ */

struct cl_rpe {
	int	Mc;		/* selected grid 0..3                          */
	int	exp;		/* decoded exponent of xmaxc, -4..6            */
	int	mant;		/* decoded mantissa of xmaxc, 0..7             */
	int16_t	xmaxc;		/* coded maximum amplitude                     */
	int32_t	x[40];		/* weighted samples (model also keeps these)   */
	int32_t	energy[4];	/* per grid energies                           */
	int16_t	xM[13];		/* selected grid samples                       */
	int16_t	xMc[13];	/* 3 bit codes, 0..7                           */
	int16_t	xMp[13];	/* inverse quantised grid samples              */
	int16_t	ep[40];		/* sparse reconstructed residual               */
};

/*
 * Weighting filter over e[-5..44]; the five samples on each side are
 * supplied as zero.  Implemented as a plain finite convolution.
 */
static void cl_weighting(const int16_t *e40, int32_t *x)
{
	static const int16_t H[11] = {
		-134, -374, 0, 2054, 5741, 8192, 5741, 2054, 0, -374, -134
	};
	int k, i;

	for (k = 0; k < 40; k++) {
		int64_t acc = 4096;			/* 8192 >> 1 */
		for (i = 0; i <= 10; i++) {
			int idx = k + i - 5;		/* e[k+i], zero padded */
			int16_t v = (idx < 0 || idx >= 40) ? 0 : e40[idx];
			acc += (int64_t)v * (int64_t)H[i];
		}
		x[k] = cl_sat16(cl_floor_shift(acc, 13));
	}
}

/*
 * Grid selection: energy of every third sample starting at Mc, for the
 * four grids.  Grid 0 and grid 3 share positions 3..36 but differ in the
 * endpoints (position 0 only for grid 0, position 39 only for grid 3).
 * Equal energies keep the smaller grid number.
 */
static void cl_grid_select(const int32_t *x, int *mc, int32_t energy[4])
{
	int m, i;

	for (m = 0; m < 4; m++) {
		int64_t acc = 0;
		for (i = 0; i <= 12; i++) {
			int32_t t = cl_floor_shift(x[m + 3 * i], 2);
			acc += (int64_t)t * t;
		}
		energy[m] = (int32_t)acc;
	}

	*mc = 0;
	for (m = 1; m < 4; m++)
		if (energy[m] > energy[*mc]) *mc = m;
}

/* xmaxc -> (exp, mantissa).  Plain arithmetic, no table. */
static void cl_decode_xmaxc(int16_t xmaxc, int *exp_out, int *mant_out)
{
	int exp = 0;
	int mant;

	if (xmaxc > 15) exp = cl_floor_shift(xmaxc, 3) - 1;
	mant = xmaxc - (exp << 3);
	if (mant == 0) {
		exp = -4;
		mant = 7;
	} else {
		while (mant <= 7) {
			mant = (mant << 1) | 1;
			exp--;
		}
		mant -= 8;
	}
	*exp_out = exp;
	*mant_out = mant;
}

static void cl_apcm_quantise(const int16_t *xM, struct cl_rpe *out)
{
	static const int16_t NRFAC[8] = {
		29128, 26215, 23832, 21846, 20165, 18725, 17476, 16384
	};
	int i, itest, exp, mant, temp;
	int32_t xmax = 0;

	for (i = 0; i <= 12; i++) {
		int32_t a = xM[i] < 0 ? -(int32_t)xM[i] : (int32_t)xM[i];
		if (xM[i] == MIN_WORD) a = MAX_WORD;
		if (a > xmax) xmax = a;
	}

	exp = 0;
	itest = 0;
	temp = cl_floor_shift(xmax, 9);
	for (i = 0; i <= 5; i++) {
		itest |= (temp <= 0);
		temp = cl_floor_shift(temp, 1);
		if (itest == 0) exp++;
	}
	temp = exp + 5;
	out->xmaxc = cl_add((int16_t)cl_floor_shift(xmax, temp),
			    (int16_t)(exp << 3));

	cl_decode_xmaxc(out->xmaxc, &exp, &mant);
	out->exp = exp;
	out->mant = mant;

	{
		int shift = 6 - exp;
		int16_t nrf = NRFAC[mant];
		for (i = 0; i <= 12; i++) {
			int16_t t = (int16_t)((int32_t)xM[i] << shift);
			int16_t q = cl_mul_q15(t, nrf);
			out->xMc[i] = (int16_t)(cl_floor_shift(q, 12) + 4);
		}
	}
}

static void cl_apcm_inverse(const struct cl_rpe *in, int16_t *xMp)
{
	static const int16_t FAC[8] = {
		18431, 20479, 22527, 24575, 26623, 28671, 30719, 32767
	};
	int i;
	int shift = 6 - in->exp;
	int16_t fac = FAC[in->mant];
	int16_t half = (int16_t)(1 << (shift - 1));

	for (i = 0; i < 13; i++) {
		int16_t s = (int16_t)((in->xMc[i] << 1) - 7);
		int16_t t = (int16_t)((int32_t)s << 12);
		int16_t m = cl_mul_r15(fac, t);
		int16_t a = cl_add(m, half);
		xMp[i] = (int16_t)cl_floor_shift(a, shift);
	}
}

/*
 * Full RPE model for one subframe.  e40 is the long term residual; the
 * reconstructed sparse residual is written to out->ep.
 */
static void cl_rpe_model(const int16_t *e40, struct cl_rpe *out)
{
	int i, mc;

	cl_weighting(e40, out->x);
	cl_grid_select(out->x, &mc, out->energy);
	out->Mc = mc;
	for (i = 0; i <= 12; i++) out->xM[i] = (int16_t)out->x[mc + 3 * i];
	cl_apcm_quantise(out->xM, out);
	cl_apcm_inverse(out, out->xMp);

	for (i = 0; i < 40; i++) out->ep[i] = 0;
	for (i = 0; i <= 12; i++) out->ep[mc + 3 * i] = out->xMp[i];
}

/*
 * Compare the model against Gsm_RPE_Encoding, one field and one sample
 * at a time.  Returns the number of mismatches (0 on success).  The
 * caller's e[-5..-1] and e[40..44] padding is zeroed here; the model
 * keeps its own copy of the 40 reconstructed samples in ref->ep.
 */
static int cl_rpe_check(const char *tag, const int16_t *e40,
			struct cl_rpe *ref)
{
	struct gsm_state S;
	word buf[50], xmaxc, Mc, xMc[13];
	int i, bad = 0;

	memset(&S, 0, sizeof(S));
	memset(buf, 0, sizeof(buf));
	memcpy(buf + 5, e40, 40 * sizeof(word));
	cl_rpe_model(e40, ref);

	Gsm_RPE_Encoding(&S, buf + 5, &xmaxc, &Mc, xMc);

	if (ref->Mc != (int)Mc) {
		cl_mismatch("PHASE rpe %s sub=0 field=Mc model=%d lib=%d\n",
			    tag, ref->Mc, (int)Mc);
		bad++;
	}
	if (ref->xmaxc != (int)xmaxc) {
		cl_mismatch("PHASE rpe %s sub=0 field=xmaxc model=%d lib=%d\n",
			    tag, ref->xmaxc, (int)xmaxc);
		bad++;
	}
	for (i = 0; i < 13; i++) {
		if (ref->xMc[i] != (int)xMc[i]) {
			cl_mismatch("PHASE rpe %s sub=0 field=xMc[%d] model=%d lib=%d\n",
				    tag, i, ref->xMc[i], (int)xMc[i]);
			bad++;
		}
	}
	for (i = 0; i < 40; i++) {
		if (ref->ep[i] != (int)buf[5 + i]) {
			cl_mismatch("PHASE rpe %s sub=0 field=ep[%d] model=%d lib=%d\n",
				    tag, i, ref->ep[i], (int)buf[5 + i]);
			bad++;
		}
	}
	/* One recorded check per field and per sample. */
	TASK_CHECK(ref->Mc == (int)Mc);
	TASK_CHECK(ref->xmaxc == (int)xmaxc);
	for (i = 0; i < 13; i++) TASK_CHECK(ref->xMc[i] == (int)xMc[i]);
	for (i = 0; i < 40; i++) TASK_CHECK(ref->ep[i] == (int)buf[5 + i]);
	return bad;
}

/* ------------------------------------------------------------------ */
/* Closed loop reference                                               */
/* ------------------------------------------------------------------ */

struct cl_loop {
	gsm	front;		/* separate context: preprocess/LPC/analysis only */
	int16_t	dp0[280];	/* mirror of S->dp0                         */
	word	LARc[8];
	word	Nc[4], bc[4], Mc[4], xmaxc[4];
	word	xMc[52];
	int16_t	resid[160];	/* short term residual of the last frame      */
};

static void cl_loop_init(struct cl_loop *L)
{
	memset(L, 0, sizeof(*L));
	L->front = gsm_create();
}

static void cl_loop_close(struct cl_loop *L)
{
	if (L->front) gsm_destroy(L->front);
	L->front = 0;
}

/*
 * One frame of the model loop.
 *
 * Preprocess, LPC analysis and short term analysis are taken from a
 * separate context, since this suite does not model them.  Everything
 * afterwards is advanced by the model itself: the LTP history receives
 * the *reconstructed* residual (RPE output + LTP prediction), never the
 * original short term residual, and only the final 120 reconstructed
 * samples of the frame survive into the next frame.
 */
static void cl_loop_frame(struct cl_loop *L, const int16_t *s)
{
	int16_t so[160];
	word *dp = L->dp0 + 120;
	int k, i;

	memcpy(so, s, sizeof(so));
	Gsm_Preprocess((struct gsm_state *)L->front, (word *)so, (word *)so);
	Gsm_LPC_Analysis((struct gsm_state *)L->front, (word *)so, L->LARc);
	Gsm_Short_Term_Analysis_Filter((struct gsm_state *)L->front,
				       L->LARc, (word *)so);
	memcpy(L->resid, so, sizeof(so));

	for (k = 0; k < 4; k++) {
		struct cl_ltp lt;
		struct cl_rpe rp;
		int16_t d[40];
		for (i = 0; i < 40; i++) d[i] = so[k * 40 + i];

		cl_ltp_model(d, dp, &lt);
		cl_rpe_model(lt.e, &rp);

		L->Nc[k]    = (word)lt.Nc;
		L->bc[k]    = (word)lt.bc;
		L->Mc[k]    = (word)rp.Mc;
		L->xmaxc[k] = rp.xmaxc;
		for (i = 0; i < 13; i++) L->xMc[k * 13 + i] = rp.xMc[i];

		for (i = 0; i < 40; i++)
			dp[i] = cl_add(rp.ep[i], lt.dpp[i]);
		dp += 40;
	}

	/* Only the last 120 reconstructed samples remain history. */
	memcpy(L->dp0, L->dp0 + 160, 120 * sizeof(int16_t));
}

/* Compare each model field against what Gsm_Coder produced for a frame. */
static int cl_loop_compare(const char *tag, int frame,
			   const struct cl_loop *ref,
			   const word *LARc,
			   const word *Nc, const word *bc, const word *Mc,
			   const word *xmaxc, const word *xMc,
			   const word *dp0)
{
	int k, i, mism = 0;

	for (i = 0; i < 8; i++) {
		if (LARc[i] != ref->LARc[i]) {
			cl_mismatch("PHASE loop %s frame=%d field=LARc[%d] model=%d lib=%d\n",
				    tag, frame, i, ref->LARc[i], LARc[i]);
			mism++;
		}
	}
	for (k = 0; k < 4; k++) {
		if (Nc[k] != ref->Nc[k]) {
			cl_mismatch("PHASE loop %s frame=%d sub=%d field=Nc model=%d lib=%d\n",
				    tag, frame, k, ref->Nc[k], Nc[k]);
			mism++;
		}
		if (bc[k] != ref->bc[k]) {
			cl_mismatch("PHASE loop %s frame=%d sub=%d field=bc model=%d lib=%d\n",
				    tag, frame, k, ref->bc[k], bc[k]);
			mism++;
		}
		if (Mc[k] != ref->Mc[k]) {
			cl_mismatch("PHASE loop %s frame=%d sub=%d field=Mc model=%d lib=%d\n",
				    tag, frame, k, ref->Mc[k], Mc[k]);
			mism++;
		}
		if (xmaxc[k] != ref->xmaxc[k]) {
			cl_mismatch("PHASE loop %s frame=%d sub=%d field=xmaxc model=%d lib=%d\n",
				    tag, frame, k, ref->xmaxc[k], xmaxc[k]);
			mism++;
		}
		for (i = 0; i < 13; i++) {
			if (xMc[k * 13 + i] != ref->xMc[k * 13 + i]) {
				cl_mismatch("PHASE loop %s frame=%d sub=%d field=xMc[%d] model=%d lib=%d\n",
					    tag, frame, k, i,
					    ref->xMc[k * 13 + i], xMc[k * 13 + i]);
				mism++;
				break;
			}
		}
	}
	for (i = 0; i < 280; i++) {
		if (dp0[i] != ref->dp0[i]) {
			cl_mismatch("PHASE loop %s frame=%d field=dp0[%d] model=%d lib=%d\n",
				    tag, frame, i, ref->dp0[i], dp0[i]);
			mism++;
			break;
		}
	}
	/* One recorded check per field and per sample. */
	for (i = 0; i < 8; i++) TASK_CHECK(ref->LARc[i] == LARc[i]);
	for (k = 0; k < 4; k++) {
		TASK_CHECK(ref->Nc[k] == Nc[k]);
		TASK_CHECK(ref->bc[k] == bc[k]);
		TASK_CHECK(ref->Mc[k] == Mc[k]);
		TASK_CHECK(ref->xmaxc[k] == xmaxc[k]);
		for (i = 0; i < 13; i++)
			TASK_CHECK(ref->xMc[k * 13 + i] == xMc[k * 13 + i]);
	}
	for (i = 0; i < 280; i++) TASK_CHECK(ref->dp0[i] == dp0[i]);
	if (mism) return 0;
	return 1;
}

/* ------------------------------------------------------------------ */
/* Deterministic PCM generators                                        */
/* ------------------------------------------------------------------ */

/*
 * Signal A: four frames of an alternating plateau with a small
 * superimposed pattern, then an abrupt aperiodic change at frame 4, then
 * a slower irregular pattern.  The frame 4 change forces a different
 * LTP lag and gain after a periodic run.
 */
static int16_t cl_pcm_a(int frame, int i)
{
	int32_t v;

	if (frame < 4) {
		int p = i % 40;
		v = (p < 20 ? 6000 : -6000);
		v += 100 * ((p * 7 + frame * 13) % 17) - 800;
	} else if (frame == 4) {
		v = (i * 197 - 12000) % 32768;
		if (v < 0) v += 32768;
		v -= 16384;
	} else {
		v = 3000 * ((i % 7) - 3) + 500 * ((i + frame * 37) % 11) - 2500;
	}

	if (v < -30000) v = -30000;
	if (v >  30000) v =  30000;
	return (int16_t)v;
}

/* Signal B: a completely different irregular signal. */
static int16_t cl_pcm_b(int frame, int i)
{
	int32_t v = ((i * 13 + frame * 211) % 1000 - 500) * 40
		  + ((i % 3) - 1) * 700;
	if (v < -30000) v = -30000;
	if (v >  30000) v =  30000;
	return (int16_t)v;
}

/* ------------------------------------------------------------------ */
/* Case fixtures                                                       */
/* ------------------------------------------------------------------ */

/* ltp group: 40 sample residual plus 120 sample reconstructed history */

static const int16_t ltp_period_d[40] = {
	-400, -257, -114, -271, -128, -285, -142, 1, -156, -13,
	-400, -257, -114, -271, -128, -285, -142, 1, -156, -13,
	-400, -257, -114, -271, -128, -285, -142, 1, -156, -13,
	-400, -257, -114, -271, -128, -285, -142, 1, -156, -13
};
static const int16_t ltp_period_h[120] = {
	-400, -257, -114, -271, -128, -285, -142, 1, -156, -13,
	-400, -257, -114, -271, -128, -285, -142, 1, -156, -13,
	-400, -257, -114, -271, -128, -285, -142, 1, -156, -13,
	-400, -257, -114, -271, -128, -285, -142, 1, -156, -13,
	-400, -257, -114, -271, -128, -285, -142, 1, -156, -13,
	-400, -257, -114, -271, -128, -285, -142, 1, -156, -13,
	-400, -257, -114, -271, -128, -285, -142, 1, -156, -13,
	-400, -257, -114, -271, -128, -285, -142, 1, -156, -13,
	-400, -257, -114, -271, -128, -285, -142, 1, -156, -13,
	-400, -257, -114, -271, -128, -285, -142, 1, -156, -13,
	-400, -257, -114, -271, -128, -285, -142, 1, -156, -13,
	-400, -257, -114, -271, -128, -285, -142, 1, -156, -13
};

static const int16_t ltp_nonpos_d[40] = {
	1, 38, 75, 112, 149, 186, 223, 260, 297, 334,
	371, 408, 445, 482, 19, 56, 93, 130, 167, 204,
	241, 278, 315, 352, 389, 426, 463, 500, 37, 74,
	111, 148, 185, 222, 259, 296, 333, 370, 407, 444
};
static const int16_t ltp_nonpos_h[120] = {
	-1, -14, -27, -40, -53, -66, -79, -92, -105, -118,
	-131, -144, -157, -170, -183, -196, -209, -222, -235, -248,
	-261, -274, -287, -300, -313, -326, -339, -352, -365, -378,
	-391, -404, -417, -430, -443, -456, -469, -482, -495, -508,
	-521, -534, -547, -560, -573, -586, -599, -612, -625, -638,
	-651, -664, -677, -690, -703, -716, -729, -742, -755, -768,
	-781, -794, -807, -820, -833, -846, -859, -872, -885, -898,
	-11, -24, -37, -50, -63, -76, -89, -102, -115, -128,
	-141, -154, -167, -180, -193, -206, -219, -232, -245, -258,
	-271, -284, -297, -310, -323, -336, -349, -362, -375, -388,
	-401, -414, -427, -440, -453, -466, -479, -492, -505, -518,
	-531, -544, -557, -570, -583, -596, -609, -622, -635, -648
};

static const int16_t ltp_base_d[40] = {
	-400, -13, 374, 11, 398, 785, 422, -150, 237, -126,
	261, 648, 285, 672, 100, -263, 124, 511, 148, 535,
	922, -400, -13, 374, 11, 398, 785, 422, -150, 237,
	-126, 261, 648, 285, 672, 100, -263, 124, 511, 148
};

/* History blocks used by the four gain levels; only the last 40 samples
 * (positions 80..119) are nonzero.  Multiply the base by num/den. */
static const int16_t gain0_h[120] = {
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, -2400, -78, 2244, 66,
	2388, 4710, 2532, -900, 1422, -756, 1566, 3888,
	1710, 4032, 600, -1578, 744, 3066, 888, 3210,
	5532, -2400, -78, 2244, 66, 2388, 4710, 2532,
	-900, 1422, -756, 1566, 3888, 1710, 4032, 600,
	-1578, 744, 3066, 888
};
static const int16_t gain1_h[120] = {
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, -1200, -39, 1122, 33,
	1194, 2355, 1266, -450, 711, -378, 783, 1944,
	855, 2016, 300, -789, 372, 1533, 444, 1605,
	2766, -1200, -39, 1122, 33, 1194, 2355, 1266,
	-450, 711, -378, 783, 1944, 855, 2016, 300,
	-789, 372, 1533, 444
};
static const int16_t gain2_h[120] = {
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, -800, -26, 748, 22,
	796, 1570, 844, -300, 474, -252, 522, 1296,
	570, 1344, 200, -526, 248, 1022, 296, 1070,
	1844, -800, -26, 748, 22, 796, 1570, 844,
	-300, 474, -252, 522, 1296, 570, 1344, 200,
	-526, 248, 1022, 296
};
static const int16_t gain3_h[120] = {
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, -400, -13, 374, 11,
	398, 785, 422, -150, 237, -126, 261, 648,
	285, 672, 100, -263, 124, 511, 148, 535,
	922, -400, -13, 374, 11, 398, 785, 422,
	-150, 237, -126, 261, 648, 285, 672, 100,
	-263, 124, 511, 148
};

/* History where lag 60 is an exact copy of d: exercises scal 5. */
static const int16_t ltp_scal5_d[40] = {
	2345, 3322, 4299, 5276, 6253, 7230, 8207, 9184,
	-9840, -8863, -7886, -6909, -5932, -4955, -3978, -3001,
	-2024, -1047, -70, 907, 1884, 2861, 3838, 4815,
	5792, 6769, 7746, 8723, 9700, -9324, -8347, -7370,
	-6393, -5416, -4439, -3462, -2485, -1508, -531, 446
};
static const int16_t ltp_scal5_h[120] = {
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	2345, 3322, 4299, 5276, 6253, 7230, 8207, 9184, -9840, -8863, -7886, -6909,
	-5932, -4955, -3978, -3001, -2024, -1047, -70, 907, 1884, 2861, 3838, 4815,
	5792, 6769, 7746, 8723, 9700, -9324, -8347, -7370, -6393, -5416, -4439, -3462,
	-2485, -1508, -531, 446, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

/* History where lag 73 is an exact copy of d: exercises scal 6 and a
 * non trivial selected lag away from the boundaries. */
static const int16_t ltp_scal6_d[40] = {
	7035, 9864, 12693, 15522, 18351, 21180, 24009,
	26838, -29667, -32496, 30211, 27382, 24553, 21724,
	18895, 16066, 13237, 10408, 7579, 4750, 1921, -908,
	-3737, -6566, -9395, -12224, -15053, -17882, -20711,
	23540, 26369, 29198, 32027, -30680, -27851, -25022,
	-22193, -19364, -16535, -13706
};
static const int16_t ltp_scal6_h[120] = {
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 7035,
	9864, 12693, 15522, 18351, 21180, 24009, 26838, -29667, -32496, 30211, 27382, 24553,
	21724, 18895, 16066, 13237, 10408, 7579, 4750, 1921, -908, -3737, -6566, -9395,
	-12224, -15053, -17882, -20711, 23540, 26369, 29198, 32027, -30680, -27851, -25022, -22193,
	-19364, -16535, -13706, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

/* rpe group: 40 sample residual, zero padded by five on both sides */

static const int16_t rpe_ep0_e[40] = {
	879, 1070, 1300, 88, 1274, -961, 1046, 969, 1114,
	-23, -183, -1468, -701, 613, 782, 1060, 1545, -322,
	-1442, 491, -578, -1124, 640, -965, -703, -142, 1510,
	-1324, 778, 1142, -1185, -539, 1228, 1494, -115, -1153,
	-749, 256, 544, -992
};
static const int16_t rpe_ep3_e[40] = {
	1113, -842, 702, 181, 1173, -1643, 168, -457, -946,
	-1634, -318, 407, -1563, -1180, -139, -740, 171, -775,
	-404, -621, 1174, 942, -1016, 1509, 1426, 677, -707,
	881, 138, 1145, -1011, 1340, -470, -1067, 781, 1077,
	413, 58, 1249, 1272
};
static const int16_t rpe_tie_e[40] = {
	-14, -90, -42, 78, -30, 40, 2, 20, 92, 55,
	33, -20, -13, 42, -46, -70, -17, -57, -90, 19,
	11, 33, 7, 16, -38, 30, 72, -8, 77, -31,
	-42, -21, -68, 51, -48, 81, 63, 3, -6, 97
};
static const int16_t rpe_negodd_e[40] = {
	538, 3434, -2781, 5789, -6425, 4963, -4630, 2715,
	1805, -3966, -2197, -3904, -4786, -2705, 5820, 5416,
	-1903, 5912, 1124, 3386, -2910, 2100, -161, 3654,
	5218, 496, 3072, -5630, -2173, 5930, -4292, -5378,
	-5385, -3775, 3693, -5542, -800, 5946, 6329, -3317
};
static const int16_t rpe_small_e[40] = {
	39, -12, -13, 45, -19, -76, -95, 82, -48, 44,
	24, -19, -14, 82, 51, 47, 60, 44, 5, 17,
	87, 19, -17, 86, 44, -31, 53, -72, -66, -25,
	11, -44, 51, 40, 65, 83, 1, 9, 79, -27
};
static const int16_t rpe_zero_e[40] = {
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};
static const int16_t rpe_sw15_e[40] = {
	-152, -155, -75, -221, 50, 130, -34, -8, -122, 85,
	123, 191, -13, -26, 11, 163, -221, -17, 185, 159,
	113, 154, 186, 200, 177, 179, -120, 129, -141, 191,
	66, -34, -92, 15, -141, 11, -30, 21, 44, 35
};
static const int16_t rpe_sw16_e[40] = {
	-194, -76, -98, -42, 300, -135, -290, -185, 191, -131,
	-305, 196, -32, -107, -287, 5, -95, 182, -258, -168,
	264, 218, 83, -16, 278, -108, 242, -70, -254, 47,
	276, 289, 202, -236, -91, 253, -231, 227, 192, 101
};

/* ------------------------------------------------------------------ */
/* ltp cases                                                           */
/* ------------------------------------------------------------------ */

/* Tracks how many of the "must be hit" ltp situations each case
 * actually exercises, so that a silently degrading fixture shows up. */
static int ltp_hits_scaled;	/* scal > 0, i.e. scaling applied            */
static int ltp_hits_unscaled;	/* scal == 0, the "no scaling" branch        */
static int ltp_hits_tie;	/* repeated period produced a positive tie  */
static int ltp_hits_nonpos;	/* all correlations non-positive            */
static int ltp_hits_levels[4];	/* which gain levels (bc) appeared         */

/* Mid level case helper: 0 on success, 1 on failure (like run()). */
static int cl_ltp_case(const char *tag, const int16_t *d,
		       const int16_t *hist, int want_level)
{
	struct cl_ltp lt;
	int fail = 0;

	if (cl_ltp_check(tag, d, hist, &lt) != 0) fail = 1;
	if (lt.scal > 0) ltp_hits_scaled++;
	if (lt.scal == 0) ltp_hits_unscaled++;
	if (lt.ties > 1 && lt.max_corr > 0) ltp_hits_tie++;
	if (lt.all_nonpos) ltp_hits_nonpos++;
	if (lt.bc >= 0 && lt.bc < 4) ltp_hits_levels[lt.bc]++;

	if (want_level >= 0 && lt.bc != want_level) {
		cl_mismatch("PHASE ltp %s sub=0 field=bc model=%d expected=%d\n",
			    tag, lt.bc, want_level);
		fail = 1;
	}

	/* Independent checks that are not just "lag is in range". */
	if (lt.Nc < 40 || lt.Nc > 120) {
		fprintf(stderr,
			"PHASE ltp %s sub=0 field=Nc range model=%d\n",
			tag, lt.Nc);
		fail = 1;
	}
	if (lt.all_nonpos && (lt.Nc != 40 || lt.bc != 0)) {
		fprintf(stderr,
			"PHASE ltp %s sub=0 field=nonpos Nc=%d bc=%d\n",
			tag, lt.Nc, lt.bc);
		fail = 1;
	}
	if (lt.ties > 1 && lt.max_corr > 0) {
		int lam, k;
		int first = 40;
		for (lam = 40; lam <= 120; lam++) {
			int32_t acc = 0;
			for (k = 0; k < 40; k++)
				acc += (int32_t)cl_floor_shift(d[k], lt.scal) *
				       (int32_t)hist[k - lam];
			if (acc == lt.max_corr) { first = lam; break; }
		}
		if (first != lt.Nc) {
			fprintf(stderr,
				"PHASE ltp %s sub=0 field=tie-first model=%d lib=%d\n",
				tag, lt.Nc, first);
			fail = 1;
		}
	}
	if (fail) {
		TASK_CHECK(0);
		return 1;
	}
	TASK_CHECK(1);
	return 0;
}

static int case_ltp_scaling_switch(void)
{
	/* One vector is scaled with scal 5 and selects lag 60; the other is
	 * scaled with the maximum scal 6 and selects lag 73.  Requiring the
	 * exact lags and distinct scales shows the scaling search ran and
	 * that each lag was found from the scaled residual.
	 */
	struct cl_ltp a, b;

	if (cl_ltp_case("scal5", ltp_scal5_d, ltp_scal5_h + 120, 3)) return 1;
	if (cl_ltp_case("scal6", ltp_scal6_d, ltp_scal6_h + 120, 3)) return 1;
	cl_ltp_model(ltp_scal5_d, ltp_scal5_h + 120, &a);
	cl_ltp_model(ltp_scal6_d, ltp_scal6_h + 120, &b);
	TASK_CHECK(a.scal == 5);
	TASK_CHECK(b.scal == 6);
	TASK_CHECK(a.scal != b.scal);
	TASK_CHECK(a.Nc == 60);
	TASK_CHECK(b.Nc == 73);
	return 0;
}

static int case_ltp_period_tie(void)
{
	if (cl_ltp_case("period", ltp_period_d, ltp_period_h + 120, 3)) return 1;
	{
		struct cl_ltp lt;
		int lam, k;
		int strict_first = 40;
		int loose_last = 40;
		int32_t best = -1;

		cl_ltp_model(ltp_period_d, ltp_period_h + 120, &lt);
		/* period 10 leaves 40, 60, 80, 100, 120 identical. */
		TASK_CHECK(lt.Nc == 40);
		TASK_CHECK(lt.ties >= 2);

		/*
		 * The strict scan keeps the first lag that meets the maximum;
		 * a ">=" scan would keep the last.  Require the two to differ,
		 * which is exactly what the LTP mutant inverts.
		 */
		for (lam = 40; lam <= 120; lam++) {
			int32_t acc = 0;
			for (k = 0; k < 40; k++)
				acc += (int32_t)cl_floor_shift(ltp_period_d[k], lt.scal) *
				       (int32_t)ltp_period_h[120 + k - lam];
			if (acc > best) {
				best = acc;
				strict_first = lam;
			}
		}
		best = -1;
		for (lam = 40; lam <= 120; lam++) {
			int32_t acc = 0;
			for (k = 0; k < 40; k++)
				acc += (int32_t)cl_floor_shift(ltp_period_d[k], lt.scal) *
				       (int32_t)ltp_period_h[120 + k - lam];
			if (acc >= best) {
				best = acc;
				loose_last = lam;
			}
		}
		TASK_CHECK(strict_first == lt.Nc);
		TASK_CHECK(loose_last != strict_first);
	}
	return 0;
}

static int case_ltp_all_nonpositive(void)
{
	if (cl_ltp_case("nonpositive", ltp_nonpos_d, ltp_nonpos_h + 120, 0))
		return 1;
	{
		struct cl_ltp lt;
		cl_ltp_model(ltp_nonpos_d, ltp_nonpos_h + 120, &lt);
		TASK_CHECK(lt.all_nonpos);
		TASK_CHECK(lt.Nc == 40);
	}
	return 0;
}

static int case_ltp_gain_levels(void)
{
	if (cl_ltp_case("gain0", ltp_base_d, gain0_h + 120, 0)) return 1;
	if (cl_ltp_case("gain1", ltp_base_d, gain1_h + 120, 1)) return 1;
	if (cl_ltp_case("gain2", ltp_base_d, gain2_h + 120, 2)) return 1;
	if (cl_ltp_case("gain3", ltp_base_d, gain3_h + 120, 3)) return 1;
	return 0;
}

static int case_ltp_report(void)
{
	int levels = 0, i, registered = 0;

	for (i = 0; i < 4; i++) if (ltp_hits_levels[i]) levels++;
	printf("LTP_HITS scaled=%d unscaled=%d tie=%d nonpositive=%d levels=%d\n",
	       ltp_hits_scaled, ltp_hits_unscaled, ltp_hits_tie,
	       ltp_hits_nonpos, levels);
	TASK_CHECK(ltp_hits_scaled >= 1);
	TASK_CHECK(ltp_hits_unscaled >= 1);
	TASK_CHECK(ltp_hits_tie >= 1);
	TASK_CHECK(ltp_hits_nonpos >= 1);
	TASK_CHECK(levels >= 3);
	for (i = 0; i < 4; i++) if (ltp_hits_levels[i]) registered++;
	TASK_CHECK(registered >= 3);
	return 0;
}

/* ------------------------------------------------------------------ */
/* rpe cases                                                           */
/* ------------------------------------------------------------------ */

static int rpe_hits_endpoint_flip;
static int rpe_hits_tie;
static int rpe_hits_zero;
static int rpe_hits_negative_odd;
static int rpe_hits_exp_switch;

/* Mid level case helper: 0 on success, 1 on failure (like run()). */
static int cl_rpe_case(const char *tag, const int16_t *e40)
{
	struct cl_rpe rp;
	int i, fail = 0;

	if (cl_rpe_check(tag, e40, &rp) != 0) fail = 1;

	/* Range and structural invariants of the model output itself. */
	TASK_CHECK(rp.Mc >= 0 && rp.Mc <= 3);
	TASK_CHECK(rp.xmaxc >= 0 && rp.xmaxc <= 63);
	for (i = 0; i < 13; i++)
		TASK_CHECK(rp.xMc[i] >= 0 && rp.xMc[i] <= 7);
	for (i = 0; i < 40; i++) {
		if (i >= rp.Mc && i <= rp.Mc + 36 && (i - rp.Mc) % 3 == 0) {
			int j = (i - rp.Mc) / 3;
			TASK_CHECK(j <= 12);
			TASK_CHECK(rp.ep[i] == rp.xMp[j]);
		} else {
			TASK_CHECK(rp.ep[i] == 0);
		}
	}
	if (fail) TASK_CHECK(0);
	return fail;
}

static int case_rpe_endpoint(void)
{
	if (cl_rpe_case("ep0", rpe_ep0_e)) return 1;
	{
		struct cl_rpe rp;
		int32_t e0_without_first;
		cl_rpe_model(rpe_ep0_e, &rp);
		TASK_CHECK(rp.Mc == 0);
		TASK_CHECK(rp.energy[0] > rp.energy[3]);
		e0_without_first = rp.energy[0]
			- (int32_t)cl_floor_shift(rp.x[0], 2) *
			  (int32_t)cl_floor_shift(rp.x[0], 2);
		TASK_CHECK(e0_without_first <= rp.energy[3]);
		rpe_hits_endpoint_flip = 1;
	}

	if (cl_rpe_case("ep3", rpe_ep3_e)) return 1;
	{
		struct cl_rpe rp;
		int32_t e3_without_last;
		cl_rpe_model(rpe_ep3_e, &rp);
		TASK_CHECK(rp.Mc == 3);
		TASK_CHECK(rp.energy[3] > rp.energy[0]);
		e3_without_last = rp.energy[3]
			- (int32_t)cl_floor_shift(rp.x[39], 2) *
			  (int32_t)cl_floor_shift(rp.x[39], 2);
		TASK_CHECK(e3_without_last <= rp.energy[0]);
		rpe_hits_endpoint_flip = 1;
	}
	return 0;
}

static int case_rpe_tie(void)
{
	struct cl_rpe rp;

	if (cl_rpe_case("tie", rpe_tie_e)) return 1;
	cl_rpe_model(rpe_tie_e, &rp);
	TASK_CHECK(rp.energy[0] == rp.energy[3]);
	TASK_CHECK(rp.Mc == 0);
	rpe_hits_tie = 1;

	/* A direct selection tie between grids 1 and 2: equal energy, the
	 * smaller grid number must win. */
	{
		int32_t x[40];
		int32_t energy[4];
		int mc, i;
		static const int pat[13] = { 5, 9, 3, 7, 1, 8, 2, 6, 4, 10, 0, 11, 7 };
		memset(x, 0, sizeof(x));
		for (i = 0; i <= 12; i++) {
			x[1 + 3 * i] = pat[i] * 40;
			x[2 + 3 * i] = pat[i] * 40;
		}
		cl_grid_select(x, &mc, energy);
		TASK_CHECK(energy[1] == energy[2]);
		TASK_CHECK(mc == 1);
	}
	return 0;
}

static int case_rpe_zero(void)
{
	struct cl_rpe rp;

	if (cl_rpe_case("zero", rpe_zero_e)) return 1;
	cl_rpe_model(rpe_zero_e, &rp);
	TASK_CHECK(rp.xmaxc == 0);
	TASK_CHECK(rp.exp == -4);
	TASK_CHECK(rp.mant == 7);
	TASK_CHECK(rp.Mc == 0);
	rpe_hits_zero = 1;
	return 0;
}

static int case_rpe_exponent(void)
{
	struct cl_rpe rp;

	if (cl_rpe_case("small", rpe_small_e)) return 1;
	if (cl_rpe_case("negodd", rpe_negodd_e)) return 1;
	if (cl_rpe_case("sw15", rpe_sw15_e)) return 1;
	if (cl_rpe_case("sw16", rpe_sw16_e)) return 1;

	cl_rpe_model(rpe_negodd_e, &rp);
	{
		int i, any_neg_odd = 0;
		for (i = 0; i < 13; i++)
			if (rp.xMp[i] < 0 && (rp.xMp[i] & 1)) any_neg_odd = 1;
		TASK_CHECK(any_neg_odd);
		TASK_CHECK(rp.xmaxc > 15);
	}
	{
		struct cl_rpe a, b;
		cl_rpe_model(rpe_sw15_e, &a);
		cl_rpe_model(rpe_sw16_e, &b);
		TASK_CHECK(a.xmaxc == 15 && a.exp == 0 && a.mant == 7);
		TASK_CHECK(b.xmaxc == 16 && b.exp == 1 && b.mant == 0);
		TASK_CHECK(a.exp != b.exp);
	}
	rpe_hits_negative_odd = 1;
	rpe_hits_exp_switch = 1;
	return 0;
}

static int case_rpe_report(void)
{
	printf("RPE_HITS endpoint=%d tie=%d zero=%d negodd=%d expswitch=%d\n",
	       rpe_hits_endpoint_flip, rpe_hits_tie, rpe_hits_zero,
	       rpe_hits_negative_odd, rpe_hits_exp_switch);
	TASK_CHECK(rpe_hits_endpoint_flip >= 1);
	TASK_CHECK(rpe_hits_tie >= 1);
	TASK_CHECK(rpe_hits_zero >= 1);
	TASK_CHECK(rpe_hits_negative_odd >= 1);
	TASK_CHECK(rpe_hits_exp_switch >= 1);
	return 0;
}

/* ------------------------------------------------------------------ */
/* loop cases                                                          */
/* ------------------------------------------------------------------ */

struct cl_inst {
	gsm coder;		/* Gsm_Coder instance                         */
	gsm enc;		/* gsm_encode instance                        */
	struct cl_loop ref;	/* model instance, separate front end context */
};

static void cl_inst_open(struct cl_inst *I)
{
	memset(I, 0, sizeof(*I));
	I->coder = gsm_create();
	I->enc   = gsm_create();
	cl_loop_init(&I->ref);
}

static void cl_inst_shut(struct cl_inst *I)
{
	if (I->coder) gsm_destroy(I->coder);
	if (I->enc) gsm_destroy(I->enc);
	cl_loop_close(&I->ref);
}

/*
 * Run one frame through a single instance:
 *   - Gsm_Coder on its own context,
 *   - gsm_encode/gsm_explode on a second, independently advanced context,
 *   - the model loop on a third context used only for the front end.
 * All three field sets and the model's reconstructed history are
 * compared to the model.
 */
/* Run one frame; 1 on success, 0 on mismatch (mismatch already printed). */
static int cl_loop_run(struct cl_inst *I, const char *tag, int frame,
		       const int16_t *pcm)
{
	word LARc[8], Nc[4], bc[4], Mc[4], xmaxc[4], xMc[52];
	gsm_frame fr;
	gsm_signal fields[76];
	int k, fail = 0;

	Gsm_Coder((struct gsm_state *)I->coder, (word *)pcm,
		  LARc, Nc, bc, Mc, xmaxc, xMc);
	cl_loop_frame(&I->ref, pcm);
	if (!cl_loop_compare(tag, frame, &I->ref, LARc, Nc, bc, Mc, xmaxc, xMc,
			     ((struct gsm_state *)I->coder)->dp0))
		fail = 1;

	gsm_encode(I->enc, (gsm_signal *)pcm, fr);
	if (gsm_explode(I->enc, fr, fields) != 0) {
		cl_mismatch("PHASE loop %s frame=%d field=explode\n",
			    tag, frame);
		fail = 1;
	} else {
		/* Record every public field comparison with TASK_CHECK. */
		for (k = 0; k < 8; k++) {
			if (fields[k] != I->ref.LARc[k]) {
				cl_mismatch("PHASE loop %s frame=%d field=pubLARc[%d] model=%d lib=%d\n",
					    tag, frame, k, I->ref.LARc[k], fields[k]);
				fail = 1;
			}
			TASK_CHECK(fields[k] == I->ref.LARc[k]);
		}
		for (k = 0; k < 4; k++) {
			int base = 8 + k * 17;
			int j;
			if (fields[base + 0] != I->ref.Nc[k]) {
				cl_mismatch("PHASE loop %s frame=%d sub=%d field=pubNc model=%d lib=%d\n",
					    tag, frame, k, I->ref.Nc[k], fields[base + 0]);
				fail = 1;
			}
			if (fields[base + 1] != I->ref.bc[k]) {
				cl_mismatch("PHASE loop %s frame=%d sub=%d field=pubbc model=%d lib=%d\n",
					    tag, frame, k, I->ref.bc[k], fields[base + 1]);
				fail = 1;
			}
			if (fields[base + 2] != I->ref.Mc[k]) {
				cl_mismatch("PHASE loop %s frame=%d sub=%d field=pubMc model=%d lib=%d\n",
					    tag, frame, k, I->ref.Mc[k], fields[base + 2]);
				fail = 1;
			}
			if (fields[base + 3] != I->ref.xmaxc[k]) {
				cl_mismatch("PHASE loop %s frame=%d sub=%d field=pubxmaxc model=%d lib=%d\n",
					    tag, frame, k, I->ref.xmaxc[k], fields[base + 3]);
				fail = 1;
			}
			TASK_CHECK(fields[base + 0] == I->ref.Nc[k]);
			TASK_CHECK(fields[base + 1] == I->ref.bc[k]);
			TASK_CHECK(fields[base + 2] == I->ref.Mc[k]);
			TASK_CHECK(fields[base + 3] == I->ref.xmaxc[k]);
			for (j = 0; j < 13; j++) {
				if (fields[base + 4 + j] != I->ref.xMc[k * 13 + j]) {
					cl_mismatch("PHASE loop %s frame=%d sub=%d field=pubxMc[%d] model=%d lib=%d\n",
						    tag, frame, k, j,
						    I->ref.xMc[k * 13 + j],
						    fields[base + 4 + j]);
					fail = 1;
				}
				TASK_CHECK(fields[base + 4 + j] == I->ref.xMc[k * 13 + j]);
			}
		}
	}
	if (fail) return 0;
	return 1;
}

/* Feed one instance the PCM of signal A or B, for the given frame. */
static int cl_feed(struct cl_inst *I, const char *tag, int use_b, int frame)
{
	int16_t pcm[160];
	int i;

	for (i = 0; i < 160; i++)
		pcm[i] = use_b ? cl_pcm_b(frame, i) : cl_pcm_a(frame, i);
	return cl_loop_run(I, tag, frame, pcm);
}

static int case_loop_single(void)
{
	struct cl_inst I;
	int f;

	cl_inst_open(&I);
	if (!I.coder || !I.enc || !I.ref.front) return 1;
	for (f = 0; f < 7; f++) {
		TASK_CHECK(cl_feed(&I, "single", 0, f));
	}
	cl_inst_shut(&I);
	return 0;
}

static int case_loop_abrupt(void)
{
	struct cl_inst I;
	int f;

	/*
	 * Signal A is periodic for frames 0..3 and changes abruptly at
	 * frame 4.  Run eight frames to make sure the reconstructed
	 * history carries across the change.
	 */
	cl_inst_open(&I);
	if (!I.coder || !I.enc || !I.ref.front) return 1;
	for (f = 0; f < 8; f++) {
		TASK_CHECK(cl_feed(&I, "abrupt", 0, f));
	}

	/*
	 * Independently confirm that the abrupt change actually moved the
	 * LTP decision: run a model-only instance to frame 3 and to frame 4
	 * and require a different lag or gain.
	 */
	{
		struct cl_loop l;
		int16_t pcm[160];
		int i;
		word nc_before[4], bc_before[4], mc_before[4];
		word xmaxc_before[4];

		cl_loop_init(&l);
		for (f = 0; f < 4; f++) {
			for (i = 0; i < 160; i++) pcm[i] = cl_pcm_a(f, i);
			cl_loop_frame(&l, pcm);
		}
		for (i = 0; i < 4; i++) {
			nc_before[i]    = l.Nc[i];
			bc_before[i]    = l.bc[i];
			mc_before[i]    = l.Mc[i];
			xmaxc_before[i] = l.xmaxc[i];
		}
		for (i = 0; i < 160; i++) pcm[i] = cl_pcm_a(4, i);
		cl_loop_frame(&l, pcm);
		{
			int moved = 0;
			for (i = 0; i < 4; i++)
				if (l.Nc[i] != nc_before[i] ||
				    l.bc[i] != bc_before[i] ||
				    l.Mc[i] != mc_before[i] ||
				    l.xmaxc[i] != xmaxc_before[i])
					moved = 1;
			TASK_CHECK(moved);
		}
		cl_loop_close(&l);
	}
	cl_inst_shut(&I);
	return 0;
}

static int case_loop_interleaved(void)
{
	struct cl_inst A, B;
	int f;

	/* Two differently seeded instances must not influence each other. */
	cl_inst_open(&A);
	cl_inst_open(&B);
	if (!A.coder || !A.enc || !A.ref.front) return 1;
	if (!B.coder || !B.enc || !B.ref.front) return 1;
	for (f = 0; f < 7; f++) {
		TASK_CHECK(cl_feed(&A, "inter-A", 0, f));
		TASK_CHECK(cl_feed(&B, "inter-B", 1, f));
	}
	{
		/* The two histories must differ at the end of the run. */
		int i, differ = 0;
		for (i = 0; i < 280; i++)
			if (A.ref.dp0[i] != B.ref.dp0[i]) differ = 1;
		TASK_CHECK(differ);
	}
	cl_inst_shut(&A);
	cl_inst_shut(&B);
	return 0;
}

static int case_loop_history(void)
{
	struct cl_inst I;
	int f, i;
	int16_t pcm[160];

	/*
	 * Feed six frames of A; after each frame cross-check the model
	 * history against the coder context.  The model advances its own
	 * history from the reconstructed residual, so any feedback bug
	 * shows up here as well as in the field comparison.
	 */
	cl_inst_open(&I);
	if (!I.coder || !I.enc || !I.ref.front) return 1;
	for (f = 0; f < 6; f++) {
		for (i = 0; i < 160; i++) pcm[i] = cl_pcm_a(f, i);
		TASK_CHECK(cl_loop_run(&I, "history", f, pcm));
		{
			word *ctx = ((struct gsm_state *)I.coder)->dp0;
			for (i = 0; i < 280; i++) {
				if (ctx[i] != I.ref.dp0[i]) {
					cl_mismatch("PHASE loop history frame=%d field=dp0[%d] model=%d lib=%d\n",
						    f, i, I.ref.dp0[i], ctx[i]);
					return 1;
				}
			}
		}
	}
	cl_inst_shut(&I);
	return 0;
}

/* ------------------------------------------------------------------ */

static const struct task_case cl_cases[] = {
	/* ltp */
	{ "ltp", "scaling_switch",	case_ltp_scaling_switch },
	{ "ltp", "period_tie",		case_ltp_period_tie },
	{ "ltp", "all_nonpositive",	case_ltp_all_nonpositive },
	{ "ltp", "gain_levels",		case_ltp_gain_levels },
	{ "ltp", "report_hits",		case_ltp_report },
	/* rpe */
	{ "rpe", "endpoint",		case_rpe_endpoint },
	{ "rpe", "tie",			case_rpe_tie },
	{ "rpe", "zero",		case_rpe_zero },
	{ "rpe", "exponent",		case_rpe_exponent },
	{ "rpe", "report_hits",		case_rpe_report },
	/* loop */
	{ "loop", "single",		case_loop_single },
	{ "loop", "abrupt",		case_loop_abrupt },
	{ "loop", "interleaved",	case_loop_interleaved },
	{ "loop", "history",		case_loop_history }
};

const struct task_case task_cases[] = {
	cl_cases[0], cl_cases[1], cl_cases[2], cl_cases[3], cl_cases[4],
	cl_cases[5], cl_cases[6], cl_cases[7], cl_cases[8], cl_cases[9],
	cl_cases[10], cl_cases[11], cl_cases[12], cl_cases[13]
};

const size_t task_case_count =
	sizeof(task_cases) / sizeof(task_cases[0]);
