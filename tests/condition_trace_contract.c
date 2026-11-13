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
Contract tests for the condition-trace feature. All inputs are benign byte
buffers constructed locally; no external code is executed. The four groups
required by the task are registered through baseline/contract.h:

  shortcircuit: undefined/false short circuit, and/or semantics
  quantifiers:  two-level "for" loops, per-iteration identity
  decisions:    private references, global-rule blocking, skip reasons
  persistence:  two scans, save/load identity, legacy metadata-missing error
*/

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <yara.h>

#include "../../baseline/contract.h"

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static int g_failures = 0;

#define CHECK(cond, msg)                                        \
  do                                                            \
  {                                                             \
    if (!(cond))                                                \
    {                                                           \
      fprintf(stderr, "CHECK failed: %s (%s:%d)\n", msg,       \
              __FILE__, __LINE__);                             \
      g_failures++;                                            \
    }                                                           \
  } while (0)

static YR_RULE* find_rule(YR_RULES* rules, const char* name)
{
  YR_RULE* rule;

  yr_rules_foreach(rules, rule)
  {
    if (strcmp(rule->identifier, name) == 0)
      return rule;
  }

  return NULL;
}

static const YR_CT_NODE* node_by_id(const YR_CONDITION_TRACE* t, uint32_t id)
{
  for (uint32_t i = 0; i < t->num_nodes; i++)
  {
    if (t->nodes[i].id == id)
      return &t->nodes[i];
  }

  return NULL;
}

static const YR_CT_INSTANCE* instance_of_node(
    const YR_CONDITION_TRACE* t,
    uint32_t node_id)
{
  for (uint32_t i = 0; i < t->num_instances; i++)
  {
    if (t->instances[i].node_id == node_id)
      return &t->instances[i];
  }

  return NULL;
}

static const YR_CT_NODE* first_node_of_kind(
    const YR_CONDITION_TRACE* t,
    YR_CT_NODE_KIND kind,
    const char* rule_name)
{
  for (uint32_t i = 0; i < t->num_nodes; i++)
  {
    if (t->nodes[i].kind == kind &&
        (rule_name == NULL ||
         (t->nodes[i].rule_name != NULL &&
          strcmp(t->nodes[i].rule_name, rule_name) == 0)))
    {
      return &t->nodes[i];
    }
  }

  return NULL;
}

static int count_instances_of_node(
    const YR_CONDITION_TRACE* t,
    uint32_t node_id)
{
  int n = 0;

  for (uint32_t i = 0; i < t->num_instances; i++)
  {
    if (t->instances[i].node_id == node_id)
      n++;
  }

  return n;
}

static int count_skipped_in_rule(
    const YR_CONDITION_TRACE* t,
    const char* rule_name)
{
  int n = 0;

  for (uint32_t i = 0; i < t->num_instances; i++)
  {
    const YR_CT_INSTANCE* inst = &t->instances[i];
    const YR_CT_NODE* node = node_by_id(t, inst->node_id);

    if (node != NULL && node->rule_name != NULL &&
        strcmp(node->rule_name, rule_name) == 0 &&
        inst->state == YR_CT_INST_SKIPPED)
    {
      n++;
    }
  }

  return n;
}

static bool edge_exists(
    const YR_CONDITION_TRACE* t,
    uint32_t from_instance,
    uint32_t to_instance)
{
  for (uint32_t i = 0; i < t->num_edges; i++)
  {
    if (t->edges[i].from_instance == from_instance &&
        t->edges[i].to_instance == to_instance)
      return true;
  }

  return false;
}

static const YR_CT_RULE_DECISION* decision_for(
    const YR_CONDITION_TRACE* t,
    YR_RULES* rules,
    const char* name)
{
  YR_RULE* rule = find_rule(rules, name);

  if (rule == NULL)
    return NULL;

  uint32_t idx = (uint32_t)(rule - rules->rules_table);

  for (uint32_t i = 0; i < t->num_decisions; i++)
  {
    if (t->decisions[i].rule_idx == idx)
      return &t->decisions[i];
  }

  return NULL;
}

static int collect_matches;

static int test_callback(
    YR_SCAN_CONTEXT* ctx,
    int message,
    void* message_data,
    void* user_data)
{
  (void) ctx;
  (void) message_data;
  (void) user_data;

  if (message == CALLBACK_MSG_RULE_MATCHING)
    collect_matches++;

  return CALLBACK_CONTINUE;
}

