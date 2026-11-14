/* Capstone Disassembly Engine */
/* Thumb IR lifter: raise a Thumb code snippet to a typed, acyclic
   expression graph over the entry state (r0-r12, N/Z/C/V, byte memory). */

#ifndef CAPSTONE_THUMB_IR_H
#define CAPSTONE_THUMB_IR_H

#include "platform.h"
#include "capstone.h"

#ifdef __cplusplus
extern "C" {
#endif

/// Opaque handle to a lifted expression graph.
/// A graph owns all of its storage: it never borrows the input bytes,
/// any cs_insn, or the caller's evaluation inputs.
typedef struct cs_thumb_ir cs_thumb_ir;

/// Number of general purpose registers tracked by the graph (r0-r12).
#define CS_THUMB_IR_REG_COUNT 13
/// Number of flags tracked by the graph (N, Z, C, V).
#define CS_THUMB_IR_FLAG_COUNT 4
/// Maximum number of result roots a single instruction can modify
/// (one register + four flags + memory).
#define CS_THUMB_IR_MAX_INSN_ROOTS 6

/// Invalid node id, returned by root queries on bad indices.
#define CS_THUMB_IR_INVALID_NODE 0xFFFFFFFFu

/// Entry flags, in their canonical order.
typedef enum cs_thumb_ir_flag {
    CS_THUMB_IR_FLAG_N = 0,    ///< Negative
    CS_THUMB_IR_FLAG_Z = 1,    ///< Zero
    CS_THUMB_IR_FLAG_C = 2,    ///< Carry
    CS_THUMB_IR_FLAG_V = 3,    ///< oVerflow
} cs_thumb_ir_flag;

/// Type of a graph node.
typedef enum cs_thumb_ir_type {
    CS_THUMB_IR_TYPE_U32 = 0,  ///< 32-bit scalar
    CS_THUMB_IR_TYPE_U1 = 1,   ///< 1-bit scalar (conditions, flags)
    CS_THUMB_IR_TYPE_MEM = 2,  ///< memory version (whole 32-bit address space)
} cs_thumb_ir_type;

/// Operation kind of a graph node.
/// Children of a node always have smaller ids than the node itself
/// (nodes are stored in topological order).
typedef enum cs_thumb_ir_op {
    /// No children. value = constant. Type U32 or U1.
    CS_THUMB_IR_OP_CONST = 0,
    /// No children. value = entry register index 0..12 (r0..r12). Type U32.
    CS_THUMB_IR_OP_INPUT_REG,
    /// No children. value = entry flag (cs_thumb_ir_flag). Type U1.
    CS_THUMB_IR_OP_INPUT_FLAG,
    /// No children. The entry memory version. Type MEM.
    CS_THUMB_IR_OP_INPUT_MEM,
    /// child[0] + child[1], wrapping. Type U32.
    CS_THUMB_IR_OP_ADD,
    /// child[0] - child[1], wrapping. Type U32.
    CS_THUMB_IR_OP_SUB,
    /// Bitwise/logical AND. Type U32 or U1.
    CS_THUMB_IR_OP_AND,
    /// Bitwise/logical OR. Type U32 or U1.
    CS_THUMB_IR_OP_OR,
    /// Bitwise/logical XOR. Type U32 or U1.
    CS_THUMB_IR_OP_XOR,
    /// Bitwise/logical NOT of child[0]. Type U32 or U1.
    CS_THUMB_IR_OP_NOT,
    /// child[0] << (child[1] & 31). Type U32.
    CS_THUMB_IR_OP_SHL,
    /// child[0] >> (child[1] & 31), logical. Type U32.
    CS_THUMB_IR_OP_SHR,
    /// child[0] ? child[1] : child[2]. child[0] is U1; result type is the
    /// common type of child[1]/child[2] (U32, U1 or MEM). Evaluated lazily:
    /// the branch not taken is never evaluated.
    CS_THUMB_IR_OP_SELECT,
    /// Carry-out of child[0] + child[1]. Type U1.
    CS_THUMB_IR_OP_CARRY_ADD,
    /// No-borrow of child[0] - child[1] (child[0] >= child[1], unsigned). Type U1.
    CS_THUMB_IR_OP_CARRY_SUB,
    /// Signed overflow of child[0] + child[1]. Type U1.
    CS_THUMB_IR_OP_OVF_ADD,
    /// Signed overflow of child[0] - child[1]. Type U1.
    CS_THUMB_IR_OP_OVF_SUB,
    /// child[0] == 0. Type U1.
    CS_THUMB_IR_OP_IS_ZERO,
    /// Bit 31 of child[0]. Type U1.
    CS_THUMB_IR_OP_MSB,
    /// Byte at address child[1] (U32) of memory version child[0] (MEM).
    /// Type U32, value range 0..255.
    CS_THUMB_IR_OP_LOAD8,
    /// Memory version child[0] with the byte at address child[1] (U32)
    /// replaced by the low 8 bits of child[2] (U32). Type MEM.
    CS_THUMB_IR_OP_STORE8,
} cs_thumb_ir_op;

/// Which class of result root an instruction modified.
typedef enum cs_thumb_ir_root_kind {
    CS_THUMB_IR_ROOT_REG = 0,  ///< general register, index 0..12
    CS_THUMB_IR_ROOT_FLAG = 1, ///< flag, index is cs_thumb_ir_flag
    CS_THUMB_IR_ROOT_MEM = 2,  ///< memory version, index is 0
} cs_thumb_ir_root_kind;

/// One result root as modified by an instruction.
typedef struct cs_thumb_ir_root_ref {
    uint32_t kind;   ///< cs_thumb_ir_root_kind
    uint32_t index;  ///< register or flag index; 0 for CS_THUMB_IR_ROOT_MEM
    uint32_t node;   ///< node id of the root value after the instruction
} cs_thumb_ir_root_ref;

/// Public, self-contained description of one graph node.
typedef struct cs_thumb_ir_node_info {
    uint32_t type;         ///< cs_thumb_ir_type
    uint32_t op;           ///< cs_thumb_ir_op
    uint32_t value;        ///< CONST value, or entry register/flag index
    uint32_t child_count;  ///< number of valid entries in child[]
    uint32_t child[3];     ///< child node ids (all < this node's id)
} cs_thumb_ir_node_info;

/// Public description of one lifted instruction.
typedef struct cs_thumb_ir_insn_info {
    uint32_t addr;         ///< address of the original instruction
    uint32_t cond;         ///< U1 node: execution condition (CONST 1 if unconditional)
    uint32_t root_count;   ///< number of valid entries in roots[]
    /// Roots this instruction modified, with their node ids after it.
    cs_thumb_ir_root_ref roots[CS_THUMB_IR_MAX_INSN_ROOTS];
} cs_thumb_ir_insn_info;

/// Error codes of the lifter and evaluator.
typedef enum cs_thumb_ir_err {
    CS_THUMB_IR_OK = 0,        ///< no error
    CS_THUMB_IR_ERR_ARG,       ///< invalid argument (NULL pointer, bad id, ...)
    CS_THUMB_IR_ERR_DECODE,    ///< illegal or truncated encoding
    CS_THUMB_IR_ERR_UNSUPPORTED, ///< unsupported instruction or operand shape
    CS_THUMB_IR_ERR_IT,        ///< nested IT, or IT block not finished at fragment end
    CS_THUMB_IR_ERR_RANGE,     ///< the [addr, addr+size) range wraps around uint32
    CS_THUMB_IR_ERR_NOMEM,     ///< allocation failure
    CS_THUMB_IR_ERR_READ,      ///< an entry-memory read8 callback failed (evaluation)
} cs_thumb_ir_err;

/// Detailed error report. addr is the address of the instruction related
/// to the failure (for CS_THUMB_IR_ERR_RANGE the fragment start address;
/// for CS_THUMB_IR_ERR_READ the faulting memory address).
typedef struct cs_thumb_ir_error {
    cs_thumb_ir_err code;
    uint32_t addr;
} cs_thumb_ir_error;

/// Read callback for entry memory bytes during evaluation.
/// Must return true and store the byte in *out on success, false on failure.
/// The callback is only invoked for entry-memory bytes that are actually
/// needed and not covered by a later store. It is never invoked with a
/// write intent; the caller's memory is never modified.
typedef bool (CAPSTONE_API *cs_thumb_ir_read8)(void *ud, uint32_t addr, uint8_t *out);

/// Entry state for one evaluation of a graph.
typedef struct cs_thumb_ir_inputs {
    uint32_t regs[CS_THUMB_IR_REG_COUNT];   ///< entry r0..r12
    uint8_t flags[CS_THUMB_IR_FLAG_COUNT];  ///< entry N,Z,C,V (0 or 1)
    cs_thumb_ir_read8 read8;                ///< entry memory reader (may be NULL
                                            ///<  if no entry byte is ever needed)
    void *read8_ud;                         ///< user data passed to read8
} cs_thumb_ir_inputs;

/// Lift a little-endian Thumb code fragment into an expression graph.
///
/// code/size: read-only machine code (may be NULL when size is 0).
/// addr:      uint32 start address of the fragment.
/// out:       receives the graph on success (CS_THUMB_IR_OK); set to NULL
///            on any error. No partial graph is ever published.
/// err:       optional, receives the error code and related address.
///
/// The fragment is decoded with Capstone (ARM/Thumb, detail on, skipdata
/// off); operands, condition codes and flag-update behaviour are taken
/// from Capstone's decode. The machine code is never executed.
///
/// Supported: 16-bit MOV (immediate and unshifted register forms), low
/// ADD/SUB (register and immediate forms), CMP (register and immediate),
/// LDR/STR with word immediate offset, IT, NOP, and 32-bit MOVW/MOVT.
/// Everything else (SP/LR/PC uses, writeback or register-offset memory
/// operands, other wide instructions, control transfers) is rejected with
/// CS_THUMB_IR_ERR_UNSUPPORTED.
///
/// An empty fragment succeeds and yields the entry roots.
CAPSTONE_EXPORT
cs_thumb_ir_err CAPSTONE_API cs_thumb_ir_lift(const uint8_t *code, size_t size,
                                              uint32_t addr,
                                              cs_thumb_ir **out,
                                              cs_thumb_ir_error *err);

/// Free a graph. NULL is accepted. After this call all node ids are invalid.
CAPSTONE_EXPORT
void CAPSTONE_API cs_thumb_ir_free(cs_thumb_ir *graph);

/// Human-readable name of an error code (never NULL).
CAPSTONE_EXPORT
const char *CAPSTONE_API cs_thumb_ir_strerror(cs_thumb_ir_err err);

/// Number of nodes in the graph (0 for a NULL graph).
CAPSTONE_EXPORT
uint32_t CAPSTONE_API cs_thumb_ir_node_count(const cs_thumb_ir *graph);

/// Copy the description of node `id` into *info.
/// Returns false when the graph or info is NULL, or id is out of range.
CAPSTONE_EXPORT
bool CAPSTONE_API cs_thumb_ir_get_node(const cs_thumb_ir *graph, uint32_t id,
                                        cs_thumb_ir_node_info *info);

/// Node id of the final root of register `reg` (0..12 for r0..r12),
/// or CS_THUMB_IR_INVALID_NODE on bad input.
CAPSTONE_EXPORT
uint32_t CAPSTONE_API cs_thumb_ir_root_reg(const cs_thumb_ir *graph, uint32_t reg);

/// Node id of the final root of flag `flag` (cs_thumb_ir_flag),
/// or CS_THUMB_IR_INVALID_NODE on bad input.
CAPSTONE_EXPORT
uint32_t CAPSTONE_API cs_thumb_ir_root_flag(const cs_thumb_ir *graph, uint32_t flag);

/// Node id of the final memory version root (type MEM),
/// or CS_THUMB_IR_INVALID_NODE on bad input.
CAPSTONE_EXPORT
uint32_t CAPSTONE_API cs_thumb_ir_root_mem(const cs_thumb_ir *graph);

/// Number of lifted instructions (0 for a NULL graph).
CAPSTONE_EXPORT
uint32_t CAPSTONE_API cs_thumb_ir_insn_count(const cs_thumb_ir *graph);

/// Copy the record of the idx-th lifted instruction into *info.
/// Returns false when the graph or info is NULL, or idx is out of range.
CAPSTONE_EXPORT
bool CAPSTONE_API cs_thumb_ir_get_insn(const cs_thumb_ir *graph, uint32_t idx,
                                        cs_thumb_ir_insn_info *info);

/// Evaluate one scalar (U32 or U1) node of the graph under the given entry
/// inputs. The node id may be any scalar node, including the roots returned
/// by cs_thumb_ir_root_reg / cs_thumb_ir_root_flag and the per-instruction
/// roots from cs_thumb_ir_insn_info. U1 results are returned as 0 or 1.
///
/// Evaluation is read-only and keeps no state between calls: the same graph
/// can be evaluated with any number of different input sets, and a failed
/// evaluation leaves no cache behind.
///
/// Returns CS_THUMB_IR_OK on success, CS_THUMB_IR_ERR_ARG on bad arguments
/// (NULL, MEM-typed node, id out of range), and CS_THUMB_IR_ERR_READ when a
/// needed entry-memory byte could not be read; in that case, when fail_addr
/// is not NULL, it receives the faulting address.
CAPSTONE_EXPORT
cs_thumb_ir_err CAPSTONE_API cs_thumb_ir_eval_node(const cs_thumb_ir *graph,
                                                   const cs_thumb_ir_inputs *in,
                                                   uint32_t node,
                                                   uint32_t *out,
                                                   uint32_t *fail_addr);

/// Evaluate the byte at `addr` of the final memory version root under the
/// given entry inputs. Same error contract as cs_thumb_ir_eval_node.
CAPSTONE_EXPORT
cs_thumb_ir_err CAPSTONE_API cs_thumb_ir_eval_mem8(const cs_thumb_ir *graph,
                                                   const cs_thumb_ir_inputs *in,
                                                   uint32_t addr,
                                                   uint8_t *out,
                                                   uint32_t *fail_addr);

#ifdef __cplusplus
}
#endif

#endif /* CAPSTONE_THUMB_IR_H */
