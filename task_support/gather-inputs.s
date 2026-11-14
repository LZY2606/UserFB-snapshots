.intel_syntax noprefix
.text
.global gather_inputs
gather_inputs:
vgatherdps xmm1,[rax+xmm4*4+16],xmm2
vgatherdps ymm1,[rax+ymm4*4+16],ymm2
vgatherdpd xmm1,[rax+xmm4*4+16],xmm2
vgatherdpd ymm1,[rax+xmm4*4+16],ymm2
vgatherqps xmm1,[rax+xmm4*4+16],xmm2
vgatherqps xmm1,[rax+ymm4*4+16],xmm2
vgatherqpd xmm1,[rax+xmm4*4+16],xmm2
vgatherqpd ymm1,[rax+ymm4*4+16],ymm2
vpgatherdd xmm1,[rax+xmm4*4+16],xmm2
vpgatherdd ymm1,[rax+ymm4*4+16],ymm2
vpgatherdq xmm1,[rax+xmm4*4+16],xmm2
vpgatherdq ymm1,[rax+xmm4*4+16],ymm2
vpgatherqd xmm1,[rax+xmm4*4+16],xmm2
vpgatherqd xmm1,[rax+ymm4*4+16],xmm2
vpgatherqq xmm1,[rax+xmm4*4+16],xmm2
vpgatherqq ymm1,[rax+ymm4*4+16],ymm2
vgatherdpd ymm3,[r13+xmm12*8-32],ymm5
vgatherqps xmm6,[rbx+ymm7*2],xmm8
vgatherqpd ymm9,[ymm10*4+0x11223344],ymm11
vpgatherdd xmm0,[ecx+xmm4*2+16],xmm2