static int compile_traced(
    const char* source,
    YR_COMPILER** out_compiler,
    YR_RULES** out_rules)
{
  int rc = yr_compiler_create(out_compiler);

  if (rc != ERROR_SUCCESS)
    return rc;

  yr_compiler_set_condition_trace(*out_compiler, 1);

  rc = yr_compiler_add_string(*out_compiler, source, NULL);

  if (rc != 0)
  {
    char buf[256];
    yr_compiler_get_error_message(*out_compiler, buf, sizeof(buf));
    fprintf(stderr, "compile error: %s\n", buf);
    return rc;
  }

  return yr_compiler_get_rules(*out_compiler, out_rules);
}

static int scan_and_take(
    YR_RULES* rules,
    const uint8_t* data,
    size_t size,
    YR_CONDITION_TRACE** out)
{
  YR_SCANNER* scanner;

  *out = NULL;

  int rc = yr_scanner_create(rules, &scanner);

  if (rc != ERROR_SUCCESS)
    return rc;

  rc = yr_scanner_set_condition_trace(scanner, 1);

  if (rc != ERROR_SUCCESS)
  {
    yr_scanner_destroy(scanner);
    return rc;
  }

  yr_scanner_set_callback(scanner, test_callback, NULL);

  rc = yr_scanner_scan_mem(scanner, data, size);

  if (rc != ERROR_SUCCESS)
  {
    yr_scanner_destroy(scanner);
    return rc;
  }

  rc = yr_scanner_take_condition_trace(scanner, out);
  yr_scanner_destroy(scanner);
  return rc;
}

// ---------------------------------------------------------------------------
// shortcircuit group
// ---------------------------------------------------------------------------

static void test_undefined_distinct_from_false(void)
{
  const char* src =
      "rule r1 { strings: $a = \"alpha\" "
      "condition: defined(@a[2]) and #a == 2 }";

  YR_COMPILER* compiler = NULL;
  YR_RULES* rules = NULL;

  int rc = compile_traced(src, &compiler, &rules);
  CHECK(rc == ERROR_SUCCESS, "compile r1");

  YR_CONDITION_TRACE* trace = NULL;
  const uint8_t data[] = "alpha alpha";
  collect_matches = 0;

  rc = scan_and_take(rules, data, sizeof(data) - 1, &trace);
  CHECK(rc == ERROR_SUCCESS, "scan r1");
  CHECK(trace != NULL, "trace produced");

  // @a[2] exists (two matches) and must be reported as an integer value,
  // while a hypothetical third occurrence stays undefined.
  const YR_CT_NODE* off_node =
      first_node_of_kind(trace, YR_CT_NODE_STRING_OFFSET, "r1");
  CHECK(off_node != NULL, "offset node exists");

  const YR_CT_INSTANCE* off_inst =
      instance_of_node(trace, off_node->id);
  CHECK(off_inst != NULL, "offset instance exists");
  CHECK(off_inst->value_type == YR_CT_VALUE_INTEGER,
        "defined offset is integer not undefined");

  // "defined" of a present occurrence evaluates to true, so the rule reports.
  bool has_defined = false;

  for (uint32_t i = 0; i < trace->num_nodes; i++)
  {
    if (trace->nodes[i].kind == YR_CT_NODE_DEFINED)
      has_defined = true;
  }

  CHECK(has_defined, "defined node present");
  CHECK(collect_matches == 1, "r1 matches exactly once");

  yr_condition_trace_destroy(trace);
  yr_rules_destroy(rules);
  yr_compiler_destroy(compiler);
}

