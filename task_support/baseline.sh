#!/bin/bash
set -euo pipefail
c++ -std=c++17 -O1 -I. -I.task-build/lib -I.task-build/lib/libde265 task_support/baseline.cpp .task-build/lib/libde265/libde265.a -lpthread -o .task-build/baseline
.task-build/baseline
c++ -std=c++17 -DHAVE_CONFIG_H -I. -I.task-build/lib -I.task-build/lib/libde265 task_support/old_cabac_main.cpp task_support/upstream/test-cabac.cc .task-build/lib/libde265/libde265.a -lpthread -o .task-build/upstream-cabac
.task-build/upstream-cabac
