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

#include "value_plan.h"

#include <stdlib.h>
#include <string.h>

#include "common.h"

#define VP_INITIAL_NODES 32
#define VP_INITIAL_STEPS 64
#define VP_INITIAL_HASH 64

static uint64_t vp_mix(uint64_t x) {
  x += 0x9e3779b97f4a7c15ULL;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
  return x ^ (x >> 31);
}

static uint64_t vp_hash_combine(uint64_t a, uint64_t b) {
  return vp_mix(a ^ (vp_mix(b) + 0x9e3779b97f4a7c15ULL + (a << 6) + (a >> 2)));
}

static bool vp_identity_equal(const struct value_plan_node *a,
                              const struct value_plan_node *b) {
  if (a->kind != b->kind || a->hash != b->hash) {
    return false;
  }
  switch (a->kind) {
    case VP_NODE_CONST:
      return a->value == b->value;
    case VP_NODE_VAR:
      return a->arg == b->arg && a->size == b->size && a->word == b->word;
    case VP_NODE_OP:
      return a->op == b->op && a->left == b->left && a->right == b->right;
  }
  return false;
}

static int vp_append_node(struct value_plan *plan,
                          const struct value_plan_node *key) {
  if (plan->node_len == plan->node_cap) {
    size_t new_cap = plan->node_cap == 0 ? VP_INITIAL_NODES
                                         : plan->node_cap * 2;
    struct value_plan_node *new_nodes =
        realloc(plan->nodes, new_cap * sizeof(*new_nodes));
    if (new_nodes == NULL) {
      return -1;
    }
    plan->nodes = new_nodes;
    plan->node_cap = new_cap;
  }
  int id = (int)plan->node_len++;
  plan->nodes[id] = *key;
  plan->nodes[id].uses = 0;
  plan->nodes[id].slot = -1;
  return id;
}

static int vp_hash_lookup(struct value_plan *plan,
                          const struct value_plan_node *key) {
  uint64_t mask = plan->hash_cap - 1;
  size_t pos = key->hash & mask;
  while (plan->hash_table[pos] != 0) {
    int id = plan->hash_table[pos] - 1;
    if (vp_identity_equal(&plan->nodes[id], key)) {
      return id;
    }
    pos = (pos + 1) & mask;
  }
  return -1;
}

static bool vp_hash_insert_id(struct value_plan *plan, int id) {
  if ((plan->node_len + 1) * 10 >= plan->hash_cap * 7) {
    size_t old_cap = plan->hash_cap;
    size_t new_cap = old_cap * 2;
    int *new_table = calloc(new_cap, sizeof(*new_table));
    if (new_table == NULL) {
      return false;
    }
    int *old_table = plan->hash_table;
    plan->hash_table = new_table;
    plan->hash_cap = new_cap;
    for (size_t i = 0; i < old_cap; ++i) {
      if (old_table[i] == 0) {
        continue;
      }
      int old_id = old_table[i] - 1;
      size_t pos = plan->nodes[old_id].hash & (new_cap - 1);
      while (new_table[pos] != 0) {
        pos = (pos + 1) & (new_cap - 1);
      }
      new_table[pos] = old_id + 1;
    }
    free(old_table);
  }

  size_t pos = plan->nodes[id].hash & (plan->hash_cap - 1);
  while (plan->hash_table[pos] != 0) {
    pos = (pos + 1) & (plan->hash_cap - 1);
  }
  plan->hash_table[pos] = id + 1;
  return true;
}

static int vp_intern(struct value_plan *plan,
                     const struct value_plan_node *key) {
  int existing = vp_hash_lookup(plan, key);
  if (existing >= 0) {
    return existing;
  }
  int id = vp_append_node(plan, key);
  if (id < 0) {
    return -1;
  }
  if (!vp_hash_insert_id(plan, id)) {
    return -1;
  }
  return id;
}

/*
 * A deterministic semantic order.  Hashes are only an ordering shortcut;
 * equal hashes are resolved structurally.  This makes swapped commutative
 * operands receive one canonical identity without special-casing any policy.
 */
static int vp_compare_nodes(struct value_plan *plan, int a_id, int b_id) {
  if (a_id == b_id) {
    return 0;
  }
  const struct value_plan_node *a = &plan->nodes[a_id];
  const struct value_plan_node *b = &plan->nodes[b_id];

  if (a->kind != b->kind) {
    return a->kind < b->kind ? -1 : 1;
  }
  if (a->hash != b->hash) {
    return a->hash < b->hash ? -1 : 1;
  }

  switch (a->kind) {
    case VP_NODE_CONST:
      return a->value < b->value ? -1 : (a->value > b->value ? 1 : 0);
    case VP_NODE_VAR:
      if (a->arg != b->arg) return a->arg < b->arg ? -1 : 1;
      if (a->size != b->size) return a->size < b->size ? -1 : 1;
      return a->word < b->word ? -1 : (a->word > b->word ? 1 : 0);
    case VP_NODE_OP: {
      if (a->op != b->op) return a->op < b->op ? -1 : 1;
      int left = vp_compare_nodes(plan, a->left, b->left);
      if (left != 0) return left;
      return vp_compare_nodes(plan, a->right, b->right);
    }
  }
  return 0;
}