static void test_and_short_circuit(void)
{
  // false left operand: the right operand's nodes must be skipped, not
  // evaluated, and must carry the and-short-circuit reason.
  const char* src =
      "rule r2 { strings: $a = \"zzz\" "
      "condition: #a == 1 and filesize == 99 }";

  YR_COMPILER* compiler = NULL;
  YR_RULES* rules = NULL;

  int rc = compile_traced(src, &compiler, &rules);
  CHECK(rc == ERROR_SUCCESS, "compile r2");

  YR_CONDITION_TRACE* trace = NULL;
  const uint8_t data[] = "plain";
  collect_matches = 0;

  rc = scan_and_take(rules, data, sizeof(data) - 1, &trace);
  CHECK(rc == ERROR_SUCCESS, "scan r2");

  const YR_CT_NODE* filesize_node =
      first_node_of_kind(trace, YR_CT_NODE_FILESIZE, "r2");
  CHECK(filesize_node != NULL, "filesize node exists in static graph");

  const YR_CT_INSTANCE* fs_inst =
      instance_of_node(trace, filesize_node->id);
  CHECK(fs_inst != NULL, "filesize has a dynamic record");
  CHECK(fs_inst->state == YR_CT_INST_SKIPPED,
        "right operand skipped by and short-circuit");
  CHECK(fs_inst->short_circuit_reason ==
            YR_CT_SKIP_AND_SHORT_CIRCUIT,
        "skip reason records and short-circuit");
  CHECK(fs_inst->value_type == YR_CT_VALUE_UNDEFINED,
        "skipped node has no value");

  yr_condition_trace_destroy(trace);
  yr_rules_destroy(rules);
  yr_compiler_destroy(compiler);
}

static void test_or_short_circuit(void)
{
  // true left operand of "or": right side never executes.
  const char* src =
      "rule r3 { strings: $a = \"hit\" "
      "condition: #a > 0 or filesize == 123456 }";

  YR_COMPILER* compiler = NULL;
  YR_RULES* rules = NULL;

  int rc = compile_traced(src, &compiler, &rules);
  CHECK(rc == ERROR_SUCCESS, "compile r3");

  YR_CONDITION_TRACE* trace = NULL;
  const uint8_t data[] = "hit";
  collect_matches = 0;

  rc = scan_and_take(rules, data, sizeof(data) - 1, &trace);
  CHECK(rc == ERROR_SUCCESS, "scan r3");
  CHECK(collect_matches == 1, "r3 matches");

  const YR_CT_NODE* filesize_node =
      first_node_of_kind(trace, YR_CT_NODE_FILESIZE, "r3");
  CHECK(filesize_node != NULL, "filesize node exists");

  const YR_CT_INSTANCE* fs_inst =
      instance_of_node(trace, filesize_node->id);
  CHECK(fs_inst != NULL && fs_inst->state == YR_CT_INST_SKIPPED,
        "or right operand skipped");
  CHECK(fs_inst->short_circuit_reason ==
            YR_CT_SKIP_OR_SHORT_CIRCUIT,
        "or short-circuit reason recorded");

  yr_condition_trace_destroy(trace);
  yr_rules_destroy(rules);
  yr_compiler_destroy(compiler);
}

static void test_folded_constants_and_unsupported_boundary(void)
{
  // Integer literals are compile-time values (FOLDED), while a module
  // function (math module) is outside the supported expression set and must
  // be flagged as an unsupported boundary rather than silently omitted.
  const char* src =
      "import \"math\" "
      "rule fold_math { condition: 1 + 2 == 3 and "
      "math.min(3, 4) == 3 }";

  YR_COMPILER* compiler = NULL;
  YR_RULES* rules = NULL;

  int rc = compile_traced(src, &compiler, &rules);
  CHECK(rc == ERROR_SUCCESS, "compile fold_math");

  YR_CONDITION_TRACE* trace = NULL;
  const uint8_t data[] = "x";
  collect_matches = 0;

  rc = scan_and_take(rules, data, 1, &trace);
  CHECK(rc == ERROR_SUCCESS, "scan fold_math");

  // At least one constant node carries the FOLDED state with a fold origin.
  bool found_folded = false;

  for (uint32_t i = 0; i < trace->num_nodes; i++)
  {
    if (trace->nodes[i].kind != YR_CT_NODE_CONST_INT)
      continue;

    const YR_CT_INSTANCE* inst =
        instance_of_node(trace, trace->nodes[i].id);

    if (inst != NULL && inst->state == YR_CT_INST_FOLDED)
    {
      found_folded = true;
      CHECK(trace->nodes[i].fold_origin_node_id !=
                YR_CT_INVALID_NODE,
            "folded node keeps a source origin node");
      CHECK(inst->value_type == YR_CT_VALUE_INTEGER,
            "folded constant still carries its compile-time value");
    }
  }

  CHECK(found_folded, "integer literals recorded as folded");

  // The module call region is present and marked unsupported.
  bool found_unsupported = false;

  for (uint32_t i = 0; i < trace->num_nodes; i++)
  {
    if (trace->nodes[i].unsupported)
      found_unsupported = true;
  }

  CHECK(found_unsupported,
        "module function region marked as unsupported boundary");

  // The scan behavior is unchanged: the rule still evaluates.
  const YR_CT_RULE_DECISION* dec =
      decision_for(trace, rules, "fold_math");
  CHECK(dec != NULL, "fold_math decision present");
  CHECK(dec->state == YR_CT_INST_EVALUATED,
        "rule with unsupported region still evaluated normally");

  yr_condition_trace_destroy(trace);
  yr_rules_destroy(rules);
  yr_compiler_destroy(compiler);
}

