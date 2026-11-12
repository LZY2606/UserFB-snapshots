#ifndef SPVM_CHECKPOINT_FIXTURES_H
#define SPVM_CHECKPOINT_FIXTURES_H
#include <spvm/spirv.h>
#include <spvm/types.h>
/* Hand-assembled numeric modules for checkpoint tests. No assembler or SDK
 * is required. Every array is a complete, self-contained SPIR-V module. */
#define W(n, op) (((n) << SpvWordCountShift) | SpvOp##op)

/* Richly typed module: an Output int3 vector, a Private array of structs
 * (each struct holds an int and a float2), plus Function-local float64,
 * bool, 2x2 float matrix and float2 array variables. main immediately
 * returns; all variables are allocated during state creation. */
static const spvm_word cp_types_words[] = {
	SpvMagicNumber, 0x00010000, 0, 90, 0,
	W(2,Capability), SpvCapabilityShader,
	W(3,MemoryModel), SpvAddressingModelLogical, SpvMemoryModelGLSL450,
	W(5,EntryPoint), SpvExecutionModelFragment, 70, 0x0000006d, 0,
	W(3,ExecutionMode), 70, SpvExecutionModeOriginUpperLeft,

	/* scalar and aggregate types that need no constants first */
	W(2,TypeVoid), 1,
	W(4,TypeInt), 2, 32, 1,
	W(2,TypeBool), 3,
	W(3,TypeFloat), 4, 32,
	W(3,TypeFloat), 5, 64,
	W(4,TypeVector), 6, 4, 2,          /* v2f */
	W(4,TypeVector), 7, 2, 3,          /* v3i */
	W(4,TypeMatrix), 8, 6, 2,          /* mat2 of v2f */
	W(4,TypeStruct), 9, 2, 6,          /* S { int, v2f } */

	/* constants must exist before OpTypeArray reads their length */
	W(4,Constant), 2, 40, 0,
	W(4,Constant), 2, 41, 1,
	W(4,Constant), 2, 42, 2,
	W(4,Constant), 4, 43, 0x80000000u,  /* float negative zero */
	W(5,Constant), 5, 44, 0, 0x3FF80000, /* double 1.5 */
	W(3,ConstantTrue), 3, 45,
	W(3,ConstantFalse), 3, 46,

	W(4,TypeArray), 10, 2, 42,         /* arr2i: int element is allocated */
	W(4,TypeArray), 11, 9, 42,         /* arr2S */

	W(4,TypePointer), 20, SpvStorageClassFunction, 2,
	W(4,TypePointer), 21, SpvStorageClassPrivate, 11,
	W(4,TypePointer), 22, SpvStorageClassOutput, 7,
	W(4,TypePointer), 24, SpvStorageClassFunction, 5,
	W(4,TypePointer), 25, SpvStorageClassFunction, 3,
	W(4,TypePointer), 26, SpvStorageClassFunction, 8,
	W(4,TypePointer), 27, SpvStorageClassFunction, 10,
	W(3,TypeFunction), 30, 1,

	W(4,Variable), 22, 60, SpvStorageClassOutput,
	W(4,Variable), 21, 61, SpvStorageClassPrivate,

	W(5,Function), 1, 70, 0, 30,
	W(2,Label), 71,
	W(4,Variable), 24, 62, SpvStorageClassFunction,
	W(4,Variable), 25, 63, SpvStorageClassFunction,
	W(4,Variable), 26, 65, SpvStorageClassFunction,
	W(4,Variable), 27, 66, SpvStorageClassFunction,
	W(1,Return),
	W(1,FunctionEnd)
};

/* Ineligible module: declares a runtime array. Must be rejected. */
static const spvm_word cp_runtime_words[] = {
	SpvMagicNumber, 0x00010000, 0, 20, 0,
	W(2,Capability), SpvCapabilityShader,
	W(3,MemoryModel), SpvAddressingModelLogical, SpvMemoryModelGLSL450,
	W(5,EntryPoint), SpvExecutionModelFragment, 10, 0x0000006d, 0,
	W(3,ExecutionMode), 10, SpvExecutionModeOriginUpperLeft,
	W(2,TypeVoid), 1,
	W(4,TypeInt), 2, 32, 1,
	W(3,TypeRuntimeArray), 3, 2,
	W(3,TypeFunction), 4, 1,
	W(5,Function), 1, 10, 0, 4,
	W(2,Label), 11,
	W(1,Return),
	W(1,FunctionEnd)
};

/* Ineligible module: declares an image type. Must be rejected. */
static const spvm_word cp_image_words[] = {
	SpvMagicNumber, 0x00010000, 0, 20, 0,
	W(2,Capability), SpvCapabilityShader,
	W(3,MemoryModel), SpvAddressingModelLogical, SpvMemoryModelGLSL450,
	W(5,EntryPoint), SpvExecutionModelFragment, 10, 0x0000006d, 0,
	W(3,ExecutionMode), 10, SpvExecutionModeOriginUpperLeft,
	W(2,TypeVoid), 1,
	W(3,TypeFloat), 2, 32,
	W(9,TypeImage), 3, 2, SpvDim2D, 0, 0, 0, 0, SpvImageFormatUnknown,
	W(3,TypeFunction), 4, 1,
	W(5,Function), 1, 10, 0, 4,
	W(2,Label), 11,
	W(1,Return),
	W(1,FunctionEnd)
};

/* Ineligible module: contains an atomic instruction. Must be rejected. */
static const spvm_word cp_atomic_words[] = {
	SpvMagicNumber, 0x00010000, 0, 20, 0,
	W(2,Capability), SpvCapabilityShader,
	W(3,MemoryModel), SpvAddressingModelLogical, SpvMemoryModelGLSL450,
	W(5,EntryPoint), SpvExecutionModelGLCompute, 10, 0x0000006d, 0,
	W(2,TypeVoid), 1,
	W(4,TypeInt), 2, 32, 1,
	W(4,TypePointer), 3, SpvStorageClassFunction, 2,
	W(3,TypeFunction), 4, 1,
	W(4,Constant), 2, 5, 0,
	W(4,Constant), 2, 6, 1,
	W(5,Function), 1, 10, 0, 4,
	W(2,Label), 11,
	W(4,Variable), 3, 12, SpvStorageClassFunction,
	/* OpAtomicStore %12 %5 %5 %6: pointer, memory, semantics, value */
	W(5,AtomicStore), 12, 5, 5, 6,
	W(1,Return),
	W(1,FunctionEnd)
};
#undef W
#endif
