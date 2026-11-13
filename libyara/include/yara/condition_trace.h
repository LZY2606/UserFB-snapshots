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
Condition tracing.

Condition traces answer questions that match callbacks and timing statistics
can't: why a rule whose strings matched didn't report, or what happened on each
iteration of a quantifier. Tracing is opt-in at both compile time
(yr_compiler_set_condition_trace) and scan time
(yr_scanner_set_condition_trace). The scan engine is never replaced by a
custom evaluator: every recorded value comes from the real YARA compiler and
VM paths, and unsupported expressions (module functions and everything else
outside the supported set) keep their normal behavior while the trace marks
the boundary instead of silently omitting it.

A trace is made of two parts:

  - A static graph (YR_CONDITION_TRACE): one YR_CT_NODE per expression node
    emitted by the compiler. Node identities survive save/load, they don't
    depend on the original sources being available.
  - Dynamic evaluation instances (YR_CT_INSTANCE): one per actual evaluation of
    a node, distinguished by their iteration path. The same "for" body is
    evaluated once per iteration and each evaluation gets its own instance; an
    instance is never overwritten by a later iteration.

The object handed over by yr_scanner_take_condition_trace() is self-contained:
it owns copies of every name, edge and value, and remains readable after the
scanner and the compiled rules have been destroyed. Release it with
yr_condition_trace_destroy().
*/

#ifndef YR_CONDITION_TRACE_H
#define YR_CONDITION_TRACE_H

#include <yara/compiler.h>
#include <yara/scanner.h>
#include <yara/types.h>

// Kinds of static expression nodes.
typedef enum _YR_CT_NODE_KIND
{
  YR_CT_NODE_RULE = 0,         // Rule condition root.
  YR_CT_NODE_AND,              // Boolean "and".
  YR_CT_NODE_OR,               // Boolean "or".
  YR_CT_NODE_NOT,              // Boolean "not".
  YR_CT_NODE_DEFINED,          // "defined" operator.
  YR_CT_NODE_INT_ARITH,        // Integer arithmetic (+, -, *, \, %, unary -).
  YR_CT_NODE_INT_COMPARE,      // Integer comparison (<, <=, >, >=, ==, !=).
  YR_CT_NODE_DBL_ARITH,        // Double arithmetic.
  YR_CT_NODE_DBL_COMPARE,      // Double comparison.
  YR_CT_NODE_STR_COMPARE,      // String comparison.
  YR_CT_NODE_FILESIZE,         // filesize keyword.
  YR_CT_NODE_ENTRYPOINT,       // entrypoint keyword.
  YR_CT_NODE_INT_READ,         // int8/uint16/... memory reads.
  YR_CT_NODE_STRING_FOUND,     // $a / $a at x / $a in (x..y).
  YR_CT_NODE_STRING_COUNT,     // #a / #a in (x..y).
  YR_CT_NODE_STRING_OFFSET,    // @a / @a[n].
  YR_CT_NODE_STRING_LENGTH,    // !a / !a[n].
  YR_CT_NODE_RULE_REF,         // Reference to another rule.
  YR_CT_NODE_OF,               // "N of (string/rule set)" and percent forms.
  YR_CT_NODE_OF_FOUND_IN,      // "N of ... in (x..y)".
  YR_CT_NODE_OF_FOUND_AT,      // "N of ... at x".
  YR_CT_NODE_FOR,              // any/all/none/N "for" loop.
  YR_CT_NODE_CONST_INT,        // Integer literal, incl. quantifier constants.
  YR_CT_NODE_CONST_FLOAT,      // Double literal.
  YR_CT_NODE_CONST_STRING,     // Text literal.
  YR_CT_NODE_LOOP_VAR,         // "for" loop variable read.
  YR_CT_NODE_ITERATION,        // One iteration step of a "for" loop.
  YR_CT_NODE_PUSH,             // Other value-producing OP_PUSH (e.g. regexp).
  YR_CT_NODE_OBJ_LOAD,         // Module/object load: unsupported boundary.
  YR_CT_NODE_OBJ_FIELD,        // Module/object field: unsupported boundary.
  YR_CT_NODE_OBJ_VALUE,        // Module/object value: unsupported boundary.
  YR_CT_NODE_OBJ_INDEX,        // Array index / dictionary lookup.
  YR_CT_NODE_CALL,             // Module function call: unsupported boundary.
  YR_CT_NODE_MATCHES,          // "matches" operator: unsupported boundary.
  YR_CT_NODE_STRING_OP,        // contains/startswith/...: unsupported boundary.
  YR_CT_NODE_OTHER,            // Anything else, flagged unsupported.
} YR_CT_NODE_KIND;