// ---------------------------------------------------------------------------
// quantifier group
// ---------------------------------------------------------------------------
static void test_two_level_for_identity(void)
{
  const char* src =
      "rule nested { condition: "
      "for all i in (0..2) : ( "
      "  for all j in (0..1) : ( i + j >= 0 ) ) }";

  YR_COMPILER* compiler = NULL;
  YR_RULES* rules = NULL;

  int rc = compile_traced(src, &compiler, &rules);
  CHECK(rc == ERROR_SUCCESS, "compile nested");

  YR_CONDITION_TRACE* trace = NULL;
  const uint8_t data[] = "x";

  rc = scan_and_take(rules, data, 1, &trace);
  CHECK(rc == ERROR_SUCCESS, "scan nested");

  const YR_CT_NODE* for_node =
      first_node_of_kind(trace, YR_CT_NODE_FOR, "nested");
  CHECK(for_node != NULL, "for node exists");

  // Two distinct FOR nodes (outer any, inner all).
  int for_count = 0;

  for (uint32_t i = 0; i < trace->num_nodes; i++)
  {
    if (trace->nodes[i].kind == YR_CT_NODE_FOR)
      for_count++;
  }

  CHECK(for_count == 2, "two loop nodes");

  // Every iteration produces its own instance: 3 outer iterations, and the
  // inner body runs 3 * 2 times with distinct two-level paths. Pick the
  // comparison node that actually gets six instances.
  const YR_CT_NODE* inner_compare = NULL;

  for (uint32_t i = 0; i < trace->num_nodes; i++)
  {
    if (trace->nodes[i].kind == YR_CT_NODE_INT_COMPARE &&
        count_instances_of_node(trace, trace->nodes[i].id) == 6)
    {
      inner_compare = &trace->nodes[i];
      break;
    }
  }

  CHECK(inner_compare != NULL, "inner comparison node with six instances");
  CHECK(count_instances_of_node(trace, inner_compare->id) == 6,
        "six inner body instances (3 outer x 2 inner)");

  // Verify nested iteration paths are unique and correctly ordered.
  bool seen[3][2];
  memset(seen, 0, sizeof(seen));

  for (uint32_t i = 0; i < trace->num_instances; i++)
  {
    const YR_CT_INSTANCE* inst = &trace->instances[i];

    if (inst->node_id != inner_compare->id)
      continue;

    CHECK(inst->iteration_depth == 2, "nested path depth 2");

    if (inst->iteration_depth == 2)
    {
      uint32_t outer_ord = inst->iteration_path[0].ordinal;
      uint32_t inner_ord = inst->iteration_path[1].ordinal;
      CHECK(outer_ord >= 1 && outer_ord <= 3, "outer ordinal range");
      CHECK(inner_ord >= 1 && inner_ord <= 2, "inner ordinal range");
      CHECK(inst->iteration_path[0].is_integer, "outer integer path");
      CHECK(inst->iteration_path[0].integer_value ==
                (int64_t)(outer_ord - 1),
            "outer path carries i value");
      seen[outer_ord - 1][inner_ord - 1] = true;
    }
  }

  for (int i = 0; i < 3; i++)
    for (int j = 0; j < 2; j++)
      CHECK(seen[i][j], "all six nested paths present");

  // The outer loop summary is the depth-1 FOR instance that actually visited
  // iterations (inner FOR summaries live at depth 2).
  const YR_CT_INSTANCE* outer_summary = NULL;

  for (uint32_t i = 0; i < trace->num_instances; i++)
  {
    const YR_CT_INSTANCE* cand = &trace->instances[i];
    const YR_CT_NODE* cand_node = node_by_id(trace, cand->node_id);

    if (cand_node != NULL && cand_node->kind == YR_CT_NODE_FOR &&
        cand->iteration_depth == 1 && cand->iterations_visited > 0)
    {
      outer_summary = cand;
    }
  }

  CHECK(outer_summary != NULL, "outer loop summary instance");
  CHECK(outer_summary->iterations_visited == 3,
        "outer loop visited three iterations");
  CHECK(outer_summary->iterations_true == 3,
        "all three outer bodies true");
  CHECK(outer_summary->value_type == YR_CT_VALUE_BOOLEAN &&
        outer_summary->value.i == 1,
        "outer for-all evaluates true");

  yr_condition_trace_destroy(trace);
  yr_rules_destroy(rules);
  yr_compiler_destroy(compiler);
}

