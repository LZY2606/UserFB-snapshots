.intel_syntax noprefix
.text
.globl fixture_start
fixture_start:
.globl mask_move
mask_move: kmovw k1,k2
.globl mask_and
mask_and: kandw k3,k1,k2
.globl mask_not
mask_not: knotw k4,k3
.globl mask_shift
mask_shift: kshiftlw k5,k4,3
.globl compress_reg
compress_reg: vpcompressd zmm3{k1},zmm2
.globl compress_narrow
compress_narrow: vpcompressd ymm4{k2}{z},ymm4
.globl expand_reg
expand_reg: vpexpandd zmm5{k3}{z},zmm3
.globl compress_mem
compress_mem: vpcompressd [rip+0xfd2]{k1},zmm2
.globl expand_mem
expand_mem: vpexpandd zmm6{k2},[rip+0xfcc]
.globl move_high
move_high: vmovdqu32 xmm31{k1}{z},xmm17
.globl fixture_end
fixture_end:
