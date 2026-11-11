/*
decoder.h

diStorm3 - Powerful disassembler for X86/AMD64
http://ragestorm.net/distorm/
distorm at gmail dot com
Copyright (C) 2003-2021 Gil Dabah
This library is licensed under the BSD license. See the file COPYING.
*/


#ifndef DECODER_H
#define DECODER_H

#include "config.h"

typedef unsigned int _iflags;

/* Field span tracking, consumed by the relocation entry point. */
#define FIELD_KIND_NONE 0
#define FIELD_KIND_PC 1   /* O_PC relative branch displacement. */
#define FIELD_KIND_RIP 2  /* RIP-relative memory displacement. */
#define MAX_FIELD_SPANS 2

/*
 * The tracker is handed through the actual decoding/operand-consuming path,
 * so span offsets describe the bytes the decoder really read (never guessed).
 * It lives on the caller's stack and holds no state between instructions.
 */
typedef struct {
	const uint8_t* start;
	uint8_t count;
	uint8_t kind[MAX_FIELD_SPANS];
	uint8_t offset[MAX_FIELD_SPANS];
	uint8_t width[MAX_FIELD_SPANS];
} _FieldTracker;

void field_tracker_reset(_FieldTracker* ft, const uint8_t* startCode);
void field_tracker_record(_FieldTracker* ft, const uint8_t* pastField,
                          unsigned int kind, unsigned int width);

_DecodeResult decode_internal(_CodeInfo* _ci, int supportOldIntr, _DInst result[], unsigned int maxResultCount, unsigned int* usedInstructionsCount);

/* Same as decode_internal, but fills a parallel array of per-instruction field trackers. */
_DecodeResult decode_internal_tracked(_CodeInfo* _ci, int supportOldIntr, _DInst result[],
                                      unsigned int maxResultCount, unsigned int* usedInstructionsCount,
                                      _FieldTracker* trackers);

#endif /* DECODER_H */