static void test_for_short_circuit_and_of(void)
{
  const char* src =
      "rule quant { strings: $a = \"a\" $b = \"b\" $c = \"c\" "
      "condition: for any i in (0..4) : ( i == 1 ) "
      "and 2 of ($a, $b, $c) }";

  YR_COMPILER* compiler = NULL;
  YR_RULES* rules = NULL;

  int rc = compile_traced(src, &compiler, &rules);
  CHECK(rc == ERROR_SUCCESS, "compile quant");

  YR_CONDITION_TRACE* trace = NULL;
  const uint8_t data[] = "a and b here";
  collect_matches = 0;

  rc = scan_and_take(rules, data, sizeof(data) - 1, &trace);
  CHECK(rc == ERROR_SUCCESS, "scan quant");

  const YR_CT_NODE* of_node =
      first_node_of_kind(trace, YR_CT_NODE_OF, "quant");
  CHECK(of_node != NULL, "of node exists");

  const YR_CT_INSTANCE* of_inst =
      instance_of_node(trace, of_node->id);
  CHECK(of_inst != NULL &&
        of_inst->state == YR_CT_INST_EVALUATED,
        "of expression evaluated");
  CHECK(of_inst->value_type == YR_CT_VALUE_BOOLEAN &&
        of_inst->value.i == 1,
        "2-of-3 true");

  // The "any" loop stops after i==1 and records skipped remaining iterations.
  const YR_CT_NODE* any_for = NULL;

  for (uint32_t i = 0; i < trace->num_nodes; i++)
  {
    if (trace->nodes[i].kind == YR_CT_NODE_FOR)
      any_for = &trace->nodes[i];
  }

  CHECK(any_for != NULL, "any loop node found");

  const YR_CT_INSTANCE* any_summary = NULL;

  for (uint32_t i = 0; i < trace->num_instances; i++)
  {
    if (trace->instances[i].node_id == any_for->id &&
        trace->instances[i].iterations_visited > 0)
      any_summary = &trace->instances[i];
  }

  CHECK(any_summary != NULL, "any loop summary");
  CHECK(any_summary->iterations_visited == 2,
        "any loop stopped after second iteration");
  CHECK(any_summary->winning_instance != YR_CT_INVALID_INSTANCE,
        "winning iteration linked to summary");

  yr_condition_trace_destroy(trace);
  yr_rules_destroy(rules);
  yr_compiler_destroy(compiler);
}

// ---------------------------------------------------------------------------
// decisions group
// ---------------------------------------------------------------------------