static uint32_t vp_word_const(uint64_t number, int word) {
  if (word == VALUE_PLAN_WORD_HIGH) {
    return (uint32_t)((number >> 32) & UINT32_MAX);
  }
  return (uint32_t)(number & UINT32_MAX);
}

static int vp_build_expr(struct value_plan *plan, struct expr_tree *expr,
                         int word) {
  if (expr == NULL) {
    return -1;
  }

  /*
   * cache_constants() has already evaluated word-local constants and
   * idempotent forms such as x & 0.  EXPR_NUMBER is handled directly as well
   * so the planner remains self-contained.
   */
  if (expr->type == EXPR_NUMBER) {
    struct value_plan_node key = {
        .kind = VP_NODE_CONST,
        .word = word,
        .value = vp_word_const(expr->number, word),
    };
    key.hash = vp_mix(VP_NODE_CONST) ^ vp_mix(word) ^ vp_mix(key.value);
    return vp_intern(plan, &key);
  }

  const struct cached_value *cached =
      word == VALUE_PLAN_WORD_HIGH ? &expr->high : &expr->low;
  if (cached->is_const) {
    struct value_plan_node key = {
        .kind = VP_NODE_CONST,
        .word = word,
        .value = cached->value,
    };
    key.hash = vp_mix(VP_NODE_CONST) ^ vp_mix(word) ^ vp_mix(key.value);
    return vp_intern(plan, &key);
  }

  if (expr->type == EXPR_VAR) {
    struct value_plan_node key = {
        .kind = VP_NODE_VAR,
        .word = word,
        .arg = expr->var,
        .size = expr->size,
    };
    key.hash = vp_mix(VP_NODE_VAR) ^ vp_mix(word) ^ vp_mix(expr->var) ^
               vp_mix(expr->size);
    return vp_intern(plan, &key);
  }

  if (expr->type != EXPR_BIT_AND && expr->type != EXPR_BIT_OR) {
    return -1;
  }

  int left = vp_build_expr(plan, expr->left, word);
  int right = vp_build_expr(plan, expr->right, word);
  if (left < 0 || right < 0) {
    return -1;
  }

  /* Put a constant on the right, as BPF immediate ALU forms expect. */
  if (plan->nodes[left].kind == VP_NODE_CONST &&
      plan->nodes[right].kind != VP_NODE_CONST) {
    int tmp = left;
    left = right;
    right = tmp;
  } else if (plan->nodes[left].kind != VP_NODE_CONST &&
             plan->nodes[right].kind != VP_NODE_CONST &&
             vp_compare_nodes(plan, left, right) > 0) {
    int tmp = left;
    left = right;
    right = tmp;
  }

  struct value_plan_node key = {
      .kind = VP_NODE_OP,
      .op = expr->type,
      .left = left,
      .right = right,
      .word = word,
  };
  key.hash = vp_mix(VP_NODE_OP) ^ vp_mix(expr->type) ^
             vp_hash_combine(plan->nodes[left].hash, plan->nodes[right].hash);
  return vp_intern(plan, &key);
}

static bool vp_add_step(struct value_plan *plan,
                        const struct value_plan_step *step) {
  if (plan->step_len == plan->step_cap) {
    size_t new_cap = plan->step_cap == 0 ? VP_INITIAL_STEPS
                                         : plan->step_cap * 2;
    struct value_plan_step *new_steps =
        realloc(plan->steps, new_cap * sizeof(*new_steps));
    if (new_steps == NULL) {
      plan->feasible = false;
      return false;
    }
    plan->steps = new_steps;
    plan->step_cap = new_cap;
  }
  plan->steps[plan->step_len++] = *step;
  return true;
}

static bool vp_simple_step(struct value_plan *plan,
                           enum value_plan_step_type type, int slot) {
  struct value_plan_step step = {.type = type, .slot = slot};
  return vp_add_step(plan, &step);
}

static int vp_alloc_work_slot(struct value_plan *plan) {
  for (int slot = 0; slot < plan->work_slot_limit; ++slot) {
    if (!plan->used_slot[slot]) {
      plan->used_slot[slot] = true;
      if (slot > plan->max_slot_index) {
        plan->max_slot_index = slot;
      }
      return slot;
    }
  }
  plan->feasible = false;
  return -1;
}

static void vp_free_slot(struct value_plan *plan, int slot) {
  if (slot >= 0 && slot < plan->work_slot_limit) {
    plan->used_slot[slot] = false;
  }
}

