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

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <yara/arena.h>
#include <yara/compiler.h>
#include <yara/condition_trace.h>
#include <yara/error.h>
#include <yara/exec.h>
#include <yara/mem.h>
#include <yara/parser.h>
#include <yara/sizedstr.h>
#include <yara/unaligned.h>

#include "ct_trace.h"

#define CT_MEM_SIZE \
  (YR_MAX_LOOP_NESTING * (YR_MAX_LOOP_VARS + YR_INTERNAL_LOOP_VARS))

// ---------------------------------------------------------------------------
// Opcode classification
// ---------------------------------------------------------------------------

static bool ct_opcode_is_unsupported(uint8_t opcode)
{
  switch (opcode)
  {
  case OP_OBJ_LOAD:
  case OP_OBJ_FIELD:
  case OP_OBJ_VALUE:
  case OP_INDEX_ARRAY:
  case OP_LOOKUP_DICT:
  case OP_CALL:
  case OP_IMPORT:
  case OP_MATCHES:
  case OP_CONTAINS:
  case OP_ICONTAINS:
  case OP_STARTSWITH:
  case OP_ISTARTSWITH:
  case OP_ENDSWITH:
  case OP_IENDSWITH:
  case OP_IEQUALS:
  case OP_ENTRYPOINT:
  case OP_INT8:
  case OP_INT16:
  case OP_INT32:
  case OP_UINT8:
  case OP_UINT16:
  case OP_UINT32:
  case OP_INT8BE:
  case OP_INT16BE:
  case OP_INT32BE:
  case OP_UINT8BE:
  case OP_UINT16BE:
  case OP_UINT32BE:
    return true;
  default:
    return false;
  }
}

// Opcodes that are pure VM machinery and never represent expression nodes.
static bool ct_opcode_is_ignored(uint8_t opcode)
{
  switch (opcode)
  {
  case OP_NOP:
  case OP_HALT:
  case OP_ERROR:
  case OP_POP:
  case OP_CLEAR_M:
  case OP_INCR_M:
  case OP_JUNDEF:
  case OP_JNUNDEF:
  case OP_JNUNDEF_P:
  case OP_JUNDEF_P:
  case OP_JFALSE_P:
  case OP_JZ:
  case OP_JZ_P:
  case OP_JL_P:
  case OP_JLE_P:
  case OP_ITER_CONDITION:
  case OP_ITER_START_ARRAY:
  case OP_ITER_START_DICT:
  case OP_ITER_START_INT_RANGE:
  case OP_ITER_START_INT_ENUM:
  case OP_ITER_START_STRING_SET:
  case OP_ITER_START_TEXT_STRING_SET:
    return true;
  default:
    return false;
  }
}

static YR_CT_NODE_KIND ct_kind_for_opcode(uint8_t opcode)
{
  switch (opcode)
  {
  case OP_AND:
    return YR_CT_NODE_AND;
  case OP_OR:
    return YR_CT_NODE_OR;
  case OP_NOT:
    return YR_CT_NODE_NOT;
  case OP_DEFINED:
    return YR_CT_NODE_DEFINED;
  case OP_FILESIZE:
    return YR_CT_NODE_FILESIZE;
  case OP_ENTRYPOINT:
    return YR_CT_NODE_ENTRYPOINT;
  case OP_FOUND:
  case OP_FOUND_AT:
  case OP_FOUND_IN:
    return YR_CT_NODE_STRING_FOUND;
  case OP_COUNT:
  case OP_COUNT_IN:
    return YR_CT_NODE_STRING_COUNT;
  case OP_OFFSET:
    return YR_CT_NODE_STRING_OFFSET;
  case OP_LENGTH:
    return YR_CT_NODE_STRING_LENGTH;
  case OP_PUSH_RULE:
    return YR_CT_NODE_RULE_REF;
  case OP_OF:
  case OP_OF_PERCENT:
    return YR_CT_NODE_OF;
  case OP_OF_FOUND_IN:
    return YR_CT_NODE_OF_FOUND_IN;
  case OP_OF_FOUND_AT:
    return YR_CT_NODE_OF_FOUND_AT;
  case OP_ITER_END:
    return YR_CT_NODE_FOR;
  case OP_ITER_NEXT:
    return YR_CT_NODE_ITERATION;
  case OP_PUSH_M:
    return YR_CT_NODE_LOOP_VAR;
  case OP_OBJ_LOAD:
    return YR_CT_NODE_OBJ_LOAD;
  case OP_OBJ_FIELD:
    return YR_CT_NODE_OBJ_FIELD;
  case OP_OBJ_VALUE:
    return YR_CT_NODE_OBJ_VALUE;
  case OP_INDEX_ARRAY:
  case OP_LOOKUP_DICT:
    return YR_CT_NODE_OBJ_INDEX;
  case OP_CALL:
    return YR_CT_NODE_CALL;
  case OP_MATCHES:
    return YR_CT_NODE_MATCHES;
  case OP_CONTAINS:
  case OP_ICONTAINS:
  case OP_STARTSWITH:
  case OP_ISTARTSWITH:
  case OP_ENDSWITH:
  case OP_IENDSWITH:
  case OP_IEQUALS:
    return YR_CT_NODE_STRING_OP;
  case OP_INT8:
  case OP_INT16:
  case OP_INT32:
  case OP_UINT8:
  case OP_UINT16:
  case OP_UINT32:
  case OP_INT8BE:
  case OP_INT16BE:
  case OP_INT32BE:
  case OP_UINT8BE:
  case OP_UINT16BE:
  case OP_UINT32BE:
    return YR_CT_NODE_INT_READ;
  case OP_PUSH:
  case OP_PUSH_8:
  case OP_PUSH_16:
  case OP_PUSH_32:
    return YR_CT_NODE_CONST_INT;
  case OP_PUSH_U:
    return YR_CT_NODE_CONST_INT;
  default:
    break;
  }

  if (opcode >= OP_INT_BEGIN && opcode <= OP_INT_END)
  {
    switch (opcode)
    {
    case OP_INT_EQ:
    case OP_INT_NEQ:
    case OP_INT_LT:
    case OP_INT_GT:
    case OP_INT_LE:
    case OP_INT_GE:
      return YR_CT_NODE_INT_COMPARE;
    default:
      return YR_CT_NODE_INT_ARITH;
    }
  }

  if (opcode >= OP_DBL_BEGIN && opcode <= OP_DBL_END)
  {
    switch (opcode)
    {
    case OP_DBL_EQ:
    case OP_DBL_NEQ:
    case OP_DBL_LT:
    case OP_DBL_GT:
    case OP_DBL_LE:
    case OP_DBL_GE:
      return YR_CT_NODE_DBL_COMPARE;
    default:
      return YR_CT_NODE_DBL_ARITH;
    }
  }

  if (opcode >= OP_STR_BEGIN && opcode <= OP_STR_END)
    return YR_CT_NODE_STR_COMPARE;

  switch (opcode)
  {
  case OP_MOD:
  case OP_SHL:
  case OP_SHR:
  case OP_BITWISE_NOT:
  case OP_BITWISE_AND:
  case OP_BITWISE_OR:
  case OP_BITWISE_XOR:
    return YR_CT_NODE_INT_ARITH;
  default:
    return YR_CT_NODE_OTHER;
  }
}

// ---------------------------------------------------------------------------
// Compiler-side collection
// ---------------------------------------------------------------------------

typedef struct _YR_CT_POOL_ENTRY
{
  char* text;
  uint32_t offset;
} YR_CT_POOL_ENTRY;

typedef struct _YR_CT_LOOP_INFO
{
  uint32_t next_offset;
  uint32_t exit_jump_offset;
  uint32_t back_jump_offset;
  uint32_t cond_offset;
  uint32_t end_offset;
  int active;
} YR_CT_LOOP_INFO;

typedef struct _YR_CT_COMPILER_CTX
{
  YR_CT_STATIC_NODE* nodes;
  uint32_t node_count;
  uint32_t node_capacity;

  YR_CT_POOL_ENTRY* pool;
  uint32_t pool_count;
  uint32_t pool_capacity;
  uint32_t pool_size;

  YR_CT_LOOP_INFO loops[YR_MAX_LOOP_NESTING];
  int loop_depth;

  uint32_t last_jump_offset;
  int have_last_jump;
} YR_CT_COMPILER_CTX;

static uint32_t ct_intern(YR_CT_COMPILER_CTX* ctx, const char* text)
{
  if (text == NULL)
    return YR_CT_NO_OFFSET;

  for (uint32_t i = 0; i < ctx->pool_count; i++)
  {
    if (strcmp(ctx->pool[i].text, text) == 0)
      return ctx->pool[i].offset;
  }

  if (ctx->pool_count == ctx->pool_capacity)
  {
    uint32_t new_cap = ctx->pool_capacity == 0 ? 32 : ctx->pool_capacity * 2;
    YR_CT_POOL_ENTRY* grown = (YR_CT_POOL_ENTRY*) yr_realloc(
        ctx->pool, new_cap * sizeof(YR_CT_POOL_ENTRY));

    if (grown == NULL)
      return YR_CT_NO_OFFSET;

    ctx->pool = grown;
    ctx->pool_capacity = new_cap;
  }

  char* copy = yr_strdup(text);

  if (copy == NULL)
    return YR_CT_NO_OFFSET;

  uint32_t offset = ctx->pool_size;
  ctx->pool[ctx->pool_count].text = copy;
  ctx->pool[ctx->pool_count].offset = offset;
  ctx->pool_count++;
  ctx->pool_size += (uint32_t) strlen(text) + 1;

  return offset;
}

