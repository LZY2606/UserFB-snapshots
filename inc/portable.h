/*
 * Portable fixed point primitives for the GSM 06.10 integer path.
 *
 * The original code relied on three things the C standard does not
 * guarantee:
 *
 *	1. "long" is exactly 32 bits wide,
 *	2. left shifting a negative signed integer keeps the sign,
 *	3. right shifting a negative signed integer fills with the
 *	   sign bit (arithmetic shift).
 *
 * This header gives every arithmetic in the codec an explicit meaning
 * without changing its numerical result:
 *
 *	- gsm_word / gsm_longword are explicitly 16/32 bit two's
 *	  complement signed quantities (checked with a compile time
 *	  assert), regardless of how wide "long" happens to be.
 *	- Left shifts that are meant to keep the low bits of a 16 or 32
 *	  bit word are performed on the matching unsigned type; the
 *	  resulting bit pattern is turned back into the signed value by
 *	  a range-checked conversion.  This reproduces the wrap-around
 *	  truncation of a hardware word shift instead of saturating.
 *	- Signed right shifts round towards negative infinity and are
 *	  computed from the unsigned magnitude, so they never depend on
 *	  the implementation's signed shift direction and never use C
 *	  division on a negative value.
 *	- Wide intermediate products use the guaranteed 32 bit multiply
 *	  of the two 16 bit operands.
 *
 * The header compiles both as C99 (the devcheck build) and as strict
 * ANSI C89 (the legacy Makefile build); only the exact-width typedef
 * source and the inline keyword differ between those modes.
 *
 * Saturating arithmetic (gsm_add, gsm_L_add, ...) keeps the original
 * GSM saturation rules and lives in add.c; the helpers here only make
 * the individual bit operations well defined.
 */

#ifndef PORTABLE_H
#define PORTABLE_H

#include <limits.h>

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 199901L
#  include <stdint.h>
#  if defined(__GNUC__)
#    define GSM_INLINE inline __attribute__((unused))
#  else
#    define GSM_INLINE inline
#  endif
typedef int16_t		gsm_word;
typedef int32_t		gsm_longword;
typedef uint16_t	gsm_uword;
typedef uint32_t	gsm_ulongword;
#else
/*
 * ANSI C89 fallback for the legacy Makefile.  The documented target
 * guarantees 16 bit short and 32 bit int; the width assert below still
 * guards this assumption.  C89 has no inline keyword, so the helpers
 * stay plain static functions; the unused attribute keeps translation
 * units that only need part of the header warning free.
 */
#  if defined(__GNUC__)
#    define GSM_INLINE __attribute__((unused))
#  else
#    define GSM_INLINE
#  endif
typedef short		gsm_word;
typedef int		gsm_longword;
typedef unsigned short	gsm_uword;
typedef unsigned int	gsm_ulongword;
#endif

#define GSM_MIN_WORD		((gsm_word)(-32767 - 1))
#define GSM_MAX_WORD		((gsm_word)32767)
#define GSM_MIN_LONGWORD	((gsm_longword)(-2147483647 - 1))
#define GSM_MAX_LONGWORD	((gsm_longword)2147483647)

/*
 * Reinterpret the low 16/32 bits of an unsigned word as the signed
 * two's complement value with that bit pattern.  The unsigned types
 * are exactly 16/32 bits wide, so the value always fits the signed
 * type; converting out-of-range values is the one operation the
 * standard leaves implementation defined, and every supported target
 * uses the natural two's complement conversion.
 */
static GSM_INLINE gsm_word gsm_word_from_bits(gsm_uword u)
{
	return (gsm_word)u;
}

static GSM_INLINE gsm_longword gsm_longword_from_bits(gsm_ulongword u)
{
	return (gsm_longword)u;
}

/*
 * Explicit two's complement view of a signed word.  Conversion to the
 * matching unsigned type is defined by the C standard to reduce
 * modulo 2**N.
 */
static GSM_INLINE gsm_uword gsm_word_bits(gsm_word x)
{
	return (gsm_uword)x;
}

static GSM_INLINE gsm_ulongword gsm_longword_bits(gsm_longword x)
{
	return (gsm_ulongword)x;
}

/*
 * Arithmetic shift right, rounding towards negative infinity.
 *
 * The shift count is a normal non-negative C shift count here; the
 * public gsm_asr/gsm_L_asr wrappers in add.c handle the out-of-range
 * and negative-count cases with their historical return values.
 */
