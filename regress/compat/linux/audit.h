/* SPDX-License-Identifier: Apache-2.0
 * Linux audit UAPI architecture IDs, no host syscall dependency. */
#ifndef CHECK_AUDIT_H
#define CHECK_AUDIT_H
#define EM_ARM 40
#define AUDIT_ARCH_ARM (40|0x40000000U)
#define AUDIT_ARCH_AARCH64 (183|0xc0000000U)
#define AUDIT_ARCH_I386 (3|0x40000000U)
#define AUDIT_ARCH_X86_64 (62|0xc0000000U)
#define AUDIT_ARCH_MIPS 8
#define AUDIT_ARCH_MIPS64 (8|0x80000000U)
#define AUDIT_ARCH_RISCV64 (243|0xc0000000U)
#define AUDIT_ARCH_M68K 4
#endif
