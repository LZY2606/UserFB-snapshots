/*
xop.h

diStorm3 - Powerful disassembler for X86/AMD64
http://ragestorm.net/distorm/
distorm at gmail dot com
Copyright (C) 2003-2021 Gil Dabah
This library is licensed under the BSD license. See the file COPYING.
*/

/*
 * AMD XOP three-byte 0x8F prefix support (map 8 / map 9).
 * The supported instruction subset is:
 *   map 8: VPCMOV (A2), VPPERM (A3), VPROTB/W/D/Q vector count forms (90-93)
 *   map 9: VPROTB/W/D/Q imm8 count forms (C0-C3)
 * Decoding is byte reading only; no machine code is ever executed.
 */

#ifndef XOP_H
#define XOP_H

#include "config.h"
#include "decoder.h"
#include "prefix.h"
#include "instructions.h"

/*
 * The inst-lookup returns this marker inst-info for a recognized XOP
 * instruction. decode_inst redirects it straight to xop_decode.
 */
extern _InstInfo II_XOP;

/*
 * Returns TRUE when ci->code starts with an XOP 0x8F prefix (second byte's
 * low five bits select map 8 or 9). A low-five value below 8 is the normal
 * opcode space (e.g. POP), so it is never treated as an XOP prefix here.
 * XOP prefixes are only recognized in 32/64 bits decoding modes.
 */
int xop_is_prefix(const _CodeInfo* ci);

/* Peeks at the XOP prefix + opcode and validates the supported encodings. */
_InstInfo* xop_lookup(_CodeInfo* ci, _PrefixState* ps);

/*
 * Fully decodes the recognized XOP instruction into the regular _DInst
 * operand model. ModRM, SIB, displacement and the trailing selector/imm byte
 * are all consumed here, so on success ci->code is aligned to the next
 * instruction. Returns DECRES_INPUTERR on any truncated or illegal encoding.
 */
_DecodeResult xop_decode(_CodeInfo* ci, _PrefixState* ps, const uint8_t* startCode, _DInst* di);

#endif /* XOP_H */
