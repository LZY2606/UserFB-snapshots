/*
 * Copyright 1992 by Jutta Degener and Carsten Bormann, Technische
 * Universitaet Berlin.  See the accompanying file "COPYRIGHT" for
 * details.  THERE IS ABSOLUTELY NO WARRANTY FOR THIS SOFTWARE.
 */

/* $Header: /tmp_amd/presto/export/kbs/jutta/src/gsm/RCS/add.c,v 1.6 1996/07/02 09:57:33 jutta Exp $ */

/*
 *  Portable fixed point primitives.
 *
 *  Every shift is expressed through the helpers below so that no
 *  signed value is ever shifted left and no right shift relies on a
 *  hardware arithmetic shift or on C division rounding.  Saturation
 *  keeps the historical GSM rules.  The code is strictly ANSI C89:
 *  it assumes CHAR_BIT == 8, 16 bit short and 32 bit int, but never
 *  assumes ``long`` is exactly 32 bits.
 */

#include	<stdio.h>
#include	<assert.h>

#include	"private.h"
#include	"gsm.h"
#include	"proto.h"

/* Re-interpret the low 16 bits of an unsigned pattern as a two's
 * complement word.  Adding MIN_WORD to the sign-cleared magnitude
 * avoids any value outside the 16 bit range.
 */
word gsm_mk_word P1((u), unsigned u)
{
	unsigned m = u & 0xFFFFu;
	int v = (int)(m & 0x7FFFu);
	if (m & 0x8000u) v += MIN_WORD;
	return (word)v;
}

/* Re-interpret the low 32 bits of an unsigned long pattern as a
 * two's complement longword, whether long is 32 or 64 bits.  Adding
 * MIN_LONGWORD to the sign-cleared magnitude stays in range, so no
 * 2^32 constant (which would not fit a 32 bit long) is needed.
 */
longword gsm_mk_long P1((u), ulongword u)
{
	ulongword m = u & 0xFFFFFFFFul;
	longword v = (longword)(m & 0x7FFFFFFFul);
	if (m & 0x80000000ul) v += MIN_LONGWORD;
	return v;
}

/* Truncating 16 bit left shift; n in [0,15]. */
word gsm_sll_word P2((a, n), word a, int n)
{
	return gsm_mk_word((unsigned)(uword)a << n);
}

/* Truncating 32 bit left shift; n in [0,31]. */
longword gsm_sll_long P2((a, n), longword a, int n)
{
	return gsm_mk_long((ulongword)a << n);
}

/* Arithmetic right shift with floor (toward -infinity); n in [0,15]. */
word gsm_sra_word P2((a, n), word a, int n)
{
	if (a >= 0) return (word)((uword)a >> n);
	return (word)~((uword)(word)~a >> n);
}

/* Arithmetic right shift with floor (toward -infinity); n in [0,31]. */
longword gsm_sra_long P2((a, n), longword a, int n)
{
	if (a >= 0) return (longword)((ulongword)a >> n);
	return (longword)~((ulongword)(longword)~a >> n);
}

/* Q15 products matching the historical macros exactly, including the
 * truncating re-interpretation (no MIN_WORD special case here).
 */
word gsm_mult_m P2((a, b), word a, word b)
{
	return gsm_mk_word((uword)gsm_sra_long(
				(longword)a * (longword)b, 15));
}

word gsm_mult_r_m P2((a, b), word a, word b)
{
	return gsm_mk_word((uword)gsm_sra_long(
				(longword)a * (longword)b + 16384, 15));
}

longword gsm_L_mult_m P2((a, b), word a, word b)
{
	return gsm_sll_long((longword)a * (longword)b, 1);
}

word gsm_add P2((a,b), word a, word b)
{
	longword sum = (longword)a + (longword)b;
	return (word)(sum < MIN_WORD ? MIN_WORD
				    : sum > MAX_WORD ? MAX_WORD : sum);
}

word gsm_sub P2((a,b), word a, word b)
{
	longword diff = (longword)a - (longword)b;
	return (word)(diff < MIN_WORD ? MIN_WORD
				     : diff > MAX_WORD ? MAX_WORD : diff);
}