static void ct_compiler_ctx_free(void* opaque)
{
  YR_CT_COMPILER_CTX* ctx = (YR_CT_COMPILER_CTX*) opaque;

  if (ctx == NULL)
    return;

  for (uint32_t i = 0; i < ctx->pool_count; i++)
    yr_free(ctx->pool[i].text);

  yr_free(ctx->pool);
  yr_free(ctx->nodes);
  yr_free(ctx);
}

int yr_ct_meta_begin(YR_COMPILER* compiler)
{
  if (compiler->condition_trace_ctx != NULL)
    return ERROR_SUCCESS;

  YR_CT_COMPILER_CTX* ctx = (YR_CT_COMPILER_CTX*) yr_calloc(
      1, sizeof(YR_CT_COMPILER_CTX));

  if (ctx == NULL)
    return ERROR_INSUFFICIENT_MEMORY;

  ctx->node_capacity = 128;
  ctx->nodes = (YR_CT_STATIC_NODE*) yr_malloc(
      ctx->node_capacity * sizeof(YR_CT_STATIC_NODE));

  if (ctx->nodes == NULL)
  {
    yr_free(ctx);
    return ERROR_INSUFFICIENT_MEMORY;
  }

  for (int i = 0; i < YR_MAX_LOOP_NESTING; i++)
    ctx->loops[i].active = 0;

  compiler->condition_trace_ctx = ctx;
  compiler->condition_trace_node_count = 0;

  return ERROR_SUCCESS;
}

int yr_ct_meta_emit(
    YR_COMPILER* compiler,
    uint8_t opcode,
    uint32_t code_offset,
    uint32_t* node_id)
{
  if (!compiler->condition_trace_enabled ||
      compiler->condition_trace_ctx == NULL)
  {
    return ERROR_SUCCESS;
  }

  YR_CT_COMPILER_CTX* ctx =
      (YR_CT_COMPILER_CTX*) compiler->condition_trace_ctx;

  // Loop structure tracking.
  if (opcode == OP_ITER_NEXT)
  {
    if (ctx->loop_depth >= YR_MAX_LOOP_NESTING)
      return ERROR_INTERNAL_FATAL_ERROR;

    YR_CT_LOOP_INFO* info = &ctx->loops[ctx->loop_depth++];
    info->active = 1;
    info->next_offset = code_offset;
    info->exit_jump_offset = YR_CT_NO_OFFSET;
    info->back_jump_offset = YR_CT_NO_OFFSET;
    info->cond_offset = YR_CT_NO_OFFSET;
    info->end_offset = YR_CT_NO_OFFSET;
  }

  if (opcode == OP_ITER_CONDITION && ctx->loop_depth > 0)
    ctx->loops[ctx->loop_depth - 1].cond_offset = code_offset;

  if (opcode == OP_JTRUE_P && ctx->loop_depth > 0)
  {
    YR_CT_LOOP_INFO* info = &ctx->loops[ctx->loop_depth - 1];

    if (info->exit_jump_offset == YR_CT_NO_OFFSET)
      info->exit_jump_offset = code_offset;
    else
      info->back_jump_offset = code_offset;

    // Loop jumps are control flow: their offsets live on the FOR node.
    return ERROR_SUCCESS;
  }

  if (opcode == OP_JFALSE || opcode == OP_JTRUE)
  {
    ctx->last_jump_offset = code_offset;
    ctx->have_last_jump = 1;

    // The jump itself is control flow, not an expression node. Its offset is
    // attached to the owning and/or node through control_offset, which is what
    // the runtime lookup uses to detect the short-circuit decision.
    return ERROR_SUCCESS;
  }

  if (ct_opcode_is_ignored(opcode))
    return ERROR_SUCCESS;

  if (ctx->node_count == ctx->node_capacity)
  {
    uint32_t new_capacity = ctx->node_capacity * 2;
    YR_CT_STATIC_NODE* grown = (YR_CT_STATIC_NODE*) yr_realloc(
        ctx->nodes, new_capacity * sizeof(YR_CT_STATIC_NODE));

    if (grown == NULL)
      return ERROR_INSUFFICIENT_MEMORY;

    ctx->nodes = grown;
    ctx->node_capacity = new_capacity;
  }

  YR_CT_STATIC_NODE* node = &ctx->nodes[ctx->node_count];
  memset(node, 0, sizeof(*node));

  node->id = ctx->node_count;
  node->kind = (uint32_t) ct_kind_for_opcode(opcode);
  node->rule_idx = compiler->current_rule_idx == UINT32_MAX
                       ? UINT32_MAX
                       : compiler->current_rule_idx;
  node->end_line = 0;
  node->code_offset = code_offset;
  node->control_offset = YR_CT_NO_OFFSET;
  node->loop_next_offset = YR_CT_NO_OFFSET;
  node->loop_jump_offset = YR_CT_NO_OFFSET;
  node->loop_cond_offset = YR_CT_NO_OFFSET;
  node->loop_end_offset = YR_CT_NO_OFFSET;
  node->fold_origin_id = YR_CT_INVALID_NODE;
  node->file_off = YR_CT_NO_OFFSET;
  node->namespace_off = YR_CT_NO_OFFSET;
  node->rule_name_off = YR_CT_NO_OFFSET;
  node->detail_off = YR_CT_NO_OFFSET;

  if (ct_opcode_is_unsupported(opcode))
    node->flags |= YR_CT_STATIC_FLAG_UNSUPPORTED;

  if (opcode == OP_INIT_RULE)
    node->flags |= YR_CT_STATIC_FLAG_RULE_ROOT;

  if (opcode == OP_PUSH_8 || opcode == OP_PUSH_16 || opcode == OP_PUSH_32 ||
      opcode == OP_PUSH || opcode == OP_PUSH_U)
  {
    node->flags |= YR_CT_STATIC_FLAG_COMPILE_TIME;
  }

  if ((opcode == OP_AND || opcode == OP_OR) && ctx->have_last_jump)
    node->control_offset = ctx->last_jump_offset;

  if (opcode == OP_ITER_END && ctx->loop_depth > 0)
  {
    YR_CT_LOOP_INFO* info = &ctx->loops[ctx->loop_depth - 1];
    node->loop_next_offset = info->next_offset;
    node->loop_jump_offset = info->exit_jump_offset;
    node->loop_cond_offset = info->cond_offset;
    node->loop_end_offset = code_offset;
    node->flags |= YR_CT_STATIC_FLAG_LOOP;
    info->active = 0;
    ctx->loop_depth--;
  }

  if (node->rule_idx != UINT32_MAX)
  {
    YR_RULE* rule = _yr_compiler_get_rule_by_idx(compiler, node->rule_idx);

    if (rule != NULL)
    {
      node->rule_name_off = ct_intern(ctx, rule->identifier);
      node->namespace_off = ct_intern(ctx, rule->ns->name);
    }
  }

  if (node_id != NULL)
    *node_id = node->id;

  compiler->condition_trace_node_count = ++ctx->node_count;

  return ERROR_SUCCESS;
}

int yr_ct_meta_annotate(
    YR_COMPILER* compiler,
    int end_line,
    const char* file_name,
    const char* detail)
{
  if (!compiler->condition_trace_enabled ||
      compiler->condition_trace_ctx == NULL)
  {
    return ERROR_SUCCESS;
  }

  YR_CT_COMPILER_CTX* ctx =
      (YR_CT_COMPILER_CTX*) compiler->condition_trace_ctx;

  if (ctx->node_count == 0)
    return ERROR_SUCCESS;

  YR_CT_STATIC_NODE* node = &ctx->nodes[ctx->node_count - 1];
  node->end_line = end_line;

  if (file_name != NULL)
    node->file_off = ct_intern(ctx, file_name);

  if (detail != NULL)
    node->detail_off = ct_intern(ctx, detail);

  return ERROR_SUCCESS;
}

