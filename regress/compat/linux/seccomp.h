/* SPDX-License-Identifier: Apache-2.0
 * Linux seccomp UAPI layout/constants only, for offline interpreter inputs. */
#ifndef CHECK_SECCOMP_H
#define CHECK_SECCOMP_H
#include <stdint.h>
struct seccomp_data { int nr; uint32_t arch; uint64_t instruction_pointer; uint64_t args[6]; };
#define SECCOMP_RET_KILL 0x00000000U
#define SECCOMP_RET_KILL_THREAD SECCOMP_RET_KILL
#define SECCOMP_RET_KILL_PROCESS 0x80000000U
#define SECCOMP_RET_TRAP 0x00030000U
#define SECCOMP_RET_ERRNO 0x00050000U
#define SECCOMP_RET_TRACE 0x7ff00000U
#define SECCOMP_RET_LOG 0x7ffc0000U
#define SECCOMP_RET_ALLOW 0x7fff0000U
#define SECCOMP_RET_USER_NOTIF 0x7fc00000U
#define SECCOMP_RET_ACTION 0x7fff0000U
#define SECCOMP_RET_ACTION_FULL 0xffff0000U
#define SECCOMP_RET_DATA 0x0000ffffU
#endif
