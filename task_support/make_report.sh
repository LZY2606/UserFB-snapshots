#!/bin/bash
# Complete offline command sequence for building the raw_pdb library and generating
# analysis/module_symbols.tsv from the three synthetic container inputs.
# Does not execute any analyzed image and does not modify input fixtures.
set -euo pipefail
cd "$(dirname "$0")/.."

CXX_BIN=${CXX:-c++}

# 1. Build the static library from every src/*.cpp with C++17.
bash task_support/build.sh

# 2. Compile the standalone report example against the task_support fixtures.
"$CXX_BIN" -std=c++17 -O2 -I. -Isrc -Itask_support \
  src/Examples/ExampleModuleSymbolReport.cpp \
  .task-build/libraw_pdb.a \
  -o .task-build/module_symbol_report

# 3. Generate the TSV from actual ValidateFile/CreateRawFile/analysis reads.
mkdir -p analysis
./.task-build/module_symbol_report analysis/module_symbols.tsv

echo "report written to analysis/module_symbols.tsv"