// Value types carried by evaluation instances.
typedef enum _YR_CT_VALUE_TYPE
{
  YR_CT_VALUE_UNDEFINED = 0,  // Explicitly undefined, distinct from 0/false.
  YR_CT_VALUE_INTEGER,
  YR_CT_VALUE_FLOAT,
  YR_CT_VALUE_BOOLEAN,
  YR_CT_VALUE_STRING,
} YR_CT_VALUE_TYPE;

// Evaluation state of a dynamic instance.
typedef enum _YR_CT_INSTANCE_STATE
{
  // The node's code ran and produced a value.
  YR_CT_INST_EVALUATED = 0,

  // Never executed at runtime: the value was resolved by compile-time constant
  // folding. The instance value is the folded value and its origin node points
  // back at the source expression that was folded.
  YR_CT_INST_FOLDED,

  // Never executed because control flow didn't reach it (short-circuit, loop
  // quantifier already decided, skipped rule). "short_circuit_reason" explains
  // why when one is known.
  YR_CT_INST_SKIPPED,

  // The node belongs to an unsupported expression region. Its scan behavior is
  // unchanged but the trace doesn't claim per-node evaluation evidence here.
  YR_CT_INST_UNSUPPORTED,
} YR_CT_INSTANCE_STATE;

// Reasons why a node was skipped without being evaluated.
typedef enum _YR_CT_SKIP_REASON
{
  YR_CT_SKIP_NONE = 0,
  YR_CT_SKIP_AND_SHORT_CIRCUIT,   // Left operand of "and" was false/undefined.
  YR_CT_SKIP_OR_SHORT_CIRCUIT,    // Left operand of "or" was true.
  YR_CT_SKIP_LOOP_NOT_ENTERED,    // Quantifier decided before more iterations.
  YR_CT_SKIP_LOOP_EMPTY,          // Iterator produced no elements.
  YR_CT_SKIP_RULE_DISABLED,       // Rule disabled with yr_rule_disable().
  YR_CT_SKIP_RULE_NOT_REQUIRED,   // required_eval optimization skipped it.
  YR_CT_SKIP_UNSUPPORTED,         // Part of an unsupported expression.
} YR_CT_SKIP_REASON;

// One entry in a nested iteration path, e.g. the outer loop's i=2 followed by
// the inner loop's j=1. String-set iterations carry the iterated string's
// identifier instead of an integer value.
typedef struct _YR_CT_ITERATION
{
  // 1-based ordinal of the iteration within its loop (first iteration is 1).
  uint32_t ordinal;

  // Value of the loop variable for integer iterations, YR_UNDEFINED otherwise.
  int64_t integer_value;

  // True when integer_value is meaningful.
  bool is_integer;

  // Identifier of the iterated string for string-set iterations, NULL
  // otherwise. Owned by the trace object.
  const char* string_identifier;
} YR_CT_ITERATION;

// A causal edge between dynamic instances: the "from" instance actually fed
// the "to" instance during execution.
typedef struct _YR_CT_EDGE
{
  uint32_t from_instance;
  uint32_t to_instance;
} YR_CT_EDGE;

// Static node. All pointers are owned by the trace and stay valid until
// yr_condition_trace_destroy().
typedef struct _YR_CT_NODE
{
  // Stable identity, also used by YR_CT_INSTANCE.node_id.
  uint32_t id;

  YR_CT_NODE_KIND kind;

  // Rule that contains the node.
  uint32_t rule_idx;

  // Namespace and rule names; owned by the trace.
  const char* namespace_name;
  const char* rule_name;

  // Source file name and 1-based line where the expression ends. The file name
  // is NULL for compiled rules loaded from a stream whose sources weren't
  // available (the identity of the node doesn't depend on it).
  const char* file_name;
  int32_t end_line;

  // Static child node ids, in evaluation order. Folded constants have no
  // runtime children but point at their origin through fold_origin_node_id.
  uint32_t child_count;
  const uint32_t* child_ids;

  // When this node was produced by constant folding, the id of the node that
  // represents the expression before folding; YR_CT_INVALID_NODE otherwise.
  uint32_t fold_origin_node_id;

  // True for nodes inside an unsupported region (module functions and other
  // out-of-scope expressions). The graph stays structurally complete but no
  // runtime evidence is claimed for those subtrees.
  bool unsupported;

  // Node-specific extra text (e.g. the string identifier for string nodes,
  // the referenced rule name for rule references). NULL when not applicable.
  const char* detail;
} YR_CT_NODE;