// Best-effort static arity inference: replay the node stream of each rule on
// an abstract operand stack to record how many child expressions each node
// has. The authoritative child identities exposed by a snapshot are always
// the dynamic causal edges; this only supplies a static operand-count hint
// for nodes whose runtime path was skipped and produced no edge.
static void ct_infer_structural_children(YR_CT_COMPILER_CTX* ctx)
{
  uint32_t* abstack = (uint32_t*) yr_malloc(
      (ctx->node_count + 1) * sizeof(uint32_t));
  uint32_t* children = (uint32_t*) yr_malloc(
      (ctx->node_count + 1) * sizeof(uint32_t));

  if (abstack == NULL || children == NULL)
  {
    yr_free(abstack);
    yr_free(children);
    return;
  }

  uint32_t sp = 0;

  for (uint32_t i = 0; i < ctx->node_count; i++)
  {
    YR_CT_STATIC_NODE* node = &ctx->nodes[i];
    uint8_t kind = (uint8_t) node->kind;

    if (kind == YR_CT_NODE_RULE)
    {
      sp = 0;
      abstack[sp++] = node->id;
      continue;
    }

    if (kind == YR_CT_NODE_ITERATION)
      continue;

    if (kind == YR_CT_NODE_FOR)
    {
      uint32_t n = 0;

      // ITER_END consumes the three PUSH_M values before it.
      for (int k = 0; k < 3 && sp > 0; k++)
        children[n++] = abstack[--sp];

      node->child_count = n;
      node->child_start = 0;  // children stored inline during finalize below
      abstack[sp++] = node->id;
      continue;
    }

    int pops = 0;

    switch (kind)
    {
    case YR_CT_NODE_NOT:
    case YR_CT_NODE_DEFINED:
    case YR_CT_NODE_INT_ARITH:
    case YR_CT_NODE_DBL_ARITH:
    case YR_CT_NODE_STRING_FOUND:
    case YR_CT_NODE_STRING_COUNT:
    case YR_CT_NODE_STRING_OFFSET:
    case YR_CT_NODE_STRING_LENGTH:
    case YR_CT_NODE_OBJ_FIELD:
    case YR_CT_NODE_OBJ_VALUE:
    case YR_CT_NODE_OBJ_INDEX:
    case YR_CT_NODE_LOOP_VAR:
      pops = 1;
      break;
    case YR_CT_NODE_AND:
    case YR_CT_NODE_OR:
    case YR_CT_NODE_INT_COMPARE:
    case YR_CT_NODE_DBL_COMPARE:
    case YR_CT_NODE_STR_COMPARE:
    case YR_CT_NODE_STRING_OP:
    case YR_CT_NODE_MATCHES:
      pops = 2;
      break;
    case YR_CT_NODE_OF:
    case YR_CT_NODE_OF_FOUND_IN:
    case YR_CT_NODE_OF_FOUND_AT:
      pops = 2;  // quantifier plus at least one set element
      break;
    default:
      pops = 0;
      break;
    }

    // Unsupported call regions can desync the abstract stack; recover by
    // treating whatever is available as the operands.
    if (pops > (int) sp)
      pops = sp;

    node->child_count = (uint32_t) pops;

    for (int k = 0; k < pops; k++)
      children[k] = abstack[--sp];

    abstack[sp++] = node->id;
  }

  yr_free(abstack);
  yr_free(children);
}

int yr_ct_meta_finalize(YR_COMPILER* compiler)
{
  if (!compiler->condition_trace_enabled ||
      compiler->condition_trace_ctx == NULL)
  {
    return ERROR_SUCCESS;
  }

  YR_CT_COMPILER_CTX* ctx =
      (YR_CT_COMPILER_CTX*) compiler->condition_trace_ctx;

  if (ctx->node_count == 0)
  {
    ct_compiler_ctx_free(ctx);
    compiler->condition_trace_ctx = NULL;
    return ERROR_SUCCESS;
  }

  ct_infer_structural_children(ctx);

  // Compile-time literal nodes are their own fold origin: the FOLDED dynamic
  // instance can always be traced back to this source expression, even though
  // no runtime evaluation happened.
  for (uint32_t i = 0; i < ctx->node_count; i++)
  {
    if ((ctx->nodes[i].flags & YR_CT_STATIC_FLAG_COMPILE_TIME) &&
        ctx->nodes[i].fold_origin_id == YR_CT_INVALID_NODE)
    {
      ctx->nodes[i].fold_origin_id = ctx->nodes[i].id;
    }
  }

  YR_ARENA_REF header_ref;

  FAIL_ON_ERROR(yr_arena_allocate_zeroed_memory(
      compiler->arena,
      YR_CONDITION_TRACE_SECTION,
      sizeof(YR_CT_STATIC_HEADER),
      &header_ref));

  FAIL_ON_ERROR(yr_arena_write_data(
      compiler->arena,
      YR_CONDITION_TRACE_SECTION,
      ctx->nodes,
      ctx->node_count * sizeof(YR_CT_STATIC_NODE),
      NULL));

  for (uint32_t i = 0; i < ctx->pool_count; i++)
  {
    FAIL_ON_ERROR(yr_arena_write_string(
        compiler->arena,
        YR_CONDITION_TRACE_SECTION,
        ctx->pool[i].text,
        NULL));
  }

  YR_CT_STATIC_HEADER* header = (YR_CT_STATIC_HEADER*) yr_arena_ref_to_ptr(
      compiler->arena, &header_ref);

  header->magic = YR_CT_META_MAGIC;
  header->version = YR_CT_META_VERSION;
  header->num_nodes = ctx->node_count;
  header->string_pool_size = ctx->pool_size;
  header->num_child_entries = 0;

  ct_compiler_ctx_free(ctx);
  compiler->condition_trace_ctx = NULL;

  return ERROR_SUCCESS;
}

void yr_ct_meta_abort(YR_COMPILER* compiler)
{
  if (compiler->condition_trace_ctx != NULL)
  {
    ct_compiler_ctx_free(compiler->condition_trace_ctx);
    compiler->condition_trace_ctx = NULL;
  }
}

// ---------------------------------------------------------------------------
// Runtime index
// ---------------------------------------------------------------------------

typedef struct _YR_CT_RT_INDEX
{
  YR_CT_RT_NODE* nodes;
  uint32_t* lookup_offsets;
  uint32_t* lookup_nodes;
  uint32_t lookup_count;
} YR_CT_RT_INDEX;

int yr_ct_build_runtime_index(YR_RULES* rules)
{
  rules->condition_trace_meta = NULL;
  rules->condition_trace_num_nodes = 0;
  rules->condition_trace_rt = NULL;

  if (rules->arena->num_buffers <= YR_CONDITION_TRACE_SECTION)
    return ERROR_SUCCESS;

  uint32_t section_size = yr_arena_get_current_offset(
      rules->arena, YR_CONDITION_TRACE_SECTION);

  if (section_size < sizeof(YR_CT_STATIC_HEADER))
    return ERROR_SUCCESS;

  YR_CT_STATIC_HEADER* header = (YR_CT_STATIC_HEADER*) yr_arena_get_ptr(
      rules->arena, YR_CONDITION_TRACE_SECTION, 0);

  if (header == NULL || header->magic != YR_CT_META_MAGIC ||
      header->version != YR_CT_META_VERSION || header->num_nodes == 0)
  {
    return ERROR_SUCCESS;
  }

  uint64_t need = sizeof(YR_CT_STATIC_HEADER) +
                  (uint64_t) header->num_nodes * sizeof(YR_CT_STATIC_NODE) +
                  header->string_pool_size;

  if (need > section_size)
    return ERROR_CORRUPT_FILE;

  YR_CT_STATIC_NODE* metas = (YR_CT_STATIC_NODE*) yr_arena_get_ptr(
      rules->arena,
      YR_CONDITION_TRACE_SECTION,
      sizeof(YR_CT_STATIC_HEADER));

  const char* pool = (const char*) yr_arena_get_ptr(
      rules->arena,
      YR_CONDITION_TRACE_SECTION,
      sizeof(YR_CT_STATIC_HEADER) +
          header->num_nodes * sizeof(YR_CT_STATIC_NODE));

  YR_CT_RT_INDEX* index = (YR_CT_RT_INDEX*) yr_calloc(
      1, sizeof(YR_CT_RT_INDEX));

  if (index == NULL)
    return ERROR_INSUFFICIENT_MEMORY;

  index->nodes = (YR_CT_RT_NODE*) yr_calloc(
      header->num_nodes, sizeof(YR_CT_RT_NODE));

  // Main offset plus up to four control offsets per node.
  index->lookup_offsets = (uint32_t*) yr_malloc(
      header->num_nodes * 5 * sizeof(uint32_t));
  index->lookup_nodes = (uint32_t*) yr_malloc(
      header->num_nodes * 5 * sizeof(uint32_t));

  if (index->nodes == NULL || index->lookup_offsets == NULL ||
      index->lookup_nodes == NULL)
  {
    yr_free(index->nodes);
    yr_free(index->lookup_offsets);
    yr_free(index->lookup_nodes);
    yr_free(index);
    return ERROR_INSUFFICIENT_MEMORY;
  }

  uint32_t lookup_count = 0;

  for (uint32_t i = 0; i < header->num_nodes; i++)
  {
    YR_CT_RT_NODE* rt = &index->nodes[i];
    rt->meta = &metas[i];

    rt->file_name = (metas[i].file_off == YR_CT_NO_OFFSET)
                        ? NULL
                        : pool + metas[i].file_off;
    rt->namespace_name = (metas[i].namespace_off == YR_CT_NO_OFFSET)
                             ? NULL
                             : pool + metas[i].namespace_off;
    rt->rule_name = (metas[i].rule_name_off == YR_CT_NO_OFFSET)
                        ? NULL
                        : pool + metas[i].rule_name_off;
    rt->detail = (metas[i].detail_off == YR_CT_NO_OFFSET)
                     ? NULL
                     : pool + metas[i].detail_off;

    index->lookup_offsets[lookup_count] = metas[i].code_offset;
    index->lookup_nodes[lookup_count] = i;
    lookup_count++;

    uint32_t extras[4] = {
        metas[i].control_offset,
        metas[i].loop_jump_offset,
        metas[i].loop_cond_offset,
        metas[i].loop_next_offset,
    };

    for (int j = 0; j < 4; j++)
    {
      if (extras[j] == YR_CT_NO_OFFSET)
        continue;

      index->lookup_offsets[lookup_count] = extras[j];
      index->lookup_nodes[lookup_count] = i;
      lookup_count++;
    }
  }

  // Insertion sort on parallel arrays.
  for (uint32_t i = 1; i < lookup_count; i++)
  {
    uint32_t key_off = index->lookup_offsets[i];
    uint32_t key_node = index->lookup_nodes[i];
    int64_t j = (int64_t) i - 1;

    while (j >= 0 && index->lookup_offsets[j] > key_off)
    {
      index->lookup_offsets[j + 1] = index->lookup_offsets[j];
      index->lookup_nodes[j + 1] = index->lookup_nodes[j];
      j--;
    }

    index->lookup_offsets[j + 1] = key_off;
    index->lookup_nodes[j + 1] = key_node;
  }

  index->lookup_count = lookup_count;

  rules->condition_trace_meta = header;
  rules->condition_trace_num_nodes = header->num_nodes;
  rules->condition_trace_rt = index;

  return ERROR_SUCCESS;
}

