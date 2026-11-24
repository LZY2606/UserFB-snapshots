// Forwarding shim so the mandated literal `#include "task_support/check.h"`
// resolves from tests/golomb_contract.cpp under the fixed build flags
// (`-Icodec -Itask_support`, with no `-I.`).  The real contract header and
// its task_check/TASK_CHECK definitions remain task_support/check.h.
#ifndef GOLOMB_TESTS_TASK_SUPPORT_CHECK_SHIM_H
#define GOLOMB_TESTS_TASK_SUPPORT_CHECK_SHIM_H
#include "../../task_support/check.h"
#endif