// Dynamic evaluation instance.
typedef struct _YR_CT_INSTANCE
{
  // Instance identity within the trace.
  uint32_t id;

  // Static node this is an evaluation of.
  uint32_t node_id;

  YR_CT_INSTANCE_STATE state;
  YR_CT_SKIP_REASON short_circuit_reason;

  // Nested iteration path. Empty for nodes outside any loop. Owned by the
  // trace.
  uint32_t iteration_depth;
  const YR_CT_ITERATION* iteration_path;

  YR_CT_VALUE_TYPE value_type;

  // The evaluated value. Undefined instances use YR_CT_VALUE_UNDEFINED and
  // leave both union members untouched, so undefined stays distinct from 0 and
  // from false.
  union
  {
    int64_t i;
    double d;
  } value;

  // Owned text for YR_CT_VALUE_STRING. NULL otherwise.
  const char* string_value;

  // Loop/quantifier bookkeeping. Zero for non-loop nodes.
  uint32_t iterations_visited;  // Iterations actually entered.
  uint32_t iterations_true;     // Whose body evaluated to true.
  uint32_t winning_instance;    // Body instance that satisfied an "any"/"N".
  uint32_t failing_instance;    // Body instance that failed an "all"/"none".
} YR_CT_INSTANCE;

// Final decision recorded for a rule: the condition value and the reporting
// outcome are tracked separately, because a true condition can still be
// blocked from reporting by an unsatisfied global rule in the namespace.
typedef struct _YR_CT_RULE_DECISION
{
  uint32_t rule_idx;

  // Condition value as computed by the VM (false when the rule was skipped).
  bool condition_true;

  // Whether a CALLBACK_MSG_RULE_MATCHING would be emitted for this rule.
  bool reported_match;

  YR_CT_INSTANCE_STATE state;
  YR_CT_SKIP_REASON skip_reason;

  // When reported_match is false despite condition_true, the idx of the global
  // rule that made the namespace unsatisfied; YR_CT_INVALID_RULE otherwise.
  uint32_t blocked_by_global_rule_idx;
} YR_CT_RULE_DECISION;

#define YR_CT_INVALID_NODE UINT32_MAX
#define YR_CT_INVALID_INSTANCE UINT32_MAX
#define YR_CT_INVALID_RULE UINT32_MAX

// Self-contained trace snapshot.
typedef struct _YR_CONDITION_TRACE
{
  // Static graph.
  uint32_t num_nodes;
  const YR_CT_NODE* nodes;

  // Dynamic instances, in the order they were produced during execution.
  uint32_t num_instances;
  const YR_CT_INSTANCE* instances;

  // Actual causal edges between instances.
  uint32_t num_edges;
  const YR_CT_EDGE* edges;

  // One decision per rule, in rule_idx order.
  uint32_t num_decisions;
  const YR_CT_RULE_DECISION* decisions;

  // True when the scan was interrupted (timeout, callback abort, error). The
  // dynamic part is then only the real prefix executed up to the interruption,
  // never a stale result from a previous scan.
  bool incomplete;
} YR_CONDITION_TRACE;

// Enables collection of expression metadata while compiling rules added after
// this call. Must be called before yr_compiler_add_*; enabling it only affects
// subsequently parsed sources.
YR_API void yr_compiler_set_condition_trace(
    YR_COMPILER* compiler,
    int enabled);

// Enables dynamic condition tracing for scans performed with this scanner.
// Returns ERROR_CONDITION_TRACE_MISSING when the compiled rules carry no
// trace metadata (for example rules produced with tracing disabled, or
// compiled rules loaded from a legacy .yarc).
YR_API int yr_scanner_set_condition_trace(YR_SCANNER* scanner, int enabled);

// Hands over the trace produced by the most recent scan. The caller receives
// ownership of *out and must release it with yr_condition_trace_destroy().
// On success *out is NULL if tracing is disabled or no scan ran yet, otherwise
// a self-contained snapshot. Taking the snapshot clears the scanner's recorded
// evidence; the next scan starts from an empty trace.
YR_API int yr_scanner_take_condition_trace(
    YR_SCANNER* scanner,
    YR_CONDITION_TRACE** out);

// Releases a trace snapshot obtained from yr_scanner_take_condition_trace().
YR_API void yr_condition_trace_destroy(YR_CONDITION_TRACE* trace);

#endif