bool yr_ct_rules_have_meta(const YR_RULES* rules)
{
  return rules->condition_trace_rt != NULL;
}

void yr_ct_free_runtime_index(YR_RULES* rules)
{
  if (rules == NULL)
    return;

  YR_CT_RT_INDEX* index = (YR_CT_RT_INDEX*) rules->condition_trace_rt;

  if (index != NULL)
  {
    yr_free(index->nodes);
    yr_free(index->lookup_offsets);
    yr_free(index->lookup_nodes);
    yr_free(index);
    rules->condition_trace_rt = NULL;
  }
}

static const YR_CT_RT_NODE* ct_rt_lookup(YR_CT_STATE* state, uint32_t offset)
{
  YR_CT_RT_INDEX* index = (YR_CT_RT_INDEX*) state->rules->condition_trace_rt;

  int64_t lo = 0;
  int64_t hi = (int64_t) index->lookup_count - 1;

  while (lo <= hi)
  {
    int64_t mid = lo + (hi - lo) / 2;

    if (index->lookup_offsets[mid] == offset)
      return &index->nodes[index->lookup_nodes[mid]];

    if (index->lookup_offsets[mid] < offset)
      lo = mid + 1;
    else
      hi = mid - 1;
  }

  return NULL;
}

// ---------------------------------------------------------------------------
// Per-scan dynamic state
// ---------------------------------------------------------------------------

static int ct_grow_records(YR_CT_STATE* state)
{
  if (state->rec_count < state->rec_capacity)
    return ERROR_SUCCESS;

  uint32_t new_cap = state->rec_capacity == 0 ? 256 : state->rec_capacity * 2;
  YR_CT_DYN_REC* grown = (YR_CT_DYN_REC*) yr_realloc(
      state->recs, new_cap * sizeof(YR_CT_DYN_REC));

  if (grown == NULL)
    return ERROR_INSUFFICIENT_MEMORY;

  state->recs = grown;
  state->rec_capacity = new_cap;
  return ERROR_SUCCESS;
}

static int ct_grow_edges(YR_CT_STATE* state)
{
  if (state->edge_count < state->edge_capacity)
    return ERROR_SUCCESS;

  uint32_t new_cap = state->edge_capacity == 0 ? 256 : state->edge_capacity * 2;
  YR_CT_DYN_EDGE* grown = (YR_CT_DYN_EDGE*) yr_realloc(
      state->edges, new_cap * sizeof(YR_CT_DYN_EDGE));

  if (grown == NULL)
    return ERROR_INSUFFICIENT_MEMORY;

  state->edges = grown;
  state->edge_capacity = new_cap;
  return ERROR_SUCCESS;
}

static int ct_add_edge(YR_CT_STATE* state, uint32_t from, uint32_t to)
{
  if (from == YR_CT_INVALID_INSTANCE || to == YR_CT_INVALID_INSTANCE ||
      from == to)
    return ERROR_SUCCESS;

  for (uint32_t i = 0; i < state->edge_count; i++)
  {
    if (state->edges[i].from_instance == from &&
        state->edges[i].to_instance == to)
      return ERROR_SUCCESS;
  }

  FAIL_ON_ERROR(ct_grow_edges(state));
  state->edges[state->edge_count].from_instance = from;
  state->edges[state->edge_count].to_instance = to;
  state->edge_count++;
  return ERROR_SUCCESS;
}

static int ct_slot_ensure(YR_CT_STATE* state, uint32_t sp)
{
  if (sp <= state->slot_capacity)
    return ERROR_SUCCESS;

  uint32_t new_cap = state->slot_capacity == 0 ? 256 : state->slot_capacity * 2;

  while (new_cap < sp)
    new_cap *= 2;

  YR_CT_STACK_SLOT* grown = (YR_CT_STACK_SLOT*) yr_realloc(
      state->slots, new_cap * sizeof(YR_CT_STACK_SLOT));

  if (grown == NULL)
    return ERROR_INSUFFICIENT_MEMORY;

  state->slots = grown;
  state->slot_capacity = new_cap;
  return ERROR_SUCCESS;
}

static int ct_slot_push(YR_CT_STATE* state, uint32_t instance_id)
{
  FAIL_ON_ERROR(ct_slot_ensure(state, state->slot_sp + 1));
  state->slots[state->slot_sp++].instance_id = instance_id;
  return ERROR_SUCCESS;
}

static uint32_t ct_slot_pop(YR_CT_STATE* state)
{
  if (state->slot_sp == 0)
    return YR_CT_INVALID_INSTANCE;

  return state->slots[--state->slot_sp].instance_id;
}

static void ct_snapshot_path(YR_CT_STATE* state, YR_CT_DYN_REC* rec)
{
  rec->depth = state->loop_depth;

  for (uint8_t i = 0; i < state->loop_depth; i++)
  {
    rec->ordinal[i] = state->loops[i].ordinal;
    rec->path_integer[i] = state->loops[i].path_integer;
    rec->path_is_integer[i] = state->loops[i].path_is_integer;
    rec->path_string[i] = state->loops[i].path_string;
  }
}

static int ct_new_record(
    YR_CT_STATE* state,
    const YR_CT_RT_NODE* rt,
    YR_CT_INSTANCE_STATE istate,
    YR_CT_VALUE_TYPE vtype,
    YR_CT_DYN_REC** out)
{
  FAIL_ON_ERROR(ct_grow_records(state));

  YR_CT_DYN_REC* rec = &state->recs[state->rec_count];
  memset(rec, 0, sizeof(*rec));

  rec->id = state->rec_count;
  rec->node_id = rt->meta->id;
  rec->state = istate;
  rec->skip_reason = YR_CT_SKIP_NONE;
  rec->value_type = vtype;
  rec->winning_instance = YR_CT_INVALID_INSTANCE;
  rec->failing_instance = YR_CT_INVALID_INSTANCE;
  rec->compile_time =
      (rt->meta->flags & YR_CT_STATIC_FLAG_COMPILE_TIME) != 0;

  ct_snapshot_path(state, rec);

  state->rec_count++;
  *out = rec;
  return ERROR_SUCCESS;
}

static YR_CT_VALUE_TYPE ct_value_type_for(const YR_CT_STATIC_NODE* meta)
{
  switch ((YR_CT_NODE_KIND) meta->kind)
  {
  case YR_CT_NODE_AND:
  case YR_CT_NODE_OR:
  case YR_CT_NODE_NOT:
  case YR_CT_NODE_DEFINED:
  case YR_CT_NODE_INT_COMPARE:
  case YR_CT_NODE_DBL_COMPARE:
  case YR_CT_NODE_STR_COMPARE:
  case YR_CT_NODE_STRING_FOUND:
  case YR_CT_NODE_OF:
  case YR_CT_NODE_OF_FOUND_IN:
  case YR_CT_NODE_OF_FOUND_AT:
  case YR_CT_NODE_FOR:
  case YR_CT_NODE_RULE_REF:
  case YR_CT_NODE_MATCHES:
  case YR_CT_NODE_STRING_OP:
    return YR_CT_VALUE_BOOLEAN;

  case YR_CT_NODE_CONST_FLOAT:
  case YR_CT_NODE_DBL_ARITH:
    return YR_CT_VALUE_FLOAT;

  case YR_CT_NODE_CONST_STRING:
    return YR_CT_VALUE_STRING;

  default:
    return YR_CT_VALUE_INTEGER;
  }
}