/*
 * Emit code that leaves the canonical value id in A.  Every call accounts
 * for one incoming use of id.  If later uses remain, the value is retained in
 * a planned slot; the first evaluation is therefore also the only evaluation
 * of that canonical operation.
 */
static bool vp_emit_consume(struct value_plan *plan, int id);

static bool vp_emit_definition(struct value_plan *plan, int id) {
  struct value_plan_node *node = &plan->nodes[id];

  if (node->kind == VP_NODE_CONST) {
    struct value_plan_step step = {
        .type = VP_STEP_LOAD_IMM,
        .value = node->value,
    };
    return vp_add_step(plan, &step);
  }

  if (node->kind == VP_NODE_VAR) {
    struct value_plan_step step = {
        .type = VP_STEP_LOAD_ARG,
        .arg = node->arg,
        .word = node->word,
    };
    return vp_add_step(plan, &step);
  }

  ASSERT(node->kind == VP_NODE_OP);
  int left = node->left;
  int right = node->right;

  if (plan->nodes[right].kind == VP_NODE_CONST) {
    if (plan->nodes[left].kind == VP_NODE_CONST) {
      struct value_plan_step load = {
          .type = VP_STEP_LOAD_IMM,
          .value = plan->nodes[left].value,
      };
      if (!vp_add_step(plan, &load)) {
        return false;
      }
    } else if (!vp_emit_consume(plan, left)) {
      return false;
    }
    struct value_plan_step step = {
        .type = VP_STEP_ALU_K,
        .op = node->op,
        .value = plan->nodes[right].value,
    };
    return vp_add_step(plan, &step);
  }

  /*
   * Materialize right in A and copy it into the parent's own spill slot.
   * Do not borrow right's retained canonical slot here: evaluating left may
   * consume right's remaining references, release that canonical slot, and
   * reuse it for another result.  The parent spill stays allocated across
   * evaluation of left and is therefore stable until the LDX below.
   */
  if (!vp_emit_consume(plan, right)) {
    return false;
  }

  int x_slot = vp_alloc_work_slot(plan);
  if (x_slot < 0) {
    return false;
  }
  if (!vp_simple_step(plan, VP_STEP_STORE_A_SLOT, x_slot)) {
    vp_free_slot(plan, x_slot);
    return false;
  }

  if (!vp_emit_consume(plan, left)) {
    vp_free_slot(plan, x_slot);
    return false;
  }

  if (!vp_simple_step(plan, VP_STEP_LOAD_X_SLOT, x_slot)) {
    vp_free_slot(plan, x_slot);
    return false;
  }
  /* The LDX has now consumed the spill; the slot may be reused. */
  vp_free_slot(plan, x_slot);

  struct value_plan_step step = {
      .type = VP_STEP_ALU_X,
      .op = node->op,
  };
  return vp_add_step(plan, &step);
}

static bool vp_emit_consume(struct value_plan *plan, int id) {
  if (id < 0 || (size_t)id >= plan->node_len) {
    plan->feasible = false;
    return false;
  }
  struct value_plan_node *node = &plan->nodes[id];

  if (node->slot >= 0) {
    if (!vp_simple_step(plan, VP_STEP_LOAD_A_SLOT, node->slot)) {
      return false;
    }
  } else {
    if (!vp_emit_definition(plan, id)) {
      return false;
    }
  }

  ASSERT(node->uses > 0);
  --node->uses;

  if (node->slot >= 0) {
    if (node->uses == 0) {
      vp_free_slot(plan, node->slot);
      node->slot = -1;
    }
  } else if (node->uses > 0) {
    int slot = vp_alloc_work_slot(plan);
    if (slot < 0) {
      return false;
    }
    node->slot = slot;
    if (!vp_simple_step(plan, VP_STEP_STORE_A_SLOT, slot)) {
      return false;
    }
  }

  return true;
}

static void vp_count_uses(struct value_plan *plan, const int *roots,
                          size_t root_count) {
  for (size_t i = 0; i < plan->node_len; ++i) {
    plan->nodes[i].uses = 0;
  }

  for (size_t i = 0; i < root_count; ++i) {
    plan->nodes[roots[i]].uses++;
  }

  for (size_t i = 0; i < plan->node_len; ++i) {
    struct value_plan_node *node = &plan->nodes[i];
    if (node->kind != VP_NODE_OP) {
      continue;
    }
    /*
     * Immediate constants are rematerialized as an ALU operand and do not
     * consume a planned use.  All other edges are reference counted.
     */
    if (plan->nodes[node->left].kind != VP_NODE_CONST) {
      plan->nodes[node->left].uses++;
    }
    if (plan->nodes[node->right].kind != VP_NODE_CONST) {
      plan->nodes[node->right].uses++;
    }
  }
}