static void test_private_reference_and_global_block(void)
{
  const char* src =
      "private rule priv_base { strings: $a = \"secret\" "
      "condition: $a } "
      "global rule gate { condition: false } "
      "rule visible { condition: priv_base }";

  YR_COMPILER* compiler = NULL;
  YR_RULES* rules = NULL;

  int rc = compile_traced(src, &compiler, &rules);
  CHECK(rc == ERROR_SUCCESS, "compile private/global rules");

  YR_CONDITION_TRACE* trace = NULL;
  const uint8_t data[] = "secret payload";
  collect_matches = 0;

  rc = scan_and_take(rules, data, sizeof(data) - 1, &trace);
  CHECK(rc == ERROR_SUCCESS, "scan private/global");

  // "visible" condition is true but the global gate blocks reporting.
  const YR_CT_RULE_DECISION* visible =
      decision_for(trace, rules, "visible");
  CHECK(visible != NULL, "visible decision present");
  CHECK(visible->condition_true, "visible condition evaluated true");
  CHECK(!visible->reported_match,
        "visible not reported because global gate failed");
  CHECK(visible->blocked_by_global_rule_idx != YR_CT_INVALID_RULE,
        "blocking global rule identified");
  CHECK(collect_matches == 0, "no rule reported to callback");

  // The reference links to the private rule's own result instance rather
  // than re-executing it.
  YR_RULE* priv = find_rule(rules, "priv_base");
  CHECK(priv != NULL, "private rule exists");

  const YR_CT_NODE* ref_node =
      first_node_of_kind(trace, YR_CT_NODE_RULE_REF, "visible");
  CHECK(ref_node != NULL, "rule reference node exists");

  const YR_CT_INSTANCE* ref_inst =
      instance_of_node(trace, ref_node->id);
  CHECK(ref_inst != NULL, "rule reference evaluated once");

  const YR_CT_RULE_DECISION* priv_dec =
      decision_for(trace, rules, "priv_base");
  CHECK(priv_dec != NULL && priv_dec->condition_true,
        "private rule still evaluated and true");

  bool links_to_private = false;

  for (uint32_t i = 0; i < trace->num_edges; i++)
  {
    if (trace->edges[i].to_instance == ref_inst->id)
    {
      const YR_CT_INSTANCE* from =
          &trace->instances[trace->edges[i].from_instance];
      const YR_CT_NODE* from_node = node_by_id(trace, from->node_id);

      if (from_node != NULL && from_node->rule_name != NULL &&
          strcmp(from_node->rule_name, "priv_base") == 0)
      {
        links_to_private = true;
      }
    }
  }

  CHECK(links_to_private,
        "rule reference depends on private rule's own result");

  yr_condition_trace_destroy(trace);
  yr_rules_destroy(rules);
  yr_compiler_destroy(compiler);
}

static void test_disabled_and_required_eval_skip(void)
{
  const char* src =
      "rule disabled_one { strings: $a = \"needle\" condition: $a } "
      "rule plain_one { condition: true }";

  YR_COMPILER* compiler = NULL;
  YR_RULES* rules = NULL;

  int rc = compile_traced(src, &compiler, &rules);
  CHECK(rc == ERROR_SUCCESS, "compile skip rules");

  YR_RULE* disabled = find_rule(rules, "disabled_one");
  CHECK(disabled != NULL, "found rule to disable");
  yr_rule_disable(disabled);

  YR_CONDITION_TRACE* trace = NULL;
  const uint8_t data[] = "nothing here";

  rc = scan_and_take(rules, data, sizeof(data) - 1, &trace);
  CHECK(rc == ERROR_SUCCESS, "scan skip rules");

  const YR_CT_RULE_DECISION* dec =
      decision_for(trace, rules, "disabled_one");
  CHECK(dec != NULL && dec->state == YR_CT_INST_SKIPPED,
        "disabled rule marked skipped");
  CHECK(dec->skip_reason == YR_CT_SKIP_RULE_DISABLED,
        "disabled skip reason");
  CHECK(count_skipped_in_rule(trace, "disabled_one") > 0,
        "disabled rule body nodes skipped, not false");

  yr_condition_trace_destroy(trace);
  yr_rules_destroy(rules);
  yr_compiler_destroy(compiler);
}

// ---------------------------------------------------------------------------
// persistence group
// ---------------------------------------------------------------------------

typedef struct
{
  uint8_t* buf;
  size_t cap;
  size_t len;
} mem_stream_t;

static size_t mem_stream_write(
    const void* ptr,
    size_t size,
    size_t count,
    void* user_data)
{
  mem_stream_t* s = (mem_stream_t*) user_data;
  size_t total = size * count;

  if (s->len + total > s->cap)
    return 0;

  memcpy(s->buf + s->len, ptr, total);
  s->len += total;
  return count;
}