int yr_ct_state_reset(YR_SCANNER* scanner)
{
  YR_CT_STATE* state = (YR_CT_STATE*) scanner->condition_trace_state;

  if (state != NULL)
    yr_ct_state_destroy(state);

  state = (YR_CT_STATE*) yr_calloc(1, sizeof(YR_CT_STATE));

  if (state == NULL)
    return ERROR_INSUFFICIENT_MEMORY;

  state->rules = scanner->rules;
  state->loop_depth = 0;
  state->incomplete = false;
  state->slot_sp = 0;

  uint32_t num_rules = scanner->rules->num_rules;

  state->rule_root_instance = (uint32_t*) yr_malloc(
      num_rules * sizeof(uint32_t));
  state->decisions = (YR_CT_DECISION*) yr_calloc(
      num_rules, sizeof(YR_CT_DECISION));
  state->mem_instances = (uint32_t*) yr_malloc(
      CT_MEM_SIZE * sizeof(uint32_t));

  if (state->rule_root_instance == NULL || state->decisions == NULL ||
      state->mem_instances == NULL)
  {
    yr_free(state->rule_root_instance);
    yr_free(state->decisions);
    yr_free(state->mem_instances);
    yr_free(state);
    return ERROR_INSUFFICIENT_MEMORY;
  }

  for (uint32_t i = 0; i < num_rules; i++)
  {
    state->rule_root_instance[i] = YR_CT_INVALID_INSTANCE;
    state->decisions[i].rule_idx = i;
    state->decisions[i].blocked_by_global_rule_idx = YR_CT_INVALID_RULE;
    state->decisions[i].state = YR_CT_INST_SKIPPED;
    state->decisions[i].skip_reason = YR_CT_SKIP_RULE_NOT_REQUIRED;
  }

  for (int i = 0; i < CT_MEM_SIZE; i++)
    state->mem_instances[i] = YR_CT_INVALID_INSTANCE;

  scanner->condition_trace_state = state;
  return ERROR_SUCCESS;
}

void yr_ct_state_destroy(YR_CT_STATE* state)
{
  if (state == NULL)
    return;

  for (uint32_t i = 0; i < state->rec_count; i++)
    yr_free(state->recs[i].sval);

  yr_free(state->recs);
  yr_free(state->edges);
  yr_free(state->slots);
  yr_free(state->mem_instances);
  yr_free(state->rule_root_instance);
  yr_free(state->decisions);
  yr_free(state);
}

void yr_ct_mark_incomplete(YR_SCANNER* scanner)
{
  YR_CT_STATE* state = (YR_CT_STATE*) scanner->condition_trace_state;

  if (state != NULL)
    state->incomplete = true;
}

// ---------------------------------------------------------------------------
// Skipped-subtree marking
// ---------------------------------------------------------------------------

static int ct_mark_range_skipped(
    YR_CT_STATE* state,
    uint32_t from_offset,
    uint32_t to_offset,
    YR_CT_SKIP_REASON reason)
{
  YR_CT_RT_INDEX* index = (YR_CT_RT_INDEX*) state->rules->condition_trace_rt;

  for (uint32_t i = 0; i < state->rules->condition_trace_num_nodes; i++)
  {
    const YR_CT_STATIC_NODE* meta = index->nodes[i].meta;
    uint32_t off = meta->code_offset;

    if (off < from_offset || off >= to_offset)
      continue;

    bool already = false;

    for (uint32_t j = 0; j < state->rec_count; j++)
    {
      YR_CT_DYN_REC* existing = &state->recs[j];

      if (existing->node_id != meta->id ||
          existing->depth != state->loop_depth)
        continue;

      bool same_path = true;

      for (uint8_t k = 0; k < state->loop_depth; k++)
      {
        if (existing->ordinal[k] != state->loops[k].ordinal)
        {
          same_path = false;
          break;
        }
      }

      if (same_path)
      {
        already = true;
        break;
      }
    }

    if (already)
      continue;

    YR_CT_DYN_REC* rec;
    FAIL_ON_ERROR(ct_new_record(
        state,
        &index->nodes[i],
        YR_CT_INST_SKIPPED,
        YR_CT_VALUE_UNDEFINED,
        &rec));
    rec->skip_reason = reason;
  }

  return ERROR_SUCCESS;
}

// ---------------------------------------------------------------------------
// VM hooks
// ---------------------------------------------------------------------------

int yr_ct_vm_before(
    YR_SCANNER* scanner,
    uint32_t code_offset,
    uint8_t opcode)
{
  // All evidence is collected in yr_ct_vm_after once the opcode's real effect
  // is known. The hook exists so the call sites stay symmetric and leaves room
  // for pre-execution bookkeeping without touching scan semantics.
  (void) scanner;
  (void) code_offset;
  (void) opcode;
  return ERROR_SUCCESS;
}