static struct value_plan *vp_plan_new(int word, int work_slot_limit) {
  struct value_plan *plan = calloc(1, sizeof(*plan));
  if (plan == NULL) {
    return NULL;
  }
  plan->feasible = true;
  plan->word = word;
  plan->work_slot_limit = work_slot_limit;
  plan->max_slot_index = -1;

  plan->hash_cap = VP_INITIAL_HASH;
  plan->hash_table = calloc(plan->hash_cap, sizeof(*plan->hash_table));
  if (plan->hash_table == NULL) {
    plan->feasible = false;
  }
  return plan;
}

static bool vp_slots_are_valid(const struct value_plan *plan) {
  for (size_t i = 0; i < plan->step_len; ++i) {
    const struct value_plan_step *step = &plan->steps[i];
    if ((step->type == VP_STEP_LOAD_A_SLOT ||
         step->type == VP_STEP_LOAD_X_SLOT ||
         step->type == VP_STEP_STORE_A_SLOT) &&
        (step->slot < 0 || step->slot >= VALUE_PLAN_MEMWORDS)) {
      return false;
    }
  }
  return true;
}

struct value_plan *value_plan_build(struct expr_tree *left,
                                    struct expr_tree *right, int word) {
  struct value_plan *plan = vp_plan_new(word, VALUE_PLAN_WORK_SLOTS);
  if (plan == NULL) {
    return NULL;
  }
  if (!plan->feasible) {
    return plan;
  }

  /*
   * Two slots are reserved for the final comparison operands.  The work-slot
   * pool must therefore fit in BPF_MEMWORDS - 2 slots.
   */
  plan->max_slot_index = VALUE_PLAN_RIGHT_SLOT;
  plan->used_slot[VALUE_PLAN_LEFT_SLOT] = true;
  plan->used_slot[VALUE_PLAN_RIGHT_SLOT] = true;

  int left_root = vp_build_expr(plan, left, word);
  int right_root = vp_build_expr(plan, right, word);
  if (left_root < 0 || right_root < 0) {
    plan->feasible = false;
    return plan;
  }

  int roots[2] = {left_root, right_root};
  vp_count_uses(plan, roots, 2);

  /*
   * The epilogue writes both comparison-side result slots before exposing
   * them to codegen's A/X compare sequence.  Right is parked first so that
   * evaluating left cannot clobber it.
   */
  if (!vp_emit_consume(plan, right_root) ||
      !vp_simple_step(plan, VP_STEP_STORE_A_SLOT,
                      VALUE_PLAN_RIGHT_SLOT) ||
      !vp_emit_consume(plan, left_root) ||
      !vp_simple_step(plan, VP_STEP_STORE_A_SLOT,
                      VALUE_PLAN_LEFT_SLOT) ||
      !vp_simple_step(plan, VP_STEP_LOAD_X_SLOT,
                      VALUE_PLAN_RIGHT_SLOT) ||
      !vp_simple_step(plan, VP_STEP_LOAD_A_SLOT,
                      VALUE_PLAN_LEFT_SLOT)) {
    plan->feasible = false;
    return plan;
  }

  plan->feasible = vp_slots_are_valid(plan);
  return plan;
}

struct value_plan *value_plan_build_load(struct expr_tree *value, int word) {
  /*
   * A K-form comparison only needs its left value in A.  Slot 14 is planned
   * as its result slot; slot 15 is unnecessary because the right operand is
   * an immediate.  The final ST/LD pair leaves A unchanged and does not
   * clobber X: a root ALU_X leaves its right operand in X for codegen's
   * high-word JSET reuse.
   */
  struct value_plan *plan = vp_plan_new(word, VALUE_PLAN_WORK_SLOTS);
  if (plan == NULL) {
    return NULL;
  }
  if (!plan->feasible) {
    return plan;
  }
  plan->max_slot_index = VALUE_PLAN_LEFT_SLOT;
  plan->used_slot[VALUE_PLAN_LEFT_SLOT] = true;

  int root = vp_build_expr(plan, value, word);
  if (root < 0) {
    plan->feasible = false;
    return plan;
  }

  int roots[1] = {root};
  vp_count_uses(plan, roots, 1);

  if (!vp_emit_consume(plan, root) ||
      !vp_simple_step(plan, VP_STEP_STORE_A_SLOT,
                      VALUE_PLAN_LEFT_SLOT) ||
      !vp_simple_step(plan, VP_STEP_LOAD_A_SLOT,
                      VALUE_PLAN_LEFT_SLOT)) {
    plan->feasible = false;
    return plan;
  }

  plan->feasible = vp_slots_are_valid(plan);
  return plan;
}

void value_plan_destroy(struct value_plan *plan) {
  if (plan == NULL) {
    return;
  }
  free(plan->hash_table);
  free(plan->steps);
  free(plan->nodes);
  free(plan);
}
