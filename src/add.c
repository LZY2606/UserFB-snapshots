/*
 * Copyright 1992 by Jutta Degener and Carsten Bormann, Technische
 * Universitaet Berlin.  See the accompanying file "COPYRIGHT" for
 * details.  THERE IS ABSOLUTELY NO WARRANTY FOR THIS SOFTWARE.
 */

/* $Header: /tmp_amd/presto/export/kbs/jutta/src/gsm/RCS/add.c,v 1.6 1996/07/02 09:57:33 jutta Exp $ */

/*
 *  See private.h for the more commonly used macro versions.
 *
 *  The saturation rules of the GSM primitives are unchanged; only the
 *  bit operations are expressed through the explicitly defined
 *  helpers in portable.h so that neither the width of "long" nor any
 *  signed shift direction is taken for granted.
 */

#include	<stdio.h>
#include	<assert.h>

#include	"portable.h"
#include	"private.h"
#include	"gsm.h"
#include	"proto.h"

#define	saturate(x) 	\
	((x) < GSM_MIN_WORD ? GSM_MIN_WORD : (x) > GSM_MAX_WORD ? GSM_MAX_WORD : (x))

word gsm_add P2((a,b), word a, word b)
{
	/*
	 * The sum of two 16 bit values always fits the 32 bit type,
	 * so this addition can never trap; only the final clamp
	 * reduces it to the word range.
	 */
	gsm_longword sum = (gsm_longword)a + (gsm_longword)b;
	return saturate(sum);
}

word gsm_sub P2((a,b), word a, word b)
{
	gsm_longword diff = (gsm_longword)a - (gsm_longword)b;
	return saturate(diff);
}

word gsm_mult P2((a,b), word a, word b)
{
	if (a == GSM_MIN_WORD && b == GSM_MIN_WORD) return GSM_MAX_WORD;
	else return gsm_q15_mult(a, b);
}

word gsm_mult_r P2((a,b), word a, word b)
{
	if (b == GSM_MIN_WORD && a == GSM_MIN_WORD) return GSM_MAX_WORD;
	else {
		/*
		 * Round-to-nearest Q15 multiply.  The product plus the
		 * 16384 bias stays inside the 32 bit range for every
		 * legal word pair, and the floor shift reproduces the
		 * historical arithmetic right shift result.
		 */
		gsm_longword prod = (gsm_longword)a * (gsm_longword)b + 16384;
		return (word)gsm_longword_asr(prod, 15);
	}
}

word gsm_abs P1((a), word a)
{
	/*
	 * Negating the minimum word would overflow; the GSM rule maps
	 * it to the positive saturation value instead.
	 */
	return a < 0 ? (a == GSM_MIN_WORD ? GSM_MAX_WORD : (word)-a) : a;
}

longword gsm_L_mult P2((a,b),word a, word b)
{
	assert( a != GSM_MIN_WORD || b != GSM_MIN_WORD );
	return gsm_q15_L_mult(a, b);
}

longword gsm_L_add P2((a,b), longword a, longword b)
{
	if (a < 0) {
		if (b >= 0) return a + b;
		else {
			/*
			 * Two negative operands: add the magnitudes in
			 * the unsigned 32 bit word so the arithmetic
			 * wraps modulo 2**32 exactly like the historical
			 * word operation, then apply the GSM saturation.
			 */
			gsm_ulongword A =
				(gsm_ulongword)(-(a + 1)) +
				(gsm_ulongword)(-(b + 1));
			return A >= (gsm_ulongword)GSM_MAX_LONGWORD
				? GSM_MIN_LONGWORD
				: gsm_longword_from_bits(
					(gsm_ulongword)(-(gsm_longword)A - 2));
		}
	}
	else if (b <= 0) return a + b;
	else {
		gsm_ulongword A = (gsm_ulongword)a + (gsm_ulongword)b;
		return A > (gsm_ulongword)GSM_MAX_LONGWORD
			? GSM_MAX_LONGWORD
			: (gsm_longword)A;
	}
}

longword gsm_L_sub P2((a,b), longword a, longword b)
{
	if (a >= 0) {
		if (b >= 0) return a - b;
		else {
			/* a>=0, b<0 */

			gsm_ulongword A = (gsm_ulongword)a +
					  (gsm_ulongword)(-(b + 1));
			return A >= (gsm_ulongword)GSM_MAX_LONGWORD
				? GSM_MAX_LONGWORD
				: gsm_longword_from_bits((gsm_ulongword)(A + 1));
		}
	}
	else if (b <= 0) return a - b;
	else {
		/* a<0, b>0 */

		gsm_ulongword A = (gsm_ulongword)(-(a + 1)) +
				  (gsm_ulongword)b;
		return A >= (gsm_ulongword)GSM_MAX_LONGWORD
			? GSM_MIN_LONGWORD
			: gsm_longword_from_bits(
				(gsm_ulongword)(-(gsm_longword)A - 1));
	}
}

