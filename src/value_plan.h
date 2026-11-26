/*
   Kafel - shared value and temporary slot planning
   -----------------------------------------

   Copyright 2026 Google LLC

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

     http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.

*/

#ifndef KAFEL_VALUE_PLAN_H
#define KAFEL_VALUE_PLAN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "expression.h"

/*
 * BPF classic filters expose BPF_MEMWORDS scratch words.  A comparison keeps
 * its final left and right operands in two reserved slots.  The remaining
 * slots are the planning pool for retained shared values and binary-op
 * spills.  Therefore the capacity test is:
 *
 *     max_live_work_slots + VALUE_PLAN_RESULT_SLOTS <= BPF_MEMWORDS
 *
 * i.e. at most BPF_MEMWORDS - 2 work slots may be live simultaneously.
 */
#define VALUE_PLAN_MEMWORDS 16
#define VALUE_PLAN_RESULT_SLOTS 2
#define VALUE_PLAN_WORK_SLOTS \
  (VALUE_PLAN_MEMWORDS - VALUE_PLAN_RESULT_SLOTS)
#define VALUE_PLAN_LEFT_SLOT (VALUE_PLAN_MEMWORDS - 2)
#define VALUE_PLAN_RIGHT_SLOT (VALUE_PLAN_MEMWORDS - 1)

enum value_plan_word {
  VALUE_PLAN_WORD_HIGH = 0,
  VALUE_PLAN_WORD_LOW = 1,
};

enum value_plan_node_kind {
  VP_NODE_VAR,
  VP_NODE_CONST,
  VP_NODE_OP,
};

enum value_plan_step_type {
  VP_STEP_LOAD_ARG,
  VP_STEP_LOAD_IMM,
  VP_STEP_LOAD_A_SLOT,
  VP_STEP_LOAD_X_SLOT,
  VP_STEP_STORE_A_SLOT,
  VP_STEP_ALU_K,
  VP_STEP_ALU_X,
};

struct value_plan_node {
  int kind;
  int op; /* EXPR_BIT_AND or EXPR_BIT_OR for VP_NODE_OP */
  int left;
  int right;

  /* Variable identity.  word is also part of the identity. */
  int arg;
  int size;
  int word;

  uint32_t value; /* Constant word value */
  uint64_t hash;

  int uses;
  int slot; /* Work slot holding a retained result, or -1 */
};

struct value_plan_step {
  enum value_plan_step_type type;
  int op;
  int slot;
  int arg;
  int word;
  uint32_t value;
};

struct value_plan {
  bool feasible;
  int word;
  int work_slot_limit;

  struct value_plan_node *nodes;
  size_t node_len;
  size_t node_cap;

  struct value_plan_step *steps;
  size_t step_len;
  size_t step_cap;

  int *hash_table;
  size_t hash_cap;

  bool used_slot[VALUE_PLAN_MEMWORDS];
  int max_slot_index;
};

/*
 * Builds a whole-word plan for the two sides of one comparison.  The returned
 * plan owns all memory and must be released with value_plan_destroy().  On an
 * unsupported expression or slot pressure, plan->feasible is false; callers
 * may then use the original tree emitter.
 */
struct value_plan *value_plan_build(struct expr_tree *left,
                                    struct expr_tree *right, int word);
struct value_plan *value_plan_build_load(struct expr_tree *value, int word);
void value_plan_destroy(struct value_plan *plan);

#endif /* KAFEL_VALUE_PLAN_H */
