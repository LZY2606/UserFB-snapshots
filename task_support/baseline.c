#include "fadec.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x) do { if (!(x)) {fprintf(stderr,"baseline:%d %s\n",__LINE__,#x);return 1;} } while(0)
static int one(const unsigned char *p, size_t n, FdInstrType type, FdInstr *d) {
    return fd_decode(p,n,64,0,d)==(int)n && FD_TYPE(d)==type;
}
int main(void) {
    FdInstr d;
    CHECK(one((unsigned char[]){0x39,0xc8},2,FDI_CMP,&d));
    CHECK(FD_OP_REG(&d,0)==FD_REG_AX && FD_OP_REG(&d,1)==FD_REG_CX && FD_OP_SIZE(&d,0)==4);
    CHECK(one((unsigned char[]){0x88,0xc4},2,FDI_MOV,&d));
    CHECK(FD_OP_REG_TYPE(&d,0)==FD_RT_GPH && FD_OP_REG(&d,0)==FD_REG_AH);
    CHECK(one((unsigned char[]){0x40,0x88,0xc4},3,FDI_MOV,&d));
    CHECK(FD_OP_REG_TYPE(&d,0)==FD_RT_GPL && FD_OP_REG(&d,0)==FD_REG_SP);
    CHECK(one((unsigned char[]){0x0f,0xb6,0xcc},3,FDI_MOVZX,&d));
    CHECK(FD_OP_REG_TYPE(&d,1)==FD_RT_GPH && FD_OP_SIZE(&d,0)==4);
    CHECK(one((unsigned char[]){0xfe,0xc0},2,FDI_INC,&d));
    CHECK(one((unsigned char[]){0x48,0x11,0xda},3,FDI_ADC,&d));
    CHECK(FD_OP_SIZE(&d,0)==8 && FD_OP_REG(&d,0)==FD_REG_DX && FD_OP_REG(&d,1)==FD_REG_BX);
    CHECK(one((unsigned char[]){0x4d,0x19,0xc8},3,FDI_SBB,&d));
    CHECK(FD_OP_REG(&d,0)==FD_REG_R8 && FD_OP_REG(&d,1)==FD_REG_R9);
    CHECK(one((unsigned char[]){0x0f,0x44,0xc1},3,FDI_CMOVZ,&d));
    CHECK(FD_OP_SIZE(&d,0)==4);
    CHECK(one((unsigned char[]){0x0f,0x95,0xc4},3,FDI_SETNZ,&d));
    CHECK(FD_OP_REG_TYPE(&d,0)==FD_RT_GPH);
    CHECK(one((unsigned char[]){0x9f},1,FDI_LAHF,&d));
    CHECK(one((unsigned char[]){0x9e},1,FDI_SAHF,&d));
    CHECK(one((unsigned char[]){0x83,0xf8,0xff},3,FDI_CMP,&d));
    CHECK(FD_OP_IMM(&d,1)==-1);
    CHECK(one((unsigned char[]){0x75,0x01},2,FDI_JNZ,&d));
    CHECK(FD_OP_TYPE(&d,0)==FD_OT_OFF && FD_OP_IMM(&d,0)==1);
    CHECK(one((unsigned char[]){0xc3},1,FDI_RET,&d));
    CHECK(one((unsigned char[]){0xb3,0x01},2,FDI_MOVABS,&d));
    CHECK(FD_OP_REG(&d,0)==FD_REG_BX && FD_OP_IMM(&d,1)==1);
    puts("EXISTING_DECODE_OK");
    return 0;
}
