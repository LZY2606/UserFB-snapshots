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

// Internal helper for the Label subsection (id 3) of the name custom
// section. The binary reader feeds the raw indirect name table here; the
// names are applied to the structured control nodes (block/loop/if) after the
// whole module has been read, so the table works regardless of whether the
// name section precedes or follows the code section.

#ifndef WABT_LABEL_NAMES_H_
#define WABT_LABEL_NAMES_H_

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "wabt/common.h"
#include "wabt/error.h"
#include "wabt/ir.h"

namespace wabt {

// Returns the block/loop/if nodes of a function in the order in which their
// opening instructions appear in the binary. The implicit function exit,
// else and end do not occupy a slot; nodes nested in either arm of an if take
// part in the same sequence (the true arm is visited before the false arm).
std::vector<Block*> CollectStructuredLabelNodes(Func* func);

// Invokes |callback| for every non-empty label carried by a block-like node
// (block, loop, if, try and try_table) in binary open order. The implicit
// function exit has no label in the IR and is not visited. Used to reserve
// names that automatic name generation must not collide with.
void ForEachBlockLabel(Func* func,
                       const std::function<void(const std::string&)>& callback);

// Raw contents of the Label subsection, kept independent of the input bytes:
// all strings are copied.
class LabelNameTable {
 public:
  struct Entry {
    Index label_index;
    std::string name;
    Location loc;
  };

  struct FunctionEntries {
    Index func_index;
    Location loc;
    std::vector<Entry> entries;
  };

  void AddFunction(Index func_index, Location loc) {
    functions_.push_back(FunctionEntries{func_index, loc, {}});
  }

  void AddEntry(Index label_index, std::string_view name, Location loc) {
    assert(!functions_.empty());
    functions_.back().entries.push_back(
        Entry{label_index, std::string(name), loc});
  }

  bool empty() const { return functions_.empty(); }

  // Assigns the stored names to the control nodes of the module. Empty names
  // name nothing. Within a function, the first node keeping a debug name keeps
  // it verbatim; later nodes given the same name receive the smallest unused
  // ".N" suffix. In |strict| mode, label indices outside the function's node
  // range and label tables attached to imported functions are reported as
  // positioned errors; otherwise the offending entries are skipped.
  Result Apply(Module* module, Errors* errors, bool strict) const;

 private:
  std::vector<FunctionEntries> functions_;
};

}  // namespace wabt

#endif /* WABT_LABEL_NAMES_H_ */
