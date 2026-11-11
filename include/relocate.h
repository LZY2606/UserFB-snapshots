/*
relocate.h

diStorm3 - Offline x86-64 code relocation.

This header adds two facilities on top of the unchanged distorm_decompose /
distorm_decode / distorm_format interfaces:

  1. distorm_decompose_fields64()
     A decompose entry point that additionally reports, for every decoded
     instruction, the exact in-instruction byte spans of PC-relative branch
     displacements (O_PC) and RIP-relative memory displacements, together
     with their original absolute targets. The spans are recorded by the
     decoder itself while it consumes bytes; they are never searched for or
     guessed afterwards.

  2. distorm_relocate64() / distorm_relocate_free()
     Relocates a contiguous buffer of x86-64 instructions from one virtual
     address to another, rewriting PC-relative fields, widening short
     branches only when their reach is lost, preserving RIP-relative data
     addresses, and routing out-of-range external JMP/CALL through a tail
     address table. Only caller supplied bytes and addresses are touched:
     no process memory is read, and output bytes are never loaded or executed.

Copyright (C) 2003-2021 Gil Dabah
This library is licensed under the BSD license. See the file COPYING.
*/

#ifndef RELOCATE_H
#define RELOCATE_H

#include <stddef.h>
#include "distorm.h"

#ifdef __cplusplus
 extern "C" {
#endif

/* Kind of a position-dependent instruction field. */
typedef enum {
	RFIELD_NONE = 0, /* Empty record: the instruction has no such field. */
	RFIELD_PC = 1,   /* PC-relative branch displacement (O_PC operand). */
	RFIELD_RIP = 2   /* RIP-relative memory displacement. */
} _RelocFieldKind;

/*
 * One position-dependent field inside an instruction.
 * byteOffset is relative to the first byte of the instruction, so bytes that
 * follow the field (for example an immediate after a RIP displacement) are
 * never confused with it.
 */
typedef struct {
	uint8_t kind;        /* A _RelocFieldKind value. */
	uint8_t byteOffset;  /* Offset of the field inside the instruction. */
	uint8_t byteWidth;   /* Field width in bytes: 1, 2 or 4. */
	uint8_t reserved;    /* Zero, reserved for future use. */
	_OffsetType target;  /* Original absolute target of the field. */
} _RelocField;

/* No x86-64 instruction carries more than one PC/RIP field; room for two. */
#define RELOCATE_MAX_FIELDS 2

/*
 * A decoded instruction paired with its field records.
 * One element is produced per result entry, including undecodable entries,
 * which simply get fieldCount == 0.
 */
typedef struct {
	_DInst inst;
	_RelocField fields[RELOCATE_MAX_FIELDS];
	uint8_t fieldCount;
	uint8_t reserved[3];
} _RelocFieldInst;

/* Failure classification for distorm_relocate64(). */
typedef enum {
	RLOC_OK = 0,
	RLOC_INPUT = 1,           /* Invalid arguments. */
	RLOC_UNDECODABLE = 2,     /* Undecodable or truncated instruction. */
	RLOC_BAD_TARGET = 3,      /* Internal branch lands inside an instruction. */
	RLOC_OVERFLOW = 4,        /* Address/size overflow or unreachable branch. */
	RLOC_RIP_UNREACHABLE = 5,/* RIP data address cannot be kept with disp32. */
	RLOC_UNSUPPORTED = 6      /* Construct the relocator explicitly refuses. */
} _RelocateError;

/*
 * On failure, describes what went wrong and where (offset relative to the
 * input buffer). message points at a static string and must not be freed.
 */
typedef struct {
	_RelocateError code;
	uint8_t reserved[4];
	_OffsetType offset;
	const char* message;
} _RelocateFailure;

/*
 * Per-instruction layout correspondence between the original buffer and the
 * relocated output. Offsets are relative to the respective buffer starts.
 */
typedef struct {
	_OffsetType origOffset;
	_OffsetType newOffset;
	uint16_t origSize;
	uint16_t newSize; /* Expanded size when a short branch was widened. */
	uint8_t widened;  /* Non-zero when the encoding grew. */
	uint8_t reserved[3];
} _RelocMapping;

/*
 * Owned relocation output. On success code/codeSize hold the relocated bytes
 * and mappings/mappingCount describe every original instruction. On failure
 * no owned storage remains and only 'failure' is meaningful.
 *
 * The instruction region occupies [0, instructionSize). The address constant
 * region (8-byte entries) occupies [instructionSize, codeSize). The virtual
 * address of the original segment end is 'continuation'; an explicit tail
 * jump routes natural fall-through there, so execution can never run into the
 * constant region.
 */
typedef struct {
	uint8_t* code;            /* Owned output bytes, freed by distorm_relocate_free. */
	size_t codeSize;
	_RelocMapping* mappings;  /* Owned, one entry per original instruction. */
	size_t mappingCount;
	size_t instructionSize;   /* Length of the instruction region. */
	size_t constTableSize;    /* Length of the tail address constant region. */
	_OffsetType newStartAddr; /* Virtual address of output byte zero. */
	_OffsetType continuation; /* Virtual address corresponding to old end. */
	_RelocateFailure failure;
} _RelocateResult;

/*
 * Behaves exactly like distorm_decompose (same _CodeInfo semantics and return
 * codes, Decode64Bits only), but additionally fills the field records of each
 * _RelocFieldInst result element. Records are strictly per call: interleaved
 * calls, failures and re-decodes never reuse state from a previous call.
 */
_DecodeResult distorm_decompose_fields64(_CodeInfo* ci, _RelocFieldInst result[],
                                         unsigned int maxInstructions,
                                         unsigned int* usedInstructionsCount);

/*
 * Relocates codeLen instruction bytes from oldStart to newStart.
 * The input buffer is treated as read-only and is never modified.
 * Returns 1 on success, 0 on failure (see result->failure).
 * A zero-length input yields a zeroed, empty, successful result.
 */
int distorm_relocate64(const uint8_t* code, size_t codeLen,
                       _OffsetType oldStart, _OffsetType newStart,
                       _RelocateResult* result);

/* Releases all owned storage and zeroes the result. Safe on empty/zero input. */
void distorm_relocate_free(_RelocateResult* result);

#ifdef __cplusplus
} /* End Of Extern */
#endif

#endif /* RELOCATE_H */
