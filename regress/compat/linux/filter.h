/* SPDX-License-Identifier: Apache-2.0
 * Portable classic-BPF userspace ABI shim; no kernel API implementation.
 * Numeric ABI: Linux UAPI linux/filter.h and linux/bpf_common.h.
 */
#ifndef CHECK_FILTER_H
#define CHECK_FILTER_H
#include <stdint.h>
typedef uint32_t __u32; typedef uint16_t __u16; typedef uint8_t __u8;
struct sock_filter { uint16_t code; uint8_t jt,jf; uint32_t k; };
struct sock_fprog { unsigned short len; struct sock_filter *filter; };
#define BPF_MEMWORDS 16
#define BPF_STMT(c,k) { (uint16_t)(c), 0, 0, k }
#define BPF_JUMP(c,k,jt,jf) { (uint16_t)(c), jt, jf, k }
#define BPF_LD 0x00
#define BPF_LDX 0x01
#define BPF_ST 0x02
#define BPF_STX 0x03
#define BPF_ALU 0x04
#define BPF_JMP 0x05
#define BPF_RET 0x06
#define BPF_MISC 0x07
#define BPF_W 0x00
#define BPF_H 0x08
#define BPF_B 0x10
#define BPF_IMM 0x00
#define BPF_ABS 0x20
#define BPF_IND 0x40
#define BPF_MEM 0x60
#define BPF_LEN 0x80
#define BPF_MSH 0xa0
#define BPF_ADD 0x00
#define BPF_SUB 0x10
#define BPF_MUL 0x20
#define BPF_DIV 0x30
#define BPF_OR 0x40
#define BPF_AND 0x50
#define BPF_LSH 0x60
#define BPF_RSH 0x70
#define BPF_NEG 0x80
#define BPF_MOD 0x90
#define BPF_XOR 0xa0
#define BPF_JA 0x00
#define BPF_JEQ 0x10
#define BPF_JGT 0x20
#define BPF_JGE 0x30
#define BPF_JSET 0x40
#define BPF_K 0x00
#define BPF_X 0x08
#define BPF_A 0x10
#define BPF_TAX 0x00
#define BPF_TXA 0x80
#define BPF_CLASS(c) ((c)&0x07)
#define BPF_OP(c) ((c)&0xf0)
#define BPF_SRC(c) ((c)&0x08)
#endif