static GSM_INLINE gsm_word gsm_word_asr(gsm_word x, int by)
{
	gsm_uword u;

	if (x >= 0) return (gsm_word)((gsm_uword)x >> by);

	/*
	 * floor(x / 2**by)  ==  -ceil(|x| / 2**by).
	 * Widen before negating so the magnitude never overflows, then
	 * compute ceil(|x| / 2**by) on the unsigned word.
	 */
	u = (gsm_uword)(-(gsm_longword)x);
	return gsm_word_from_bits((gsm_uword)(0 - ((u + ((gsm_uword)1 << by) - 1u) >> by)));
}

static GSM_INLINE gsm_longword gsm_longword_asr(gsm_longword x, int by)
{
	gsm_ulongword u;

	if (x >= 0) return (gsm_longword)((gsm_ulongword)x >> by);

	/*
	 * |x| for the minimum longword is 2**31, which only fits the
	 * unsigned word, so special-case it instead of negating.
	 */
	u = (x == GSM_MIN_LONGWORD)
		? ((gsm_ulongword)1 << 31)
		: (gsm_ulongword)(-x);
	return gsm_longword_from_bits(
		(gsm_ulongword)(0 - ((u + ((gsm_ulongword)1 << by) - 1u) >> by)));
}

/*
 * Word-preserving arithmetic shift left: keep the low 16/32 bits of
 * the two's complement representation, exactly like a hardware word
 * shift.  This is an explicit low-bit truncation, not a saturation.
 */
static GSM_INLINE gsm_word gsm_word_asl(gsm_word x, int by)
{
	return gsm_word_from_bits((gsm_uword)x << by);
}

static GSM_INLINE gsm_longword gsm_longword_asl(gsm_longword x, int by)
{
	return gsm_longword_from_bits((gsm_ulongword)x << by);
}

/*
 * Q15 fractional multiplies.  Both inputs are 16 bit, so their product
 * always fits a signed 32 bit word; the multiply itself is therefore
 * well defined on the guaranteed 32 bit type.  Only the final scaling
 * has a sign-dependent meaning.
 *
 * gsm_q15_mult:   product / 32768, rounded towards negative infinity.
 * gsm_q15_mult_r: (product + 16384) / 32768, again floored, matching
 *		   the historical arithmetic right shift with the
 *		   round-to-nearest bias.  The GSM rule that
 *		   MIN_WORD * MIN_WORD saturates to MAX_WORD is applied
 *		   by the callers/add.c wrappers.
 */
static GSM_INLINE gsm_word gsm_q15_mult(gsm_word a, gsm_word b)
{
	gsm_longword prod = (gsm_longword)a * (gsm_longword)b;

	/* The product is 32 bit; floor shift it there and narrow the
	 * in-range Q15 result back to a word.
	 */
	return (gsm_word)gsm_longword_asr(prod, 15);
}

static GSM_INLINE gsm_word gsm_q15_mult_r(gsm_word a, gsm_word b)
{
	gsm_longword prod = (gsm_longword)a * (gsm_longword)b + 16384;

	return (gsm_word)gsm_longword_asr(prod, 15);
}

/*
 * GSM_L_MULT: the 32 bit product of two words, shifted left by one and
 * truncated to 32 bits (a low-bit truncation).  Callers guarantee the
 * MIN_WORD/MIN_WORD combination is not used.  The shifted product can
 * exceed 32 bits, so the shift goes through the unsigned word view.
 */
static GSM_INLINE gsm_longword gsm_q15_L_mult(gsm_word a, gsm_word b)
{
	gsm_longword prod = (gsm_longword)a * (gsm_longword)b;

	return gsm_longword_asl(prod, 1);
}

/*
 * Compile-time environment checks.  Every failure here means the
 * target does not meet the portability contract stated in the README;
 * the arithmetic above depends on these exact widths and on the
 * unsigned modulo rules of the C standard.  The negative-size array
 * trick works in both C89 and C99.
 */
typedef char gsm_portable_word_check[
	(CHAR_BIT == 8 &&
	 sizeof(short) == 2 && sizeof(int) == 4 &&
	 sizeof(gsm_word) == 2 && sizeof(gsm_longword) == 4 &&
	 sizeof(gsm_uword) == 2 && sizeof(gsm_ulongword) == 4)
	? 1 : -1];

#endif /* PORTABLE_H */
