/*
 * Copyright 1992 by Jutta Degener and Carsten Bormann, Technische
 * Universitaet Berlin.  See the accompanying file "COPYRIGHT" for
 * details.  THERE IS ABSOLUTELY NO WARRANTY FOR THIS SOFTWARE.
 */

/*$Header: /tmp_amd/presto/export/kbs/jutta/src/gsm/RCS/private.h,v 1.6 1996/07/02 10:15:26 jutta Exp $*/

#ifndef	PRIVATE_H
#define	PRIVATE_H

typedef short			word;		/* 16 bit signed int	*/
typedef long			longword;	/* 32 bit signed int	*/

typedef unsigned short		uword;		/* unsigned word	*/
typedef unsigned long		ulongword;	/* unsigned longword	*/

struct gsm_state {

	word		dp0[ 280 ];
	word		e[ 50 ];	/* code.c 			*/

	word		z1;		/* preprocessing.c, Offset_com. */
	longword	L_z2;		/*                  Offset_com. */
	int		mp;		/*                  Preemphasis	*/

	word		u[8];		/* short_term_aly_filter.c	*/
	word		LARpp[2][8]; 	/*                              */
		word		j;		/*                              */

	word            ltp_cut;        /* long_term.c, LTP crosscorr.  */
	word		nrp; /* 40 */	/* long_term.c, synthesis	*/
	word		v[9];		/* short_term.c, synthesis	*/
	word		msr;		/* decoder.c,	Postprocessing	*/

	char		verbose;	/* only used if !NDEBUG		*/
	char		fast;		/* only used if FAST		*/

	char		wav_fmt;	/* only used if WAV49 defined	*/
	unsigned char	frame_index;	/*            odd/even chaining	*/
	unsigned char	frame_chain;	/*   half-byte to carry forward	*/
};


#define	MIN_WORD	(-32767 - 1)
#define	MAX_WORD	  32767

#define	MIN_LONGWORD	(-2147483647 - 1)
#define	MAX_LONGWORD	  2147483647

#include "proto.h"

/* ------------------------------------------------------------------ *
 *  Portable fixed point primitives (implemented in add.c).
 *
 *  Target: two's complement, 8 bit bytes, 16 bit short, 32 bit int,
 *  long 32 or 64 bits.  None of these rely on:
 *
 *	- long being exactly 32 bits,
 *	- left shifting a negative signed value,
 *	- right shifting a negative signed value (arithmetic shift),
 *	- signed integer overflow wrapping.
 *
 *  Four operation classes are deliberately kept separate:
 *
 *	saturated add/sub	clamp to the destination word range,
 *	arithmetic right shift	round toward -infinity (floor),
 *	truncating left shift	keep the destination word's low bits
 *				and re-interpret them signed,
 *	wide products		formed in a wider signed intermediate.
 * ------------------------------------------------------------------ */

/* Re-interpret the low 16 / 32 bits of an unsigned pattern as a
 * two's complement signed value, without assuming long's width.
 */
extern word	gsm_mk_word	P((unsigned u));
extern longword gsm_mk_long	P((ulongword u));

/* Truncating left shift (count in [0, width-1]): keep the low bits of
 * the destination word -- the deliberate low-bit truncation of the spec.
 */
extern word	gsm_sll_word	P((word a, int n));
extern longword gsm_sll_long	P((longword a, int n));

/* Arithmetic right shift, floor (toward -infinity).  For x < 0 the
 * identity floor(x/2^n) = ~((~x) >> n) only shifts non-negative values,
 * so no hardware arithmetic shift is assumed and it differs from C's
 * toward-zero division.
 */
extern word	gsm_sra_word	P((word a, int n));
extern longword gsm_sra_long	P((longword a, int n));

/* Q15 products with the exact truncation semantics of the historical
 * GSM_MULT / GSM_MULT_R / GSM_L_MULT macros (no MIN_WORD shortcut).
 */
extern word	gsm_mult_m	P((word a, word b));
extern word	gsm_mult_r_m	P((word a, word b));
extern longword gsm_L_mult_m	P((word a, word b));

/*
 * SASR keeps its historical name but no longer has two meanings:
 * regardless of the SASR build flag it is the portable floor
 * (-infinity) arithmetic right shift.  Type generic for word/longword;
 * every ``>>`` actually executed has a non-negative operand.
 */
#undef	SASR
#define	SASR(x, by)	((x) >= 0 ? (x) >> (by) : ~((~(x)) >> (by)))

/*
 *	Prototypes from add.c
 */
extern word	gsm_mult 	P((word a, word b));
extern longword gsm_L_mult 	P((word a, word b));
extern word	gsm_mult_r	P((word a, word b));

extern word	gsm_div  	P((word num, word denum));

extern word	gsm_add 	P(( word a, word b ));
extern longword gsm_L_add 	P(( longword a, longword b ));

extern word	gsm_sub 	P((word a, word b));
extern longword gsm_L_sub 	P((longword a, longword b));

extern word	gsm_abs 	P((word a));

extern word	gsm_norm 	P(( longword a ));

extern longword gsm_L_asl  	P((longword a, int n));
extern word	gsm_asl 	P((word a, int n));

extern longword gsm_L_asr  	P((longword a, int n));
extern word	gsm_asr  	P((word a, int n));

/*
 *  Inlined fixed point operations now dispatch to the portable add.c
 *  functions.  Products use a wide signed intermediate (16x16 cannot
 *  overflow a 32 bit value); shifts use the floor helpers and results
 *  are truncated to 16 or 32 bits.  Saturating add/sub apply the GSM
 *  clamp rule.
 */
