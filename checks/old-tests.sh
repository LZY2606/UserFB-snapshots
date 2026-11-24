#!/bin/bash
set -euo pipefail
: > .task-build/all_tests.c
cc -std=c99 -DLIBXMP_STATIC -Iinclude -Isrc -I.task-build \
 checks/old-tests.c test-dev/util.c test-dev/simple_module.c \
 test-dev/test_api_play_buffer.c test-dev/test_api_inject_event.c \
 test-dev/test_player_nna_cont.c test-dev/test_api_restart_module.c \
 .task-build/lib/libxmp.a -lm -o .task-build/old-tests
(cd test-dev && ../.task-build/old-tests)