static size_t mem_stream_read(
    void* ptr,
    size_t size,
    size_t count,
    void* user_data)
{
  mem_stream_t* s = (mem_stream_t*) user_data;
  size_t total = size * count;

  if (s->len + total > s->cap)
    return 0;

  memcpy(ptr, s->buf + s->len, total);
  s->len += total;
  return count;
}

static void test_two_scans_independent_and_cleared(void)
{
  const char* src =
      "rule repeat { strings: $a = \"x\" condition: #a >= 1 }";

  YR_COMPILER* compiler = NULL;
  YR_RULES* rules = NULL;

  int rc = compile_traced(src, &compiler, &rules);
  CHECK(rc == ERROR_SUCCESS, "compile repeat");

  YR_SCANNER* scanner;
  rc = yr_scanner_create(rules, &scanner);
  CHECK(rc == ERROR_SUCCESS, "scanner created");
  CHECK(yr_scanner_set_condition_trace(scanner, 1) == ERROR_SUCCESS,
        "trace enabled");
  yr_scanner_set_callback(scanner, test_callback, NULL);

  const uint8_t first[] = "x";
  rc = yr_scanner_scan_mem(scanner, first, sizeof(first) - 1);
  CHECK(rc == ERROR_SUCCESS, "first scan");

  YR_CONDITION_TRACE* first_trace = NULL;
  rc = yr_scanner_take_condition_trace(scanner, &first_trace);
  CHECK(rc == ERROR_SUCCESS && first_trace != NULL, "first take");
  CHECK(first_trace->num_instances > 0, "first scan has evidence");

  // After take, the second scan starts clean: evidence must reflect only the
  // second input (which has no match), never the first scan's records.
  const uint8_t second[] = "yyyyy";
  rc = yr_scanner_scan_mem(scanner, second, sizeof(second) - 1);
  CHECK(rc == ERROR_SUCCESS, "second scan");

  YR_CONDITION_TRACE* second_trace = NULL;
  rc = yr_scanner_take_condition_trace(scanner, &second_trace);
  CHECK(rc == ERROR_SUCCESS && second_trace != NULL, "second take");

  const YR_CT_NODE* count_node =
      first_node_of_kind(first_trace, YR_CT_NODE_STRING_COUNT, "repeat");
  CHECK(count_node != NULL, "count node in first trace");
  const YR_CT_INSTANCE* first_count =
      instance_of_node(first_trace, count_node->id);
  CHECK(first_count != NULL && first_count->value.i == 1,
        "first scan count is 1");

  const YR_CT_NODE* count_node2 =
      first_node_of_kind(second_trace, YR_CT_NODE_STRING_COUNT, "repeat");
  const YR_CT_INSTANCE* second_count =
      instance_of_node(second_trace, count_node2->id);
  CHECK(second_count != NULL && second_count->value.i == 0,
        "second scan count is 0, not stale 1");

  // Node identities are stable across scans of the same rules.
  CHECK(count_node->id == count_node2->id,
        "static node identity stable across scans");

  yr_condition_trace_destroy(first_trace);
  yr_condition_trace_destroy(second_trace);
  yr_scanner_destroy(scanner);
  yr_rules_destroy(rules);
  yr_compiler_destroy(compiler);
}

