/*
Copyright (c) 2026. The YARA Authors. All Rights Reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
this list of conditions and the following disclaimer in the documentation and/or
other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its contributors
may be used to endorse or promote products derived from this software without
specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

/*
Internal interface of the condition-trace subsystem. The public API and the
snapshot layout live in yara/condition_trace.h. Everything in this header is
used by the compiler (parser.c), the VM (exec.c), the scanner (scanner.c) and
the rules loader (rules.c) only.

Two data layouts coexist:

  1. YR_CT_STATIC_* structures are written verbatim into the compiler arena's
     YR_CONDITION_TRACE_SECTION, so they use fixed-size fields and offsets into
     an embedded string pool instead of pointers. That makes the metadata
     survive yr_rules_save_stream()/yr_rules_load_stream() with the same node
     identities and without the original rule sources.

  2. YR_CT_STATE and friends are per-scan heap state holding the dynamic
     instances of one scan. Two scanners sharing the same YR_RULES own
     independent states and never share evidence.
*/

#ifndef YR_CT_TRACE_H
#define YR_CT_TRACE_H

#include <yara/condition_trace.h>
#include <yara/compiler.h>
#include <yara/exec.h>
#include <yara/types.h>

#define YR_CT_META_MAGIC 0x43544d45u  // "CTME"
#define YR_CT_META_VERSION 1u
#define YR_CT_NO_OFFSET UINT32_MAX

// Static metadata header, first bytes of YR_CONDITION_TRACE_SECTION.
#pragma pack(push)
#pragma pack(4)

typedef struct _YR_CT_STATIC_HEADER
{
  uint32_t magic;
  uint32_t version;
  uint32_t num_nodes;

  // Size in bytes of the string pool that follows the node arrays. Offsets in
  // YR_CT_STATIC_NODE address this pool.
  uint32_t string_pool_size;

  // Total number of child-id entries in the children array.
  uint32_t num_child_entries;
} YR_CT_STATIC_HEADER;

// One static expression node. All strings are offsets within the section's
// string pool (YR_CT_NO_OFFSET when absent), so the structure needs no pointer
// relocation and can be saved/loaded verbatim.
typedef struct _YR_CT_STATIC_NODE
{
  uint32_t id;
  uint32_t kind;
  uint32_t rule_idx;
  uint32_t end_line;

  // Offset of the node's main opcode within YR_CODE_SECTION.
  uint32_t code_offset;

  // Extra control-flow opcode offsets used by loops and boolean operators.
  // YR_CT_NO_OFFSET when not applicable.
  uint32_t control_offset;    // JFALSE/JTRUE for and/or.
  uint32_t loop_next_offset;  // OP_ITER_NEXT.
  uint32_t loop_jump_offset;  // OP_JTRUE_P deciding whether to keep iterating.
  uint32_t loop_cond_offset;  // OP_ITER_CONDITION.
  uint32_t loop_end_offset;   // OP_ITER_END.

  uint32_t flags;             // YR_CT_STATIC_FLAG_*.
  uint32_t fold_origin_id;    // Source node before folding, if any.

  uint32_t child_start;       // Start in the section's child-id array.
  uint32_t child_count;

  uint32_t file_off;
  uint32_t namespace_off;
  uint32_t rule_name_off;
  uint32_t detail_off;
} YR_CT_STATIC_NODE;

#pragma pack(pop)

#define YR_CT_STATIC_FLAG_UNSUPPORTED 0x01
#define YR_CT_STATIC_FLAG_RULE_ROOT 0x02
#define YR_CT_STATIC_FLAG_LOOP 0x04
#define YR_CT_STATIC_FLAG_COMPILE_TIME 0x08

