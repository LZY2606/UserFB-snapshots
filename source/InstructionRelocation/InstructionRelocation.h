#include "dobby/dobby_internal.h"

// Relocate the instructions starting at `buffer`.
//
// `buffer` points at readable instruction bytes; it is the only place
// instructions are fetched from and may live anywhere in the host process.
// `origin->addr()` is the virtual address those instructions occupy in the
// image and is used only to compute PC-relative targets (ADR/ADRP, direct
// branches, literal loads) and to place branch-stub data. The two addresses do
// not need to be related, and `origin->addr()` is never dereferenced.
//
// On return `origin->size` is the number of whole instruction bytes consumed.
// If `branch` is true, a tail branch back to the next origin instruction is
// appended.
void GenRelocateCode(void *buffer, CodeMemBlock *origin, CodeMemBlock *relocated, bool branch);

void GenRelocateCodeAndBranch(void *buffer, CodeMemBlock *origin, CodeMemBlock *relocated);
