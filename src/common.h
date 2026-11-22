/*
** Copyright (c) 2002-2021, Erik de Castro Lopo <erikd@mega-nerd.com>
** All rights reserved.
**
** This code is released under 2-clause BSD license. Please see the
** file at : https://github.com/libsndfile/libsamplerate/blob/master/COPYING
*/

#ifndef COMMON_H_INCLUDED
#define COMMON_H_INCLUDED

#include <stdint.h>
#ifdef HAVE_STDBOOL_H
#include <stdbool.h>
#endif

#if defined(__x86_64__) || defined(_M_X64)
#   define HAVE_SSE2_INTRINSICS
#elif defined(ENABLE_SSE2_LRINT) && (defined(_M_IX86) || defined(__i386__))
#   if defined(_MSC_VER)
#       define HAVE_SSE2_INTRINSICS
#   elif defined(__clang__)
#       ifdef __SSE2__
#           define HAVE_SSE2_INTRINSICS
#       elif (__has_attribute(target))
#           define HAVE_SSE2_INTRINSICS
#           define USE_TARGET_ATTRIBUTE
#       endif
#   elif defined(__GNUC__)
#       ifdef __SSE2__
#           define HAVE_SSE2_INTRINSICS
#       elif (__GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 9))
#           define HAVE_SSE2_INTRINSICS
#           define USE_TARGET_ATTRIBUTE
#       endif
#   endif
#endif

#ifdef HAVE_SSE2_INTRINSICS
#ifdef HAVE_IMMINTRIN_H
#include <immintrin.h>
#else
#include <emmintrin.h>
#endif
#endif /* HAVE_SSE2_INTRINSICS */

#include <math.h>
#include <stdlib.h>

#ifdef HAVE_VISIBILITY
  #define LIBSAMPLERATE_DLL_PRIVATE __attribute__ ((visibility ("hidden")))
#elif defined (__APPLE__)
  #define LIBSAMPLERATE_DLL_PRIVATE __private_extern__
#else
  #define LIBSAMPLERATE_DLL_PRIVATE
#endif

#define	SRC_MAX_RATIO			256
#define	SRC_MAX_RATIO_STR		"256"

#define	SRC_MIN_RATIO_DIFF		(1e-20)

#ifndef MAX
#define	MAX(a,b)	(((a) > (b)) ? (a) : (b))
#endif

#ifndef MIN
#define	MIN(a,b)	(((a) < (b)) ? (a) : (b))
#endif

#define	ARRAY_LEN(x)			((int) (sizeof (x) / sizeof ((x) [0])))
#define OFFSETOF(type,member)	((int) (&((type*) 0)->member))

#define	MAKE_MAGIC(a,b,c,d,e,f)	((a) + ((b) << 4) + ((c) << 8) + ((d) << 12) + ((e) << 16) + ((f) << 20))

/*
** Inspiration : http://sourcefrog.net/weblog/software/languages/C/unused.html
*/
#ifdef UNUSED
#elif defined (__GNUC__)
#	define UNUSED(x) UNUSED_ ## x __attribute__ ((unused))
#elif defined (__LCLINT__)
#	define UNUSED(x) /*@unused@*/ x
#else
#	define UNUSED(x) x
#endif

#ifdef __GNUC__
#	define WARN_UNUSED	__attribute__ ((warn_unused_result))
#else
#	define WARN_UNUSED
#endif

#include "samplerate.h"

enum
{	SRC_FALSE	= 0,
	SRC_TRUE	= 1,
} ;

enum SRC_MODE
{
	SRC_MODE_PROCESS	= 0,
	SRC_MODE_CALLBACK	= 1
} ;