word gsm_mult P2((a,b), word a, word b)
{
	if (a == MIN_WORD && b == MIN_WORD) return MAX_WORD;
	return gsm_mult_m(a, b);
}

word gsm_mult_r P2((a,b), word a, word b)
{
	if (a == MIN_WORD && b == MIN_WORD) return MAX_WORD;
	return gsm_mult_r_m(a, b);
}

word gsm_abs P1((a), word a)
{
	return a < 0 ? (a == MIN_WORD ? MAX_WORD : (word)-a) : a;
}

longword gsm_L_mult P2((a,b),word a, word b)
{
	assert( a != MIN_WORD || b != MIN_WORD );
	return gsm_L_mult_m(a, b);
}

longword gsm_L_add P2((a,b), longword a, longword b)
{
	/* Unsigned magnitude bookkeeping keeps the result well defined
	 * even if long is exactly 32 bits wide.
	 */
	if (a < 0) {
		if (b >= 0) return a + b;
		else {
			ulongword A = (ulongword)-(a + 1)
				    + (ulongword)-(b + 1);
			return A >= MAX_LONGWORD
				? MIN_LONGWORD : -(longword)A - 2;
		}
	}
	else if (b <= 0) return a + b;
	else {
		ulongword A = (ulongword)a + (ulongword)b;
		return A > MAX_LONGWORD ? MAX_LONGWORD : (longword)A;
	}
}

longword gsm_L_sub P2((a,b), longword a, longword b)
{
	if (a >= 0) {
		if (b >= 0) return a - b;
		else {
			/* a>=0, b<0 */
			ulongword A = (ulongword)a + (ulongword)-(b + 1);
			return A >= MAX_LONGWORD
				? MAX_LONGWORD : (longword)(A + 1);
		}
	}
	else if (b <= 0) return a - b;
	else {
		/* a<0, b>0 */
		ulongword A = (ulongword)-(a + 1) + (ulongword)b;
		return A >= MAX_LONGWORD
			? MIN_LONGWORD : -(longword)A - 1;
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
	ulongword x;

	assert(a != 0);

	if (a < 0) {
		if (a <= -1073741824) return 0;
		/* ~a is positive here; inspect its unsigned pattern so
		 * all shifts and masks operate on non-negative values.
		 */
		x = (ulongword)~a;
	}
	else {
		x = (ulongword)a;
	}

	return    x & 0xffff0000ul
		? ( x & 0xff000000ul
		  ?  -1 + bitoff[ 0xFF & (unsigned)(x >> 24) ]
		  :   7 + bitoff[ 0xFF & (unsigned)(x >> 16) ] )
		: ( x & 0xff00ul
		  ?  15 + bitoff[ 0xFF & (unsigned)(x >> 8) ]
		  :  23 + bitoff[ 0xFF & (unsigned)x ] );
}

longword gsm_L_asl P2((a,n), longword a, int n)
{
	/* Historical boundary behavior is preserved: shifts beyond the
	 * word width truncate to zero, and negative counts reverse into
	 * an arithmetic right shift.
	 */
	if (n >= 32) return 0;
	if (n <= -32) return -(a < 0);
	if (n < 0) return gsm_L_asr(a, -n);
	return gsm_sll_long(a, n);
}

word gsm_asl P2((a,n), word a, int n)
{
	if (n >= 16) return 0;
	if (n <= -16) return -(a < 0);
	if (n < 0) return gsm_asr(a, -n);
	return gsm_sll_word(a, n);
}

longword gsm_L_asr P2((a,n), longword a, int n)
{
	if (n >= 32) return -(a < 0);
	if (n <= -32) return 0;
	if (n < 0) return gsm_sll_long(a, -n);
	return gsm_sra_long(a, n);
}

word gsm_asr P2((a,n), word a, int n)
{
	if (n >= 16) return -(a < 0);
	if (n <= -16) return 0;
	if (n < 0) return gsm_sll_word(a, -n);
	return gsm_sra_word(a, n);
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
	longword	L_num   = num;
	longword	L_denum = denum;
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

	while (k--) {
		div   = gsm_sll_word(div, 1);
		L_num = gsm_sll_long(L_num, 1);

		if (L_num >= L_denum) {
			L_num -= L_denum;
			div++;
		}
	}

	return div;
}