#define GSM_MULT_R(a, b)	gsm_mult_r_m((a), (b))
#define GSM_MULT(a, b)		gsm_mult_m((a), (b))
#define GSM_L_MULT(a, b)	gsm_L_mult_m((a), (b))
#define GSM_L_ADD(a, b)		gsm_L_add((a), (b))
#define	GSM_ADD(a, b)		gsm_add((a), (b))
#define	GSM_SUB(a, b)		gsm_sub((a), (b))
#define	GSM_ABS(a)		gsm_abs((a))

/*
 *  More prototypes from implementations..
 */
extern void Gsm_Coder P((
		struct gsm_state	* S,
		word	* s,	/* [0..159] samples		IN	*/
		word	* LARc,	/* [0..7] LAR coefficients	OUT	*/
		word	* Nc,	/* [0..3] LTP lag		OUT 	*/
		word	* bc,	/* [0..3] coded LTP gain	OUT 	*/
		word	* Mc,	/* [0..3] RPE grid selection	OUT     */
		word	* xmaxc,/* [0..3] Coded maximum amplitude OUT	*/
		word	* xMc	/* [13*4] normalized RPE samples OUT	*/));

extern void Gsm_Long_Term_Predictor P((		/* 4x for 160 samples */
		struct gsm_state * S,
		word	* d,	/* [0..39]   residual signal	IN	*/
		word	* dp,	/* [-120..-1] d'		IN	*/
		word	* e,	/* [0..40] 			OUT	*/
		word	* dpp,	/* [0..40] 			OUT	*/
		word	* Nc,	/* correlation lag		OUT	*/
		word	* bc	/* gain factor			OUT	*/));

extern void Gsm_LPC_Analysis P((
		struct gsm_state * S,
		word * s,	 /* 0..159 signals	IN/OUT	*/
	        word * LARc));   /* 0..7   LARc's	OUT	*/

extern void Gsm_Preprocess P((
		struct gsm_state * S,
		word * s, word * so));

extern void Gsm_Encoding P((
		struct gsm_state * S,
		word	* e,
		word	* ep,
		word	* xmaxc,
		word	* Mc,
		word	* xMc));

extern void Gsm_Short_Term_Analysis_Filter P((
		struct gsm_state * S,
		word	* LARc,	/* coded log area ratio [0..7]  IN	*/
		word	* d	/* st res. signal [0..159]	IN/OUT	*/));

extern void Gsm_Decoder P((
		struct gsm_state * S,
		word	* LARcr,	/* [0..7]		IN	*/
		word	* Ncr,		/* [0..3] 		IN 	*/
		word	* bcr,		/* [0..3]		IN	*/
		word	* Mcr,		/* [0..3] 		IN 	*/
		word	* xmaxcr,	/* [0..3]		IN	*/
		word	* xMcr,		/* [0..13*4]		IN	*/
		word	* s));		/* [0..159]		OUT 	*/

extern void Gsm_Decoding P((
		struct gsm_state * S,
		word 	xmaxcr,
		word	Mcr,
		word	* xMcr,  	/* [0..12]		IN	*/
		word	* erp)); 	/* [0..39]		OUT 	*/

extern void Gsm_Long_Term_Synthesis_Filtering P((
		struct gsm_state* S,
		word	Ncr,
		word	bcr,
		word	* erp,		/* [0..39]		  IN 	*/
		word	* drp)); 	/* [-120..-1] IN, [0..40] OUT 	*/

void Gsm_RPE_Decoding P((
	struct gsm_state *S,
		word xmaxcr,
		word Mcr,
		word * xMcr,  /* [0..12], 3 bits             IN      */
		word * erp)); /* [0..39]                     OUT     */

void Gsm_RPE_Encoding P((
		struct gsm_state * S,
		word    * e,            /* -5..-1][0..39][40..44     IN/OUT  */
		word    * xmaxc,        /*                              OUT */
		word    * Mc,           /*                              OUT */
		word    * xMc));        /* [0..12]                      OUT */

extern void Gsm_Short_Term_Synthesis_Filter P((
		struct gsm_state * S,
		word	* LARcr, 	/* log area ratios [0..7]  IN	*/
		word	* wt,		/* received d [0...39]	   IN  */
		word	* s));		/* signal   s [0..159]	  OUT  */

extern void Gsm_Update_of_reconstructed_short_time_residual_signal P((
		word	* dpp,		/* [0...39]	IN		*/
		word	* ep,		/* [0...39]	IN		*/
		word	* dp));		/* [-120...-1]  IN/OUT 	*/

/*
 *  Tables from table.c
 */
#ifndef	GSM_TABLE_C

extern word gsm_A[8], gsm_B[8], gsm_MIC[8], gsm_MAC[8];
extern word gsm_INVA[8];
extern word gsm_DLB[4], gsm_QLB[4];
extern word gsm_H[11];
extern word gsm_NRFAC[8];
extern word gsm_FAC[8];

#endif	/* GSM_TABLE_C */

/*
 *  Debugging
 */
#ifdef NDEBUG

#	define	gsm_debug_words(a, b, c, d)		/* nil */
#	define	gsm_debug_longwords(a, b, c, d)		/* nil */
#	define	gsm_debug_word(a, b)			/* nil */
#	define	gsm_debug_longword(a, b)		/* nil */

#else	/* !NDEBUG => DEBUG */

	extern void  gsm_debug_words     P((char * name, int, int, word *));
	extern void  gsm_debug_longwords P((char * name, int, int, longword *));
	extern void  gsm_debug_longword  P((char * name, longword));
	extern void  gsm_debug_word      P((char * name, word));

#endif /* !NDEBUG */

#include "unproto.h"

#endif	/* PRIVATE_H */