typedef enum SRC_ERROR
{
	SRC_ERR_NO_ERROR = 0,

	SRC_ERR_MALLOC_FAILED,
	SRC_ERR_BAD_STATE,
	SRC_ERR_BAD_DATA,
	SRC_ERR_BAD_DATA_PTR,
	SRC_ERR_NO_PRIVATE,
	SRC_ERR_BAD_SRC_RATIO,
	SRC_ERR_BAD_PROC_PTR,
	SRC_ERR_SHIFT_BITS,
	SRC_ERR_FILTER_LEN,
	SRC_ERR_BAD_CONVERTER,
	SRC_ERR_BAD_CHANNEL_COUNT,
	SRC_ERR_SINC_BAD_BUFFER_LEN,
	SRC_ERR_SIZE_INCOMPATIBILITY,
	SRC_ERR_BAD_PRIV_PTR,
	SRC_ERR_BAD_SINC_STATE,
	SRC_ERR_DATA_OVERLAP,
	SRC_ERR_BAD_CALLBACK,
	SRC_ERR_BAD_MODE,
	SRC_ERR_NULL_CALLBACK,
	SRC_ERR_NO_VARIABLE_RATIO,
	SRC_ERR_SINC_PREPARE_DATA_BAD_LEN,
	SRC_ERR_BAD_INTERNAL_STATE,
	SRC_ERR_CHECKPOINT_UNSUPPORTED,
	SRC_ERR_CHECKPOINT_TRUNCATED,
	SRC_ERR_CHECKPOINT_BAD_VERSION,
	SRC_ERR_CHECKPOINT_BAD_FORMAT,

	/* This must be the last error number. */
	SRC_ERR_MAX_ERROR
} SRC_ERROR ;

struct SRC_CP_WRITER ;
struct SRC_CP_READER ;

typedef struct SRC_STATE_VT_tag
{
	/* Varispeed process function. */
	SRC_ERROR		(*vari_process) (SRC_STATE *state, SRC_DATA *data) ;

	/* Constant speed process function. */
	SRC_ERROR		(*const_process) (SRC_STATE *state, SRC_DATA *data) ;

	/* State reset. */
	void			(*reset) (SRC_STATE *state) ;

	/* State clone. */
	SRC_STATE		*(*copy) (SRC_STATE *state) ;

	/* State close. */
	void			(*close) (SRC_STATE *state) ;

	/* Persistent checkpoint support. NULL means the converter cannot be
	** checkpointed (SRC_LINEAR and SRC_ZERO_ORDER_HOLD).
	** checkpoint_size returns the number of converter specific payload bytes.
	** checkpoint_write appends the payload to the given byte writer.
	** checkpoint_read restores a freshly created state from a byte reader.
	*/
	size_t			(*checkpoint_size) (SRC_STATE *state) ;
	SRC_ERROR		(*checkpoint_write) (SRC_STATE *state, struct SRC_CP_WRITER *writer) ;
	SRC_ERROR		(*checkpoint_read) (SRC_STATE *state, struct SRC_CP_READER *reader) ;
} SRC_STATE_VT ;

struct SRC_STATE_tag
{
	SRC_STATE_VT *vt ;

	double	last_ratio, last_position ;

	SRC_ERROR	error ;
	int		channels ;

	/* Converter number (SRC_SINC_* etc.) used when this state was created. */
	int		converter_type ;

	/* SRC_MODE_PROCESS or SRC_MODE_CALLBACK */
	enum SRC_MODE	mode ;

	/* Data specific to SRC_MODE_CALLBACK. */
	src_callback_t	callback_func ;
	void			*user_callback_data ;
	long			saved_frames ;
	const float		*saved_data ;

	/* Callback frames already fetched but not yet fed into the converter.
	** Owned by the state; the saved_data/saved_frames pair points into it
	** (or is NULL/zero while the converter owns the data during a read).
	*/
	float			*pending_data ;
	long			pending_frames ;

	/* Pointer to data to converter specific data. */
	void	*private_data ;
} ;

/* In src_checkpoint.c */
typedef struct SRC_CP_WRITER
{	unsigned char	*data ;
	size_t			offset, capacity ;
} SRC_CP_WRITER ;

typedef struct SRC_CP_READER
{	const unsigned char	*data ;
	size_t				offset, length ;
} SRC_CP_READER ;
void src_cp_writer_init (SRC_CP_WRITER *writer, void *buffer, size_t bytes) ;
SRC_ERROR src_cp_write_u16 (SRC_CP_WRITER *writer, uint16_t value) ;
SRC_ERROR src_cp_write_u32 (SRC_CP_WRITER *writer, uint32_t value) ;
SRC_ERROR src_cp_write_u64 (SRC_CP_WRITER *writer, uint64_t value) ;
SRC_ERROR src_cp_write_f32 (SRC_CP_WRITER *writer, float value) ;
SRC_ERROR src_cp_write_f64 (SRC_CP_WRITER *writer, double value) ;
SRC_ERROR src_cp_write_bytes (SRC_CP_WRITER *writer, const void *data, size_t bytes) ;