// Create an evaluated/unsupported instance for a node opcode and wire the
// instances of its actual operands as causal edges.
int yr_ct_vm_after(
    YR_SCANNER* scanner,
    uint32_t code_offset,
    uint8_t opcode,
    const YR_VALUE_STACK* stack)
{
  YR_CT_STATE* state = (YR_CT_STATE*) scanner->condition_trace_state;

  if (state == NULL)
    return ERROR_SUCCESS;

  const YR_CT_RT_NODE* rt = ct_rt_lookup(state, code_offset);

  // ---- Loop lifecycle --------------------------------------------------

  if (opcode == OP_ITER_START_ARRAY || opcode == OP_ITER_START_DICT ||
      opcode == OP_ITER_START_INT_RANGE || opcode == OP_ITER_START_INT_ENUM ||
      opcode == OP_ITER_START_STRING_SET ||
      opcode == OP_ITER_START_TEXT_STRING_SET)
  {
    if (state->loop_depth >= YR_MAX_LOOP_NESTING)
      return ERROR_INTERNAL_FATAL_ERROR;

    YR_CT_LOOP_FRAME* loop = &state->loops[state->loop_depth];
    memset(loop, 0, sizeof(*loop));
    loop->instance_id = YR_CT_INVALID_INSTANCE;
    loop->winning_instance = YR_CT_INVALID_INSTANCE;
    loop->failing_instance = YR_CT_INVALID_INSTANCE;

    state->loop_string_iter[state->loop_depth] =
        (opcode == OP_ITER_START_STRING_SET ||
         opcode == OP_ITER_START_TEXT_STRING_SET);

    state->loop_depth++;

    // The start opcode consumed however many constructor operands were on
    // the stack and pushed one iterator. Realign the parallel table to the
    // VM stack, then account for the iterator itself.
    while (state->slot_sp > stack->sp - 1 && state->slot_sp > 0)
      ct_slot_pop(state);

    FAIL_ON_ERROR(ct_slot_push(state, YR_CT_INVALID_INSTANCE));
    return ERROR_SUCCESS;
  }

  if (opcode == OP_ITER_NEXT)
  {
    if (state->loop_depth == 0)
      return ERROR_INTERNAL_FATAL_ERROR;

    YR_CT_LOOP_FRAME* loop = &state->loops[state->loop_depth - 1];

    if (stack->sp < 2)
      return ERROR_INTERNAL_FATAL_ERROR;

    // iter_next_func pushes the exhausted boolean first and the item on top.
    YR_VALUE item = stack->items[stack->sp - 1];
    int64_t exhausted = stack->items[stack->sp - 2].i;

    if (exhausted)
    {
      // End-of-iteration marker: both pushed values lack a real instance.
      FAIL_ON_ERROR(ct_slot_push(state, YR_CT_INVALID_INSTANCE));
      FAIL_ON_ERROR(ct_slot_push(state, YR_CT_INVALID_INSTANCE));
      return ERROR_SUCCESS;
    }

    loop->ordinal++;

    // The iteration kind comes from the ITER_START opcode recorded when the
    // loop opened. Inferring it from the item value is unsafe because a small
    // integer aliases a bogus YR_STRING pointer.
    if (state->loop_string_iter[state->loop_depth - 1])
    {
      loop->path_is_integer = false;
      loop->path_string = item.s != NULL ? item.s->identifier : NULL;
      loop->path_integer = YR_UNDEFINED;
    }
    else
    {
      loop->path_is_integer = true;
      loop->path_integer = item.i;
      loop->path_string = NULL;
    }

    if (rt == NULL)
      return ERROR_INTERNAL_FATAL_ERROR;

    YR_CT_DYN_REC* rec;
    FAIL_ON_ERROR(ct_new_record(
        state, rt, YR_CT_INST_EVALUATED, YR_CT_VALUE_UNDEFINED, &rec));

    if (loop->path_is_integer)
    {
      rec->value_type = YR_CT_VALUE_INTEGER;
      rec->ival = loop->path_integer;
    }
    else
    {
      rec->value_type = YR_CT_VALUE_STRING;
      rec->sval = yr_strdup(loop->path_string != NULL ? loop->path_string : "");
    }

    loop->instance_id = rec->id;

    // The exhausted boolean occupies the lower slot, the item the top one.
    FAIL_ON_ERROR(ct_slot_push(state, YR_CT_INVALID_INSTANCE));
    FAIL_ON_ERROR(ct_slot_push(state, rec->id));
    return ERROR_SUCCESS;
  }

  if (opcode == OP_ITER_CONDITION && state->loop_depth > 0)
  {
    YR_CT_LOOP_FRAME* loop = &state->loops[state->loop_depth - 1];

    // Post-instruction stack: [continue, body-result] with body on top.
    if (stack->sp >= 2)
    {
      int64_t continue_loop = stack->items[stack->sp - 2].i;
      int64_t body_result = stack->items[stack->sp - 1].i;

      // Mirror OP_ADD_M: a defined, true body contributes one match.
      if (!IS_UNDEFINED(body_result) && body_result)
        loop->true_count++;

      if (!continue_loop)
      {
        loop->decided = true;

        if (state->slot_sp > 0)
        {
          uint32_t body_inst =
              state->slots[state->slot_sp - 1].instance_id;

          if (body_result)
            loop->winning_instance = body_inst;
          else
            loop->failing_instance = body_inst;
        }
      }
    }

    // The VM popped min/true/body (3 slots) and pushed continue/body (2).
    uint32_t body_slot = YR_CT_INVALID_INSTANCE;

    if (state->slot_sp > 0)
      body_slot = state->slots[state->slot_sp - 1].instance_id;

    for (int i = 0; i < 3 && state->slot_sp > 0; i++)
      ct_slot_pop(state);

    FAIL_ON_ERROR(ct_slot_push(state, YR_CT_INVALID_INSTANCE));
    FAIL_ON_ERROR(ct_slot_push(state, body_slot));
    return ERROR_SUCCESS;
  }

  if (opcode == OP_ITER_END)
  {
    if (state->loop_depth == 0)
      return ERROR_INTERNAL_FATAL_ERROR;

    YR_CT_LOOP_FRAME* loop = &state->loops[state->loop_depth - 1];

    if (rt == NULL)
      return ERROR_INTERNAL_FATAL_ERROR;

    YR_CT_DYN_REC* rec;
    FAIL_ON_ERROR(ct_new_record(
        state, rt, YR_CT_INST_EVALUATED, YR_CT_VALUE_UNDEFINED, &rec));

    if (stack->sp > 0)
    {
      YR_VALUE top = stack->items[stack->sp - 1];

      if (IS_UNDEFINED(top.i))
        rec->value_type = YR_CT_VALUE_UNDEFINED;
      else
      {
        rec->value_type = YR_CT_VALUE_BOOLEAN;
        rec->ival = top.i ? 1 : 0;
      }
    }

    rec->iterations_visited = loop->ordinal;
    rec->iterations_true = loop->true_count;
    rec->winning_instance = loop->winning_instance;
    rec->failing_instance = loop->failing_instance;

    if (loop->ordinal == 0)
      rec->skip_reason = YR_CT_SKIP_LOOP_EMPTY;

    if (loop->winning_instance != YR_CT_INVALID_INSTANCE)
      FAIL_ON_ERROR(ct_add_edge(
          state, loop->winning_instance, rec->id));

    if (loop->failing_instance != YR_CT_INVALID_INSTANCE)
      FAIL_ON_ERROR(ct_add_edge(
          state, loop->failing_instance, rec->id));

    // ITER_END pops its three PUSH_M operands from the parallel stack.
    for (int i = 0; i < 3 && state->slot_sp > 0; i++)
    {
      uint32_t operand = ct_slot_pop(state);
      FAIL_ON_ERROR(ct_add_edge(state, operand, rec->id));
    }

    FAIL_ON_ERROR(ct_slot_push(state, rec->id));

    state->loop_depth--;
    return ERROR_SUCCESS;
  }

  // ---- mem-slot machinery ----------------------------------------------

  if (opcode == OP_POP_M)
  {
    uint64_t idx = (uint64_t) yr_unaligned_u64(
        scanner->rules->code_start + code_offset + 1);

    if (idx < CT_MEM_SIZE && state->slot_sp > 0)
      state->mem_instances[idx] = ct_slot_pop(state);

    return ERROR_SUCCESS;
  }

  if (opcode == OP_CLEAR_M)
  {
    uint64_t idx = (uint64_t) yr_unaligned_u64(
        scanner->rules->code_start + code_offset + 1);

    if (idx < CT_MEM_SIZE)
      state->mem_instances[idx] = YR_CT_INVALID_INSTANCE;

    return ERROR_SUCCESS;
  }

  if (opcode == OP_INCR_M)
  {
    // Counter slots are read back through OP_PUSH_M, whose value is taken
    // from the VM stack top, so no value mirroring is needed here.
    return ERROR_SUCCESS;
  }

  if (opcode == OP_ADD_M)
  {
    uint64_t idx = (uint64_t) yr_unaligned_u64(
        scanner->rules->code_start + code_offset + 1);

    if (idx < CT_MEM_SIZE && state->slot_sp > 0)
      state->mem_instances[idx] = ct_slot_pop(state);

    return ERROR_SUCCESS;
  }

  if (opcode == OP_PUSH_M && rt != NULL)
  {
    uint64_t idx = (uint64_t) yr_unaligned_u64(
        scanner->rules->code_start + code_offset + 1);

    YR_CT_DYN_REC* rec;
    FAIL_ON_ERROR(ct_new_record(
        state, rt, YR_CT_INST_EVALUATED, YR_CT_VALUE_UNDEFINED, &rec));

    if (idx < CT_MEM_SIZE)
      FAIL_ON_ERROR(ct_add_edge(
          state, state->mem_instances[idx], rec->id));

    // The VM just pushed mem[idx]; the stack top carries its current value.
    if (stack->sp > 0)
    {
      YR_VALUE top = stack->items[stack->sp - 1];

      if (IS_UNDEFINED(top.i))
        rec->value_type = YR_CT_VALUE_UNDEFINED;
      else
      {
        rec->value_type = YR_CT_VALUE_INTEGER;
        rec->ival = top.i;
      }
    }

    FAIL_ON_ERROR(ct_slot_push(state, rec->id));
    return ERROR_SUCCESS;
  }

  if (opcode == OP_SET_M || opcode == OP_SWAPUNDEF)
  {
    // Both opcodes leave the original value on the stack; keep its slot.
    return ERROR_SUCCESS;
  }

  if (opcode == OP_POP)
  {
    if (state->slot_sp > 0)
      ct_slot_pop(state);
    return ERROR_SUCCESS;
  }

  // ---- Rule boundaries -------------------------------------------------

  if (opcode == OP_INIT_RULE && rt != NULL)
  {
    uint32_t rule_idx = (uint32_t) yr_unaligned_u32(
        scanner->rules->code_start + code_offset + 1 + sizeof(int32_t));

    bool disabled =
        (scanner->rules->rules_table[rule_idx].flags &
         RULE_FLAGS_DISABLED) != 0;
    bool not_required = yr_bitmask_is_not_set(
        scanner->required_eval, rule_idx);

    YR_CT_DYN_REC* rec;
    FAIL_ON_ERROR(ct_new_record(
        state,
        rt,
        (disabled || not_required) ? YR_CT_INST_SKIPPED
                                   : YR_CT_INST_EVALUATED,
        YR_CT_VALUE_BOOLEAN,
        &rec));

    rec->ival = 0;

    if (disabled)
      rec->skip_reason = YR_CT_SKIP_RULE_DISABLED;
    else if (not_required)
      rec->skip_reason = YR_CT_SKIP_RULE_NOT_REQUIRED;

    state->rule_root_instance[rule_idx] = rec->id;

    if (disabled || not_required)
    {
      int32_t rel = (int32_t) yr_unaligned_u32(
          scanner->rules->code_start + code_offset + 1);
      uint32_t target = code_offset + (uint32_t) rel;
      uint32_t body_start =
          code_offset + 1 + (uint32_t) sizeof(int32_t) + sizeof(uint32_t);

      FAIL_ON_ERROR(ct_mark_range_skipped(
          state, body_start, target, rec->skip_reason));
    }

    return ERROR_SUCCESS;
  }

  if (opcode == OP_MATCH_RULE)
  {
    uint64_t rule_idx = (uint64_t) yr_unaligned_u64(
        scanner->rules->code_start + code_offset + 1);

    if (state->slot_sp > 0)
    {
      uint32_t condition_inst = ct_slot_pop(state);

      if (rule_idx < scanner->rules->num_rules &&
          state->rule_root_instance[rule_idx] != YR_CT_INVALID_INSTANCE)
      {
        uint32_t root = state->rule_root_instance[rule_idx];
        FAIL_ON_ERROR(ct_add_edge(state, condition_inst, root));

        if (stack->sp == 0 && state->rec_count > 0)
        {
          YR_CT_DYN_REC* root_rec = NULL;

          for (uint32_t i = 0; i < state->rec_count; i++)
          {
            if (state->recs[i].id == root)
            {
              root_rec = &state->recs[i];
              break;
            }
          }

          if (root_rec != NULL && root_rec->state == YR_CT_INST_EVALUATED &&
              condition_inst != YR_CT_INVALID_INSTANCE)
          {
            YR_CT_DYN_REC* cond_rec = NULL;

            for (uint32_t i = 0; i < state->rec_count; i++)
            {
              if (state->recs[i].id == condition_inst)
              {
                cond_rec = &state->recs[i];
                break;
              }
            }

            if (cond_rec != NULL)
            {
              root_rec->value_type = cond_rec->value_type;
              root_rec->ival = cond_rec->ival;
              root_rec->dval = cond_rec->dval;
            }
          }
        }
      }
    }

    return ERROR_SUCCESS;
  }

  // ---- Rule references link to the referenced rule's own result --------

  if (opcode == OP_PUSH_RULE && rt != NULL)
  {
    uint64_t ref_idx = (uint64_t) yr_unaligned_u64(
        scanner->rules->code_start + code_offset + 1);

    YR_CT_DYN_REC* rec;
    FAIL_ON_ERROR(ct_new_record(
        state, rt, YR_CT_INST_EVALUATED, YR_CT_VALUE_UNDEFINED, &rec));

    if (stack->sp > 0)
    {
      YR_VALUE top = stack->items[stack->sp - 1];

      if (IS_UNDEFINED(top.i))
        rec->value_type = YR_CT_VALUE_UNDEFINED;
      else
      {
        rec->value_type = YR_CT_VALUE_BOOLEAN;
        rec->ival = top.i ? 1 : 0;
      }
    }

    if (ref_idx < scanner->rules->num_rules &&
        state->rule_root_instance[ref_idx] != YR_CT_INVALID_INSTANCE)
    {
      FAIL_ON_ERROR(ct_add_edge(
          state, state->rule_root_instance[ref_idx], rec->id));
    }

    FAIL_ON_ERROR(ct_slot_push(state, rec->id));
    return ERROR_SUCCESS;
  }

  // ---- Short-circuit jumps ---------------------------------------------

  if ((opcode == OP_JFALSE || opcode == OP_JTRUE) && rt != NULL &&
      rt->meta->control_offset == code_offset)
  {
    int32_t rel = (int32_t) yr_unaligned_u32(
        scanner->rules->code_start + code_offset + 1);
    uint32_t fallthrough = code_offset + 1 + (uint32_t) sizeof(int32_t);
    uint32_t target = code_offset + (uint32_t) rel;

    // JFALSE/JTRUE don't pop: the condition value driving the jump stays on
    // the VM stack. Undefined never takes either jump.
    bool cond_defined = stack->sp > 0 &&
                        !IS_UNDEFINED(stack->items[stack->sp - 1].i);
    bool cond_true = stack->sp > 0 && stack->items[stack->sp - 1].i != 0;
    bool taken = cond_defined &&
                 (opcode == OP_JTRUE ? cond_true : !cond_true);

    if (taken && target > fallthrough)
    {
      FAIL_ON_ERROR(ct_mark_range_skipped(
          state,
          fallthrough,
          target,
          opcode == OP_JFALSE ? YR_CT_SKIP_AND_SHORT_CIRCUIT
                              : YR_CT_SKIP_OR_SHORT_CIRCUIT));
    }

    return ERROR_SUCCESS;
  }

  // Loop exit jump (first JTRUE_P of a loop): when the quantifier decided,
  // the remaining body iterations never execute. The VM pops its condition
  // slot regardless of whether the jump is taken.
  if (opcode == OP_JTRUE_P && rt != NULL &&
      rt->meta->loop_jump_offset == code_offset && state->loop_depth > 0)
  {
    YR_CT_LOOP_FRAME* loop = &state->loops[state->loop_depth - 1];

    if (state->slot_sp > 0)
      ct_slot_pop(state);

    if (loop->decided && rt->meta->loop_cond_offset != YR_CT_NO_OFFSET)
    {
      uint32_t body_start =
          code_offset + 1 + (uint32_t) sizeof(int32_t);

      FAIL_ON_ERROR(ct_mark_range_skipped(
          state,
          body_start,
          rt->meta->loop_cond_offset,
          YR_CT_SKIP_LOOP_NOT_ENTERED));
    }

    return ERROR_SUCCESS;
  }

  // ---- Generic node opcodes --------------------------------------------

  if (rt == NULL || rt->meta->code_offset != code_offset)
  {
    // Opcodes without a node must still keep the parallel stack aligned. The
    // authoritative source is the VM stack: after the opcode it has stack->sp
    // values while the parallel table still mirrors the pre-opcode stack.
    while (state->slot_sp > stack->sp)
      ct_slot_pop(state);

    while (state->slot_sp < stack->sp)
      FAIL_ON_ERROR(ct_slot_push(state, YR_CT_INVALID_INSTANCE));

    return ERROR_SUCCESS;
  }

  // The opcode pushed exactly one result. The operand count is therefore
  // derived from the invariant slot_sp == pre-opcode VM sp:
  //   post_sp = slot_sp - consumed + 1  =>  consumed = slot_sp + 1 - post_sp.
  // This also covers OP_OF, whose operand count is the whole string/rule set
  // plus its quantifier.
  int consumed = (int) (state->slot_sp + 1 - stack->sp);

  if (consumed < 0)
    consumed = 0;

  if (consumed > (int) state->slot_sp)
    consumed = (int) state->slot_sp;

  uint32_t new_instance = YR_CT_INVALID_INSTANCE;
  {
    YR_CT_INSTANCE_STATE initial_state = YR_CT_INST_EVALUATED;

    if (rt->meta->flags & YR_CT_STATIC_FLAG_UNSUPPORTED)
      initial_state = YR_CT_INST_UNSUPPORTED;
    // Literal constants are resolved by the compiler into OP_PUSH_* opcodes:
    // they are compile-time values, not runtime evaluations, so they surface
    // as FOLDED with the source node preserved through fold_origin_node_id.
    else if (rt->meta->flags & YR_CT_STATIC_FLAG_COMPILE_TIME)
      initial_state = YR_CT_INST_FOLDED;

    YR_CT_DYN_REC* created;
    FAIL_ON_ERROR(ct_new_record(
        state, rt, initial_state, YR_CT_VALUE_UNDEFINED, &created));

    new_instance = created->id;
  }

  // The record just created.
  YR_CT_DYN_REC* rec = &state->recs[state->rec_count - 1];

  if (rec->state == YR_CT_INST_UNSUPPORTED)
    rec->skip_reason = YR_CT_SKIP_UNSUPPORTED;

  // Read the produced value from the VM stack top.
  if (stack->sp > 0)
  {
    YR_VALUE top = stack->items[stack->sp - 1];
    YR_CT_VALUE_TYPE vtype = ct_value_type_for(rt->meta);

    if (IS_UNDEFINED(top.i))
    {
      rec->value_type = YR_CT_VALUE_UNDEFINED;
    }
    else if (vtype == YR_CT_VALUE_FLOAT)
    {
      rec->value_type = YR_CT_VALUE_FLOAT;
      rec->dval = top.d;
    }
    else if (vtype == YR_CT_VALUE_STRING)
    {
      rec->value_type = YR_CT_VALUE_STRING;

      if (top.ss != NULL)
      {
        size_t n = sizeof(SIZED_STRING) + top.ss->length + 1;
        rec->sval = (char*) yr_malloc(n);

        if (rec->sval != NULL)
          memcpy(rec->sval, top.ss, n);
      }
    }
    else if (vtype == YR_CT_VALUE_BOOLEAN)
    {
      rec->value_type = YR_CT_VALUE_BOOLEAN;
      rec->ival = top.i ? 1 : 0;
    }
    else
    {
      rec->value_type = YR_CT_VALUE_INTEGER;
      rec->ival = top.i;
    }
  }

  for (int i = 0; i < consumed; i++)
  {
    uint32_t operand = ct_slot_pop(state);
    FAIL_ON_ERROR(ct_add_edge(state, operand, rec->id));
  }

  FAIL_ON_ERROR(ct_slot_push(state, new_instance));

  return ERROR_SUCCESS;
}

