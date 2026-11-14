/*
 * Copyright 2026 WebAssembly Community Group participants
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "label-names.h"

#include <cinttypes>
#include <unordered_set>

#include "wabt/cast.h"
#include "wabt/string-format.h"

namespace wabt {

namespace {

// Iteration frame for a depth-first walk that preserves the binary open order
// of structured instructions.
struct Frame {
  ExprList* exprs;
  ExprList::iterator it;
};

// Pushes the child expression lists that contain block-like nodes. For an if,
// the true arm is walked completely before the false arm because both arms take
// part in the same label-index sequence.
void PushChildren(std::vector<Frame>* stack, Expr& expr) {
  switch (expr.type()) {
    case ExprType::Block: {
      auto* block_expr = cast<BlockExpr>(&expr);
      stack->push_back({&block_expr->block.exprs,
                        block_expr->block.exprs.begin()});
      break;
    }
    case ExprType::Loop: {
      auto* loop_expr = cast<LoopExpr>(&expr);
      stack->push_back({&loop_expr->block.exprs,
                        loop_expr->block.exprs.begin()});
      break;
    }
    case ExprType::If: {
      auto* if_expr = cast<IfExpr>(&expr);
      // LIFO: enqueue the false arm first so the true arm is visited first.
      stack->push_back({&if_expr->false_, if_expr->false_.begin()});
      stack->push_back({&if_expr->true_.exprs,
                        if_expr->true_.exprs.begin()});
      break;
    }
    case ExprType::Try: {
      auto* try_expr = cast<TryExpr>(&expr);
      stack->push_back({&try_expr->block.exprs,
                        try_expr->block.exprs.begin()});
      break;
    }
    case ExprType::TryTable: {
      auto* try_table_expr = cast<TryTableExpr>(&expr);
      stack->push_back({&try_table_expr->block.exprs,
                        try_table_expr->block.exprs.begin()});
      break;
    }
    default:
      break;
  }
}

bool IsBlockLike(ExprType type) {
  return type == ExprType::Block || type == ExprType::Loop ||
         type == ExprType::If || type == ExprType::Try ||
         type == ExprType::TryTable;
}

// Only block/loop/if take part in the Label subsection numbering. try and
// try_table are still traversed so nodes nested in their bodies keep the same
// binary open order, but they do not occupy a label index themselves.
bool IsNumberedLabel(ExprType type) {
  return type == ExprType::Block || type == ExprType::Loop ||
         type == ExprType::If;
}

Block* BlockOf(Expr* expr) {
  switch (expr->type()) {
    case ExprType::Block:
      return &cast<BlockExpr>(expr)->block;
    case ExprType::Loop:
      return &cast<LoopExpr>(expr)->block;
    case ExprType::If:
      return &cast<IfExpr>(expr)->true_;
    case ExprType::Try:
      return &cast<TryExpr>(expr)->block;
    case ExprType::TryTable:
      return &cast<TryTableExpr>(expr)->block;
    default:
      return nullptr;
  }
}

// Same "$" prefix convention as function and local debug names; the text
// writer rewrites any identifier-invalid characters.
std::string MakeDollarName(std::string_view name) {
  return std::string("$") + std::string(name);
}

}  // namespace

std::vector<Block*> CollectStructuredLabelNodes(Func* func) {
  std::vector<Block*> nodes;
  std::vector<Frame> stack;
  stack.push_back({&func->exprs, func->exprs.begin()});
  while (!stack.empty()) {
    Frame& frame = stack.back();
    if (frame.it == frame.exprs->end()) {
      stack.pop_back();
      continue;
    }
    Expr& expr = *frame.it++;
    // Only block/loop/if occupy a Label subsection slot; try/try_table are
    // descended into by PushChildren but are not themselves numbered.
    if (IsNumberedLabel(expr.type())) {
      if (Block* block = BlockOf(&expr)) {
        nodes.push_back(block);
      }
    }
    PushChildren(&stack, expr);
  }
  return nodes;
}

void ForEachBlockLabel(Func* func,
                       const std::function<void(const std::string&)>& callback) {
  std::vector<Frame> stack;
  stack.push_back({&func->exprs, func->exprs.begin()});
  while (!stack.empty()) {
    Frame& frame = stack.back();
    if (frame.it == frame.exprs->end()) {
      stack.pop_back();
      continue;
    }
    Expr& expr = *frame.it++;
    if (IsBlockLike(expr.type())) {
      if (Block* block = BlockOf(&expr); block && !block->label.empty()) {
        callback(block->label);
      }
    }
    PushChildren(&stack, expr);
  }
}

Result LabelNameTable::Apply(Module* module,
                             Errors* errors,
                             bool strict) const {
  Result result = Result::Ok;
  for (const FunctionEntries& fentry : functions_) {
    if (fentry.func_index >= module->funcs.size()) {
      // The structural reader already validates function indices; keep this
      // defensive guard so a standalone application stays safe.
      if (strict) {
        errors->emplace_back(
            ErrorLevel::Error, fentry.loc, module->filename,
            StringPrintf("invalid label name function index: %" PRIindex,
                         fentry.func_index));
        result = Result::Error;
      }
      continue;
    }

    if (fentry.func_index < module->num_func_imports) {
      // Imported functions have no bodies and therefore no structured labels;
      // even an entry with an empty name makes the label table non-empty.
      if (!fentry.entries.empty()) {
        if (strict) {
          errors->emplace_back(
              ErrorLevel::Error, fentry.loc, module->filename,
              StringPrintf("imported function %" PRIindex
                           " cannot have a non-empty label name table",
                           fentry.func_index));
          result = Result::Error;
        }
      }
      continue;
    }

    Func* func = module->funcs[fentry.func_index];
    std::vector<Block*> nodes = CollectStructuredLabelNodes(func);

    // Names already handed out within this function, so debug names given to
    // different nodes get stable, distinct identifiers.
    std::unordered_set<std::string> used_names;
    for (const Entry& entry : fentry.entries) {
      if (entry.name.empty()) {
        // An empty name explicitly provides no name for the node.
        continue;
      }
      if (entry.label_index >= nodes.size()) {
        if (strict) {
          errors->emplace_back(
              ErrorLevel::Error, entry.loc, module->filename,
              StringPrintf(
                  "invalid label index: %" PRIindex " (function %" PRIindex
                  " has %" PRIzx " structured labels)",
                  entry.label_index, fentry.func_index, nodes.size()));
          result = Result::Error;
        }
        continue;
      }

      const std::string base_name = MakeDollarName(entry.name);
      std::string chosen = base_name;
      unsigned suffix = 1;
      while (used_names.count(chosen) != 0) {
        chosen = base_name + "." + std::to_string(suffix++);
      }
      used_names.insert(chosen);
      nodes[entry.label_index]->label = chosen;
    }
  }
  return result;
}

}  // namespace wabt