static unsigned char const bitoff[ 256 ] = {
	 8, 7, 6, 6, 5, 5, 5, 5, 4, 4, 4, 4, 4, 4, 4, 4,
	 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
	 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
	 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
	 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
	 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
	 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
	 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
	 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

word gsm_norm P1((a), longword a )
/*
 * the number of left shifts needed to normalize the 32 bit
 * variable L_var1 for positive values on the interval
 *
 * with minimum of
 * minimum of 1073741824  (01000000000000000000000000000000) and
 * maximum of 2147483647  (01111111111111111111111111111111)
 *
 *
 * and for negative values on the interval with
 * minimum of -2147483648 (-10000000000000000000000000000000) and
 * maximum of -1073741824 ( -1000000000000000000000000000000).
 *
 * in order to normalize the result, the following
 * operation must be done: L_norm_var1 = L_var1 << norm( L_var1 );
 *
 * (That's 'ffs', only from the left, not the right..)
 */
{
	gsm_ulongword u;

	assert(a != 0);

	if (a < 0) {
		if (a <= -1073741824) return 0;
		u = ~(gsm_ulongword)a;
	}
	else {
		u = (gsm_ulongword)a;
	}

	/*
	 * Every shift below works on an unsigned word: for a negative
	 * argument we flipped the bits first, so the count depends only
	 * on the magnitude and never on a signed shift direction.
	 */
	return    u & 0xffff0000u
		? ( u & 0xff000000u
		  ?  -1 + bitoff[ 0xFF & (u >> 24) ]
		  :   7 + bitoff[ 0xFF & (u >> 16) ] )
		: ( u & 0xff00u
		  ?  15 + bitoff[ 0xFF & (u >> 8) ]
		  :  23 + bitoff[ 0xFF & u ] );
}

longword gsm_L_asl P2((a,n), longword a, int n)
{
	/*
	 * Historical edge behaviour, kept exactly:
	 *  - a shift count >= the word width yields zero (all bits
	 *    shifted out),
	 *  - a count <= -width yields -1 for a negative input and 0
	 *    for a non-negative one,
	 *  - a negative count reverses the direction and becomes an
	 *    arithmetic right shift.
	 * In-range shifts truncate to the low 32 bits, mirroring a
	 * 32 bit hardware word shift.
	 */
	if (n >= 32) return 0;
	if (n <= -32) return -(a < 0);
	if (n < 0) return gsm_L_asr(a, -n);
	return gsm_longword_asl(a, n);
}

word gsm_asl P2((a,n), word a, int n)
{
	if (n >= 16) return 0;
	if (n <= -16) return -(a < 0);
	if (n < 0) return gsm_asr(a, -n);
	return gsm_word_asl(a, n);
}

longword gsm_L_asr P2((a,n), longword a, int n)
{
	if (n >= 32) return -(a < 0);
	if (n <= -32) return 0;
	if (n < 0) return gsm_longword_asl(a, -n);

	return gsm_longword_asr(a, n);
}

word gsm_asr P2((a,n), word a, int n)
{
	if (n >= 16) return -(a < 0);
	if (n <= -16) return 0;
	if (n < 0) return gsm_word_asl(a, -n);

	return gsm_word_asr(a, n);
}

/*
 *  (From p. 46, end of section 4.2.5)
 *
 *  NOTE: The following lines gives [sic] one correct implementation
 *  	  of the div(num, denum) arithmetic operation.  Compute div
 *        which is the integer division of num by denum: with denum
 *	  >= num > 0
 */

word gsm_div P2((num,denum), word num, word denum)
{
	gsm_longword	L_num   = num;
	gsm_longword	L_denum = denum;
	word		div 	= 0;
	int		k 	= 15;

	/* The parameter num sometimes becomes zero.
	 * Although this is explicitly guarded against in 4.2.5,
	 * we assume that the result should then be zero as well.
	 */

	/* assert(num != 0); */

	assert(num >= 0 && denum >= num);
	if (num == 0)
	    return 0;

	/*
	 * Both operands are non-negative and bounded by 16 bits, so
	 * every intermediate value stays well inside the 32 bit word
	 * and the shifts are ordinary defined unsigned-style shifts.
	 */
	while (k--) {
		div   = gsm_word_asl(div, 1);
		L_num = gsm_longword_asl(L_num, 1);

		if (L_num >= L_denum) {
			L_num -= L_denum;
			div++;
		}
	}

	return div;
}