// ---------------------------------------------------------------------------
// Decisions
// ---------------------------------------------------------------------------

int yr_ct_record_decisions(YR_SCANNER* scanner)
{
  YR_CT_STATE* state = (YR_CT_STATE*) scanner->condition_trace_state;

  if (state == NULL)
    return ERROR_SUCCESS;

  YR_RULES* rules = scanner->rules;

  for (uint32_t i = 0; i < rules->num_rules; i++)
  {
    YR_RULE* rule = &rules->rules_table[i];
    YR_CT_DECISION* d = &state->decisions[i];

    bool condition_true = yr_bitmask_is_set(scanner->rule_matches_flags, i);
    bool ns_blocked =
        yr_bitmask_is_set(scanner->ns_unsatisfied_flags, rule->ns->idx);
    bool reported = condition_true && !ns_blocked;

    d->condition_true = condition_true;
    d->reported_match = reported;

    if (yr_bitmask_is_set(scanner->required_eval, i) &&
        !(rule->flags & RULE_FLAGS_DISABLED))
    {
      d->state = YR_CT_INST_EVALUATED;
      d->skip_reason = YR_CT_SKIP_NONE;
    }
    else
    {
      d->state = YR_CT_INST_SKIPPED;
      d->skip_reason = (rule->flags & RULE_FLAGS_DISABLED)
                           ? YR_CT_SKIP_RULE_DISABLED
                           : YR_CT_SKIP_RULE_NOT_REQUIRED;
    }

    if (condition_true && ns_blocked)
    {
      d->blocked_by_global_rule_idx = YR_CT_INVALID_RULE;

      for (uint32_t j = 0; j < rules->num_rules; j++)
      {
        YR_RULE* candidate = &rules->rules_table[j];

        if ((candidate->flags & RULE_FLAGS_GLOBAL) &&
            candidate->ns == rule->ns &&
            !yr_bitmask_is_set(scanner->rule_matches_flags, j))
        {
          d->blocked_by_global_rule_idx = j;
          break;
        }
      }
    }
  }

  return ERROR_SUCCESS;
}

// ---------------------------------------------------------------------------
// Snapshots
// ---------------------------------------------------------------------------

static char* ct_dup_str(const char* s)
{
  return s == NULL ? NULL : yr_strdup(s);
}

