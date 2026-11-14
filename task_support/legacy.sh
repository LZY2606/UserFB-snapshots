#!/bin/bash
set -euo pipefail
build/tests/c/instruction_checks/jalr.elf
build/tests/c/registersTrackerTest.elf
build/tests/c/instruction_checks/plain_disassembly.elf
build/tests/c/logic_checks/descriptor_logic_check.elf
.task-build/legacy
printf 'BASELINE_OK\n'