// Runtime index built from the static section when a traced scan starts. The
// array is sorted by code_offset, with the control/loop offsets indexed in
// side lookup tables for the opcodes the VM intercepts.
typedef struct _YR_CT_RT_NODE
{
  const YR_CT_STATIC_NODE* meta;

  // Resolved static children, meta->child_count entries.
  struct _YR_CT_RT_NODE** children;

  // Resolved strings inside the section pool; never NULL for namespace/rule
  // names, may be NULL for file/detail.
  const char* file_name;
  const char* namespace_name;
  const char* rule_name;
  const char* detail;
} YR_CT_RT_NODE;

// One recorded dynamic instance. The same node produces one record per
// evaluation, so repeated loop iterations never overwrite each other.
typedef struct _YR_CT_DYN_REC
{
  uint32_t id;
  uint32_t node_id;

  YR_CT_INSTANCE_STATE state;
  YR_CT_SKIP_REASON skip_reason;
  YR_CT_VALUE_TYPE value_type;

  // Value. Meaningful only when value_type != YR_CT_VALUE_UNDEFINED.
  int64_t ival;
  double dval;
  char* sval;  // Owned copy, NULL otherwise.

  // Nested iteration path at evaluation time.
  uint8_t depth;
  uint32_t ordinal[YR_MAX_LOOP_NESTING];
  int64_t path_integer[YR_MAX_LOOP_NESTING];
  const char* path_string[YR_MAX_LOOP_NESTING];  // Pool pointers, copied on take.
  bool path_is_integer[YR_MAX_LOOP_NESTING];

  // Loop/quantifier counters.
  uint32_t iterations_visited;
  uint32_t iterations_true;
  uint32_t winning_instance;
  uint32_t failing_instance;

  // Value resolved at compile time (literal constants).
  bool compile_time;
} YR_CT_DYN_REC;

typedef struct _YR_CT_DYN_EDGE
{
  uint32_t from_instance;
  uint32_t to_instance;
} YR_CT_DYN_EDGE;

// Frame describing one active "for" loop.
typedef struct _YR_CT_LOOP_FRAME
{
  uint32_t rt_index;
  uint32_t instance_id;
  uint32_t ordinal;
  uint32_t true_count;
  uint32_t winning_instance;
  uint32_t failing_instance;
  bool decided;

  // Identity of the current iteration's loop variable.
  bool path_is_integer;
  int64_t path_integer;
  const char* path_string;
} YR_CT_LOOP_FRAME;

typedef struct _YR_CT_DECISION
{
  uint32_t rule_idx;
  bool condition_true;
  bool reported_match;
  YR_CT_INSTANCE_STATE state;
  YR_CT_SKIP_REASON skip_reason;
  uint32_t blocked_by_global_rule_idx;
} YR_CT_DECISION;

// One entry of the stack-parallel instance table. Every value the VM pushes
// gets the id of the dynamic instance that produced it, so causal edges can
// be derived from the exact operands an opcode consumed, even across loops
// that reuse the same memory slots.
typedef struct _YR_CT_STACK_SLOT
{
  uint32_t instance_id;
} YR_CT_STACK_SLOT;

typedef struct _YR_CT_STATE
{
  YR_RULES* rules;

  // Static index.
  uint32_t num_rt_nodes;
  YR_CT_RT_NODE* rt_nodes;
  const YR_CT_STATIC_HEADER* header;
  const char* string_pool;

  // Sorted (offset, rt_index) lookup over main opcodes and control-flow
  // offsets. Mapped opcodes are found with a binary search.
  uint32_t* lookup_offsets;
  uint32_t* lookup_nodes;
  uint32_t lookup_count;

  // Dynamic records.
  uint32_t rec_capacity;
  uint32_t rec_count;
  YR_CT_DYN_REC* recs;

  uint32_t edge_capacity;
  uint32_t edge_count;
  YR_CT_DYN_EDGE* edges;

  // Stack-parallel instance table, kept in lockstep with the VM stack.
  uint32_t slot_sp;
  uint32_t slot_capacity;
  YR_CT_STACK_SLOT* slots;

  // The instance that produced each "mem" slot (loop variables, internal
  // counters). Indexed like the VM mem[] array.
  uint32_t mem_capacity;
  uint32_t* mem_instances;

  // Active loops, indexed by nesting depth.
  uint8_t loop_depth;
  YR_CT_LOOP_FRAME loops[YR_MAX_LOOP_NESTING];

  // Whether the loop at each depth iterates a string set (true) or integers
  // (false). Set from the ITER_START_* opcode.
  bool loop_string_iter[YR_MAX_LOOP_NESTING];

  // Rule root instance per rule idx; YR_CT_INVALID_INSTANCE when the rule
  // hasn't produced one yet.
  uint32_t* rule_root_instance;

  // Decisions, one slot per rule.
  YR_CT_DECISION* decisions;

  bool incomplete;
} YR_CT_STATE;