void src_cp_reader_init (SRC_CP_READER *reader, const void *buffer, size_t bytes) ;
SRC_ERROR src_cp_read_u16 (SRC_CP_READER *reader, uint16_t *value) ;
SRC_ERROR src_cp_read_u32 (SRC_CP_READER *reader, uint32_t *value) ;
SRC_ERROR src_cp_read_u64 (SRC_CP_READER *reader, uint64_t *value) ;
SRC_ERROR src_cp_read_f32 (SRC_CP_READER *reader, float *value) ;
SRC_ERROR src_cp_read_f64 (SRC_CP_READER *reader, double *value) ;
SRC_ERROR src_cp_read_bytes (SRC_CP_READER *reader, void *data, size_t bytes) ;

/* Free the callback remainder buffer held by a state. Safe on process mode
** states and freshly initialised storage.
*/
static inline void
src_state_free_pending (SRC_STATE *state)
{
	if (state->pending_data != NULL)
	{	free (state->pending_data) ;
		state->pending_data = NULL ;
		} ;
	state->pending_frames = 0 ;
	state->saved_data = NULL ;
	state->saved_frames = 0 ;
} /* src_state_free_pending */

/* In src_checkpoint.c: deep copy the callback remainder buffer while cloning
** a state. Returns SRC_ERR_MALLOC_FAILED on allocation failure, SRC_ERR_NO_ERROR
** otherwise (including when there is nothing to copy).
*/
SRC_ERROR src_state_copy_pending (SRC_STATE *to, const SRC_STATE *from) ;

/* In src_sinc.c */
const char* sinc_get_name (int src_enum) ;
const char* sinc_get_description (int src_enum) ;

SRC_STATE *sinc_state_new (int converter_type, int channels, SRC_ERROR *error) ;

size_t sinc_checkpoint_size (SRC_STATE *state) ;
SRC_ERROR sinc_checkpoint_write (SRC_STATE *state, SRC_CP_WRITER *writer) ;
SRC_ERROR sinc_checkpoint_read (SRC_STATE *state, SRC_CP_READER *reader) ;

/* In src_linear.c */
const char* linear_get_name (int src_enum) ;
const char* linear_get_description (int src_enum) ;

SRC_STATE *linear_state_new (int channels, SRC_ERROR *error) ;

/* In src_zoh.c */
const char* zoh_get_name (int src_enum) ;
const char* zoh_get_description (int src_enum) ;

SRC_STATE *zoh_state_new (int channels, SRC_ERROR *error) ;

/*----------------------------------------------------------
** SIMD optimized math functions.
*/

#ifdef HAVE_SSE2_INTRINSICS
static inline int
#ifdef USE_TARGET_ATTRIBUTE
__attribute__((target("sse2")))
#endif
psf_lrintf (float x)
{
	return _mm_cvtss_si32 (_mm_load_ss (&x)) ;
}
static inline int
#ifdef USE_TARGET_ATTRIBUTE
__attribute__((target("sse2")))
#endif
psf_lrint (double x)
{
	return _mm_cvtsd_si32 (_mm_load_sd (&x)) ;
}

#else

static inline int psf_lrintf (float x)
{
	return lrintf (x) ;
} /* psf_lrintf */

static inline int psf_lrint (double x)
{
	return lrint (x) ;
} /* psf_lrint */
#endif

/*----------------------------------------------------------
**	Common static inline functions.
*/

static inline double
fmod_one (double x)
{	double res ;

	res = x - psf_lrint (x) ;
	if (res < 0.0)
		return res + 1.0 ;

	return res ;
} /* fmod_one */

static inline int
is_bad_src_ratio (double ratio)
{	return (ratio < (1.0 / SRC_MAX_RATIO) || ratio > (1.0 * SRC_MAX_RATIO)) ;
} /* is_bad_src_ratio */


#endif	/* COMMON_H_INCLUDED */

