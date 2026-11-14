# Benign created instruction data. Never executed.
.intel_syntax noprefix
.text
ldtilecfg [rip + 0]
sttilecfg [rax]
tilezero tmm1
tileloadd tmm2, [rax + rcx]
tileloaddt1 tmm3, [r8 + r9 + 32]
tiledpbssd_placeholder:
tdpbssd tmm0, tmm1, tmm2
tilestored [rdx + rsi + 16], tmm0
tilerelease