int yr_ct_take_snapshot(
    YR_SCANNER* scanner,
    YR_CONDITION_TRACE** out)
{
  *out = NULL;

  YR_CT_STATE* state = (YR_CT_STATE*) scanner->condition_trace_state;

  if (state == NULL)
    return ERROR_SUCCESS;

  YR_RULES* rules = scanner->rules;
  YR_CT_RT_INDEX* index = (YR_CT_RT_INDEX*) rules->condition_trace_rt;

  YR_CONDITION_TRACE* trace = (YR_CONDITION_TRACE*) yr_calloc(
      1, sizeof(YR_CONDITION_TRACE));

  if (trace == NULL)
    return ERROR_INSUFFICIENT_MEMORY;

  trace->incomplete = state->incomplete;
  trace->num_nodes = rules->condition_trace_num_nodes;
  trace->num_instances = state->rec_count;
  trace->num_edges = state->edge_count;
  trace->num_decisions = rules->num_rules;

  trace->nodes = (YR_CT_NODE*) yr_calloc(
      trace->num_nodes, sizeof(YR_CT_NODE));
  trace->instances = (YR_CT_INSTANCE*) yr_calloc(
      trace->num_instances, sizeof(YR_CT_INSTANCE));
  trace->edges = (YR_CT_EDGE*) yr_calloc(
      trace->num_edges, sizeof(YR_CT_EDGE));
  trace->decisions = (YR_CT_RULE_DECISION*) yr_calloc(
      trace->num_decisions, sizeof(YR_CT_RULE_DECISION));

  if (trace->nodes == NULL || trace->instances == NULL ||
      trace->edges == NULL || trace->decisions == NULL)
  {
    yr_condition_trace_destroy(trace);
    return ERROR_INSUFFICIENT_MEMORY;
  }

  // Static child ids from the actual causal edges observed between nodes.
  uint32_t* child_counts = (uint32_t*) yr_calloc(
      trace->num_nodes, sizeof(uint32_t));
  uint32_t** child_lists = (uint32_t**) yr_calloc(
      trace->num_nodes, sizeof(uint32_t*));

  if (child_counts == NULL || child_lists == NULL)
  {
    yr_free(child_counts);
    yr_free(child_lists);
    yr_condition_trace_destroy(trace);
    return ERROR_INSUFFICIENT_MEMORY;
  }

  for (uint32_t e = 0; e < state->edge_count; e++)
  {
    uint32_t from_node = state->recs[state->edges[e].from_instance].node_id;
    uint32_t to_node = state->recs[state->edges[e].to_instance].node_id;
    uint32_t n = child_counts[to_node];
    bool present = false;

    for (uint32_t k = 0; k < n; k++)
    {
      if (child_lists[to_node][k] == from_node)
      {
        present = true;
        break;
      }
    }

    if (present)
      continue;

    uint32_t* grown = (uint32_t*) yr_realloc(
        child_lists[to_node], (n + 1) * sizeof(uint32_t));

    if (grown == NULL)
    {
      for (uint32_t k = 0; k < trace->num_nodes; k++)
        yr_free(child_lists[k]);
      yr_free(child_lists);
      yr_free(child_counts);
      yr_condition_trace_destroy(trace);
      return ERROR_INSUFFICIENT_MEMORY;
    }

    grown[n] = from_node;
    child_lists[to_node] = grown;
    child_counts[to_node] = n + 1;
  }

  for (uint32_t i = 0; i < trace->num_nodes; i++)
  {
    const YR_CT_RT_NODE* rt = &index->nodes[i];
    YR_CT_NODE* node = (YR_CT_NODE*) &trace->nodes[i];

    node->id = rt->meta->id;
    node->kind = (YR_CT_NODE_KIND) rt->meta->kind;
    node->rule_idx = rt->meta->rule_idx == UINT32_MAX
                         ? YR_CT_INVALID_RULE
                         : rt->meta->rule_idx;
    node->namespace_name = ct_dup_str(rt->namespace_name);
    node->rule_name = ct_dup_str(rt->rule_name);
    node->file_name = ct_dup_str(rt->file_name);
    node->end_line = rt->meta->end_line;
    node->fold_origin_node_id = rt->meta->fold_origin_id;
    node->unsupported =
        (rt->meta->flags & YR_CT_STATIC_FLAG_UNSUPPORTED) != 0;
    node->detail = ct_dup_str(rt->detail);
    node->child_count = child_counts[i];
    node->child_ids = child_lists[i];
  }

  yr_free(child_counts);
  yr_free(child_lists);

  for (uint32_t i = 0; i < state->rec_count; i++)
  {
    YR_CT_DYN_REC* src = &state->recs[i];
    YR_CT_INSTANCE* dst = (YR_CT_INSTANCE*) &trace->instances[i];

    dst->id = src->id;
    dst->node_id = src->node_id;
    dst->state = src->state;
    dst->short_circuit_reason = src->skip_reason;
    dst->iteration_depth = src->depth;
    dst->value_type = src->value_type;

    // The value is a union: assign only the member matching the recorded type
    // so the double representation can't clobber an integer/boolean value.
    if (src->value_type == YR_CT_VALUE_FLOAT)
      dst->value.d = src->dval;
    else
      dst->value.i = src->ival;

    dst->string_value = ct_dup_str(src->sval);
    dst->iterations_visited = src->iterations_visited;
    dst->iterations_true = src->iterations_true;
    dst->winning_instance = src->winning_instance;
    dst->failing_instance = src->failing_instance;

    if (src->depth > 0)
    {
      YR_CT_ITERATION* path = (YR_CT_ITERATION*) yr_calloc(
          src->depth, sizeof(YR_CT_ITERATION));

      if (path == NULL)
      {
        yr_condition_trace_destroy(trace);
        return ERROR_INSUFFICIENT_MEMORY;
      }

      for (uint8_t k = 0; k < src->depth; k++)
      {
        path[k].ordinal = src->ordinal[k];
        path[k].is_integer = src->path_is_integer[k];
        path[k].integer_value = src->path_integer[k];
        path[k].string_identifier = ct_dup_str(src->path_string[k]);
      }

      dst->iteration_path = path;
    }
  }

  YR_CT_EDGE* mutable_edges = (YR_CT_EDGE*) trace->edges;

  for (uint32_t i = 0; i < state->edge_count; i++)
  {
    mutable_edges[i].from_instance = state->edges[i].from_instance;
    mutable_edges[i].to_instance = state->edges[i].to_instance;
  }

  YR_CT_RULE_DECISION* mutable_decisions =
      (YR_CT_RULE_DECISION*) trace->decisions;

  for (uint32_t i = 0; i < rules->num_rules; i++)
  {
    YR_CT_RULE_DECISION* dst = &mutable_decisions[i];
    YR_CT_DECISION* src = &state->decisions[i];

    dst->rule_idx = src->rule_idx;
    dst->condition_true = src->condition_true;
    dst->reported_match = src->reported_match;
    dst->state = src->state;
    dst->skip_reason = src->skip_reason;
    dst->blocked_by_global_rule_idx = src->blocked_by_global_rule_idx;
  }

  yr_ct_state_destroy(state);
  scanner->condition_trace_state = NULL;

  *out = trace;
  return ERROR_SUCCESS;
}

void yr_ct_snapshot_free(YR_CONDITION_TRACE* trace)
{
  if (trace == NULL)
    return;

  if (trace->nodes != NULL)
  {
    for (uint32_t i = 0; i < trace->num_nodes; i++)
    {
      yr_free((void*) trace->nodes[i].namespace_name);
      yr_free((void*) trace->nodes[i].rule_name);
      yr_free((void*) trace->nodes[i].file_name);
      yr_free((void*) trace->nodes[i].detail);
      yr_free((void*) trace->nodes[i].child_ids);
    }
    yr_free((void*) trace->nodes);
  }

  if (trace->instances != NULL)
  {
    for (uint32_t i = 0; i < trace->num_instances; i++)
    {
      yr_free((void*) trace->instances[i].string_value);

      if (trace->instances[i].iteration_path != NULL)
      {
        for (uint32_t k = 0; k < trace->instances[i].iteration_depth; k++)
          yr_free((void*) trace->instances[i].iteration_path[k]
                      .string_identifier);
        yr_free((void*) trace->instances[i].iteration_path);
      }
    }
    yr_free((void*) trace->instances);
  }

  yr_free((void*) trace->edges);
  yr_free((void*) trace->decisions);
  yr_free(trace);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

YR_API void yr_compiler_set_condition_trace(
    YR_COMPILER* compiler,
    int enabled)
{
  if (compiler == NULL)
    return;

  compiler->condition_trace_enabled = enabled != 0;

  if (enabled)
    yr_ct_meta_begin(compiler);
}

YR_API int yr_scanner_set_condition_trace(YR_SCANNER* scanner, int enabled)
{
  if (scanner == NULL)
    return ERROR_INVALID_ARGUMENT;

  if (enabled && !yr_ct_rules_have_meta(scanner->rules))
    return ERROR_CONDITION_TRACE_MISSING;

  scanner->condition_trace_enabled = enabled != 0;
  return ERROR_SUCCESS;
}

YR_API int yr_scanner_take_condition_trace(
    YR_SCANNER* scanner,
    YR_CONDITION_TRACE** out)
{
  if (scanner == NULL || out == NULL)
    return ERROR_INVALID_ARGUMENT;

  *out = NULL;

  if (!scanner->condition_trace_enabled)
    return ERROR_SUCCESS;

  return yr_ct_take_snapshot(scanner, out);
}

YR_API void yr_condition_trace_destroy(YR_CONDITION_TRACE* trace)
{
  yr_ct_snapshot_free(trace);
}