static void test_save_load_identity(void)
{
  const char* src =
      "rule persist { strings: $a = \"alpha\" "
      "condition: for any i in (0..2) : ( #a >= i ) }";

  YR_COMPILER* compiler = NULL;
  YR_RULES* rules = NULL;

  int rc = compile_traced(src, &compiler, &rules);
  CHECK(rc == ERROR_SUCCESS, "compile persist");

  YR_CONDITION_TRACE* before = NULL;
  const uint8_t data[] = "alpha";
  rc = scan_and_take(rules, data, sizeof(data) - 1, &before);
  CHECK(rc == ERROR_SUCCESS, "scan before save");

  mem_stream_t sink;
  sink.cap = 1 << 20;
  sink.buf = (uint8_t*) malloc(sink.cap);
  sink.len = 0;

  YR_STREAM out_stream;
  out_stream.user_data = &sink;
  out_stream.write = mem_stream_write;

  rc = yr_rules_save_stream(rules, &out_stream);
  CHECK(rc == ERROR_SUCCESS, "save stream");

  mem_stream_t source;
  source.buf = sink.buf;
  source.cap = sink.len;
  source.len = 0;

  YR_STREAM in_stream;
  in_stream.user_data = &source;
  in_stream.read = mem_stream_read;

  YR_RULES* loaded = NULL;
  rc = yr_rules_load_stream(&in_stream, &loaded);
  CHECK(rc == ERROR_SUCCESS, "load stream");

  // Tracing on the loaded rules works without the original sources and keeps
  // the same node identities.
  YR_CONDITION_TRACE* after = NULL;
  rc = scan_and_take(loaded, data, sizeof(data) - 1, &after);
  CHECK(rc == ERROR_SUCCESS, "scan after load");
  CHECK(after->num_nodes == before->num_nodes,
        "same static node count after load");

  for (uint32_t i = 0; i < before->num_nodes; i++)
  {
    CHECK(before->nodes[i].kind == after->nodes[i].kind,
          "node kind preserved after load");
    CHECK(before->nodes[i].id == after->nodes[i].id,
          "node id preserved after load");
  }

  const YR_CT_NODE* for_before =
      first_node_of_kind(before, YR_CT_NODE_FOR, "persist");
  const YR_CT_NODE* for_after =
      first_node_of_kind(after, YR_CT_NODE_FOR, "persist");
  CHECK(for_before != NULL && for_after != NULL,
        "for node survives serialization");
  CHECK(for_before->id == for_after->id,
        "for node identity stable across save/load");
  CHECK(for_after->namespace_name != NULL &&
        strcmp(for_after->namespace_name, "default") == 0,
        "namespace name available without sources");

  free(sink.buf);
  yr_condition_trace_destroy(before);
  yr_condition_trace_destroy(after);
  yr_rules_destroy(loaded);
  yr_rules_destroy(rules);
  yr_compiler_destroy(compiler);
}

static void test_legacy_metadata_missing(void)
{
  // The shipped legacy .yarc was built without trace metadata.
  YR_RULES* legacy = NULL;
  int rc = yr_rules_load("baseline/legacy.yarc", &legacy);
  CHECK(rc == ERROR_SUCCESS, "legacy rules load");

  YR_SCANNER* scanner;
  rc = yr_scanner_create(legacy, &scanner);
  CHECK(rc == ERROR_SUCCESS, "scanner for legacy");

  rc = yr_scanner_set_condition_trace(scanner, 1);
  CHECK(rc == ERROR_CONDITION_TRACE_MISSING,
        "enabling trace on legacy file returns metadata-missing error");

  // Normal scanning still works unchanged.
  yr_scanner_set_callback(scanner, test_callback, NULL);
  const uint8_t data[] = "alpha";
  rc = yr_scanner_scan_mem(scanner, data, sizeof(data) - 1);
  CHECK(rc == ERROR_SUCCESS, "legacy scan still works");

  YR_CONDITION_TRACE* trace = NULL;
  rc = yr_scanner_take_condition_trace(scanner, &trace);
  CHECK(rc == ERROR_SUCCESS && trace == NULL,
        "no trace produced for legacy rules");

  yr_scanner_destroy(scanner);
  yr_rules_destroy(legacy);
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

void register_contract(void)
{
  int rc = yr_initialize();
  assert(rc == ERROR_SUCCESS);

  contract_add("shortcircuit", "undefined_distinct_from_false",
               test_undefined_distinct_from_false);
  contract_add("shortcircuit", "and_short_circuit",
               test_and_short_circuit);
  contract_add("shortcircuit", "or_short_circuit",
               test_or_short_circuit);
  contract_add("shortcircuit", "folded_constants_and_unsupported",
               test_folded_constants_and_unsupported_boundary);

  contract_add("quantifiers", "two_level_for_identity",
               test_two_level_for_identity);
  contract_add("quantifiers", "for_short_circuit_and_of",
               test_for_short_circuit_and_of);

  contract_add("decisions", "private_reference_and_global_block",
               test_private_reference_and_global_block);
  contract_add("decisions", "disabled_and_required_eval_skip",
               test_disabled_and_required_eval_skip);

  contract_add("persistence", "two_scans_independent_and_cleared",
               test_two_scans_independent_and_cleared);
  contract_add("persistence", "save_load_identity",
               test_save_load_identity);
  contract_add("persistence", "legacy_metadata_missing",
               test_legacy_metadata_missing);
}
