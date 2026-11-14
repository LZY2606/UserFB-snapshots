#!/bin/bash
set -euo pipefail
mkdir -p .task-build
make -s -j2 CC="${CC:-cc}" build/librabbitizer.a build/tests/c/instruction_checks/jalr.elf build/tests/c/registersTrackerTest.elf build/tests/c/instruction_checks/plain_disassembly.elf build/tests/c/logic_checks/descriptor_logic_check.elf
${CC:-cc} -std=c11 -O0 -Iinclude -Itables task_support/legacy.c build/librabbitizer.a -o .task-build/legacy