// ---- Compiler side (parser.c) ---------------------------------------------

// Opens the static section and writes its header. Called once when the first
// rule is compiled with tracing enabled.
int yr_ct_meta_begin(YR_COMPILER* compiler);

// Appends a static node for an opcode emitted through the parser helpers.
// Returns the node id in *node_id (may be NULL). control/loop offsets are
// YR_CT_NO_OFFSET unless the opcode is one of the control-flow instructions.
int yr_ct_meta_emit(
    YR_COMPILER* compiler,
    uint8_t opcode,
    uint32_t code_offset,
    uint32_t* node_id);

// Fills source location/detail of the node emitted most recently.
int yr_ct_meta_annotate(
    YR_COMPILER* compiler,
    int end_line,
    const char* file_name,
    const char* detail);

// Finalizes the static section: writes header and string pool. Called from
// yr_compiler_get_rules().
int yr_ct_meta_finalize(YR_COMPILER* compiler);

// Releases heap-side trace scratch when the compiler is destroyed without
// finalizing the rules.
void yr_ct_meta_abort(YR_COMPILER* compiler);

// ---- Loader side (rules.c) ------------------------------------------------

// Builds the runtime index from a loaded/compiled rules object. No-op when
// the rules have no trace metadata (legacy compiled files included).
int yr_ct_build_runtime_index(YR_RULES* rules);

// Releases the runtime index when the rules are destroyed.
void yr_ct_free_runtime_index(YR_RULES* rules);

// True when the rules carry condition-trace metadata.
bool yr_ct_rules_have_meta(const YR_RULES* rules);

// ---- Scanner side (scanner.c / exec.c) ------------------------------------

// Allocates/clears the per-scan state. Called at the start of every traced
// scan so a previous scan's evidence can never leak into the next one.
int yr_ct_state_reset(YR_SCANNER* scanner);

void yr_ct_state_destroy(YR_CT_STATE* state);

// VM dispatch hooks. yr_ct_vm_before is called right after reading the opcode,
// yr_ct_vm_after once the opcode completed (ip already updated, including
// taken jumps). Neither hook ever changes the scan's semantics.
int yr_ct_vm_before(
    YR_SCANNER* scanner,
    uint32_t code_offset,
    uint8_t opcode);

int yr_ct_vm_after(
    YR_SCANNER* scanner,
    uint32_t code_offset,
    uint8_t opcode,
    const YR_VALUE_STACK* stack);

// Records the per-rule decisions once the VM finished. Called from the
// scanner's reporting loop.
int yr_ct_record_decisions(YR_SCANNER* scanner);

// Marks the current scan's trace as an incomplete real prefix; used on
// timeouts, callback aborts and errors.
void yr_ct_mark_incomplete(YR_SCANNER* scanner);

// Builds the self-contained public snapshot and transfers ownership.
int yr_ct_take_snapshot(
    YR_SCANNER* scanner,
    YR_CONDITION_TRACE** out);

void yr_ct_snapshot_free(YR_CONDITION_TRACE* trace);

#endif
