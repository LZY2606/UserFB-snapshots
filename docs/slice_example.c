/* docs/slice_example.c - example of the libudis86 backward slicing API.
 *
 * Build (from the top of the source tree):
 *
 *   cc -std=c99 -DHAVE_STRING_H=1 -I. \
 *      libudis86/itab.c libudis86/decode.c libudis86/syn.c \
 *      libudis86/syn-intel.c libudis86/syn-att.c libudis86/udis86.c \
 *      libudis86/slice.c docs/slice_example.c -o slice_example
 *
 * or, against an installed library:
 *
 *   gcc -ludis86 slice_example.c -o slice_example
 *
 * The program slices a small straight-line fragment, asking which
 * instructions the final value of the low 32 bits of rax depends on.
 */
#include <stdio.h>
#include <string.h>

#include "udis86.h"
#include "libudis86/slice.h"

int
main(void)
{
  /* movzx eax, cl ; add eax, edx ; mov ecx, eax */
  const uint8_t code[] = { 0x0f, 0xb6, 0xc1, 0x01, 0xd0, 0x89, 0xc1 };
  const char *names[UD_SLICE_NUM_REGS] = {
    "rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
    "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"
  };
  uint64_t mask[UD_SLICE_NUM_REGS];
  struct ud_slice *slice = NULL;
  size_t i;
  int g, status;

  memset(mask, 0, sizeof(mask));
  mask[UD_SLICE_RAX] = 0xffffffffull;   /* track eax at the end */

  status = ud_slice(code, sizeof code, mask, 0, &slice);
  if (status != UD_SLICE_OK) {
    printf("slice failed: status=%d offset=%llu\n", status,
           (unsigned long long)slice->error_offset);
    ud_slice_free(slice);
    return 1;
  }

  printf("instructions in the slice:\n");
  for (i = 0; i < slice->insn_count; i++) {
    const struct ud_slice_insn *in = &slice->insns[i];
    printf("  offset=%llu writes:", (unsigned long long)in->offset);
    for (g = 0; g < UD_SLICE_NUM_REGS; g++) {
      if (in->reg_bits[g] != 0) {
        printf(" %s=0x%llx", names[g],
               (unsigned long long)in->reg_bits[g]);
      }
    }
    if (in->flag_bits != 0) {
      printf(" flags=0x%x", in->flag_bits);
    }
    printf("\n");
  }

  printf("still live at fragment entry:\n");
  for (g = 0; g < UD_SLICE_NUM_REGS; g++) {
    if (slice->entry_regs[g] != 0) {
      printf("  %s bits 0x%llx\n", names[g],
             (unsigned long long)slice->entry_regs[g]);
    }
  }
  if (slice->entry_flags != 0) {
    printf("  flags 0x%x\n", slice->entry_flags);
  }

  ud_slice_free(slice);
  return 0;
}
