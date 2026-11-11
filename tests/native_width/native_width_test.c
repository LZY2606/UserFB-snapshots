/*
 * Copyright (c) 2025 Bitdefender
 * SPDX-License-Identifier: Apache-2.0
 *
 * Native regression tests tying bddisasm decode context (NdDecodeEx) to bdshemu
 * emulation state (ShemuEmulate) for the same hand-assembled byte streams.
 *
 * The test never executes the input bytes on the real CPU: every byte is only
 * fed to NdDecodeEx and to ShemuEmulate. All external memory is served by a
 * controlled AccessMemory callback that records address, width, direction and
 * order of every access.
 *
 * Expected values are derived directly from the byte encodings and the x86
 * architecture rules (documented in README.md); a second decoder call is never
 * used to generate expectations.
 */
#include <stdio.h>
#include <string.h>

#include "bddisasm.h"
#include "bdshemu.h"

#define SHELL_BASE      0x1000ULL
#define STACK_BASE      0x8000ULL
#define STACK_SIZE      8192U
#define INTBUF_SIZE     16384U

/* Stable names of the three behavior assertions guarded by the mutation tests. */
#define BEH_32BIT_ZERO_EXTEND    "BEH_32BIT_ZERO_EXTEND"
#define BEH_HIGH8_SELECTION      "BEH_HIGH8_SELECTION"
#define BEH_ADDRSIZE_OVERRIDE    "BEH_ADDRSIZE_OVERRIDE"

static int gFailures = 0;

#define CHECK(cond, msg, ...)                                                            \
    do {                                                                                  \
        if (!(cond))                                                                      \
        {                                                                                 \
            gFailures++;                                                                  \
            printf("    FAIL: " msg "\n", ##__VA_ARGS__);                                 \
        }                                                                                 \
    } while (0)

#define BCHECK(beh, cond, msg, ...)                                                       \
    do {                                                                                  \
        if (!(cond))                                                                      \
        {                                                                                 \
            gFailures++;                                                                  \
            printf("    BEHAVIOR_FAIL %s: " msg "\n", beh, ##__VA_ARGS__);                \
        }                                                                                 \
    } while (0)


/*
 * Controlled external memory. Every callback invocation is recorded; one
 * specific access can be rejected to exercise clean emulation aborts.
 */
#define MEM_LOG_MAX     32

typedef struct _MEM_REC
{
    ND_UINT64       Addr;
    ND_UINT32       Size;
    ND_BOOL         Store;
    ND_UINT32       Seq;
    ND_UINT8        Data[8];        /* Store payload (undefined for loads). */
} MEM_REC;

typedef struct _MEM_LOG
{
    MEM_REC         Rec[MEM_LOG_MAX];
    ND_UINT32       Count;
    ND_UINT32       RejectSeq;      /* Access with this sequence returns ND_FALSE. */
} MEM_LOG;

static ND_BOOL
AccessMemoryCb(
    void *ShemuContext,
    ND_UINT64 Gla,
    ND_SIZET Size,
    ND_UINT8 *Buffer,
    ND_BOOL Store
    )
{
    SHEMU_CONTEXT *ctx = (SHEMU_CONTEXT *)ShemuContext;
    MEM_LOG *log = (MEM_LOG *)ctx->AuxData;
    ND_UINT32 seq = log->Count;

    if (seq < MEM_LOG_MAX)
    {
        log->Rec[seq].Addr = Gla;
        log->Rec[seq].Size = (ND_UINT32)Size;
        log->Rec[seq].Store = Store;
        log->Rec[seq].Seq = seq;
        memset(log->Rec[seq].Data, 0, sizeof(log->Rec[seq].Data));
        if (Store)
        {
            memcpy(log->Rec[seq].Data, Buffer, Size <= 8 ? (size_t)Size : 8U);
        }
        log->Count++;
    }

    if (log->RejectSeq == seq)
    {
        return ND_FALSE;
    }

    if (!Store)
    {
        /* Reads start from a zero-filled buffer; deterministic content is then
         * layered on for naturally-sized accesses. */
        memset(Buffer, 0, (size_t)Size);

        if (Size == 4)
        {
            ND_UINT32 v = (ND_UINT32)(Gla * 2654435761U) ^ 0x9E3779B9U;
            memcpy(Buffer, &v, 4);
        }
        else if (Size == 8)
        {
            ND_UINT64 v = (Gla * 0x9E3779B97F4A7C15ULL) ^ 0xDEADBEEFCAFE1234ULL;
            memcpy(Buffer, &v, 8);
        }
    }

    return ND_TRUE;
}

static ND_UINT32
ExpectedDword(
    ND_UINT64 Gla
    )
{
    return (ND_UINT32)(Gla * 2654435761U) ^ 0x9E3779B9U;
}


/*
 * Emulation context construction. Every run gets fresh, separately allocated
 * buffers; nothing mutable is shared between runs.
 */
typedef struct _RUN_INPUT
{
    ND_UINT64       Rax, Rbx, Rcx, Rdx, Rsi, Rdi, Rsp, Rbp, R8, Flags;
    ND_UINT64       FsBase, GsBase;
    ND_UINT64       MaxInstructions;
} RUN_INPUT;

static void
BuildContext(
    SHEMU_CONTEXT *Ctx,
    ND_UINT8 *Code,
    ND_UINT64 CodeSize,
    ND_UINT8 *Stack,
    ND_UINT8 *Intbuf,
    MEM_LOG *Log,
    const RUN_INPUT *Input
    )
{
    memset(Ctx, 0, sizeof(*Ctx));
    memset(Log, 0, sizeof(*Log));
    Log->RejectSeq = 0xFFFFFFFFU;

    Ctx->ArchType = SHEMU_ARCH_TYPE_X86;
    Ctx->Arch.X86.Mode = ND_CODE_64;
    Ctx->Arch.X86.Ring = 3;
    Ctx->Arch.X86.Registers.RegRip = SHELL_BASE;
    Ctx->Arch.X86.Registers.RegRax = Input->Rax;
    Ctx->Arch.X86.Registers.RegRbx = Input->Rbx;
    Ctx->Arch.X86.Registers.RegRcx = Input->Rcx;
    Ctx->Arch.X86.Registers.RegRdx = Input->Rdx;
    Ctx->Arch.X86.Registers.RegRsi = Input->Rsi;
    Ctx->Arch.X86.Registers.RegRdi = Input->Rdi;
    Ctx->Arch.X86.Registers.RegRsp = Input->Rsp;
    Ctx->Arch.X86.Registers.RegRbp = Input->Rbp;
    Ctx->Arch.X86.Registers.RegR8 = Input->R8;
    Ctx->Arch.X86.Registers.RegFlags = Input->Flags;
    Ctx->Arch.X86.Segments.Fs.Base = Input->FsBase;
    Ctx->Arch.X86.Segments.Gs.Base = Input->GsBase;

    Ctx->Shellcode = Code;
    Ctx->ShellcodeBase = SHELL_BASE;
    Ctx->ShellcodeSize = CodeSize;
    Ctx->Stack = Stack;
    Ctx->StackBase = STACK_BASE;
    Ctx->StackSize = STACK_SIZE;
    Ctx->Intbuf = Intbuf;
    Ctx->IntbufSize = INTBUF_SIZE;
    Ctx->MaxInstructionsCount = Input->MaxInstructions;

    Ctx->MemThreshold = 64;
    Ctx->AccessMemory = AccessMemoryCb;
    Ctx->AuxData = Log;
}

/*
 * Decode every instruction in a byte stream with NdDecodeEx, the same public
 * entry point the requirement pins down. Returns the number of instructions
 * and verifies that the encoded lengths cover the whole stream.
 */
static unsigned
DecodeAll(
    const ND_UINT8 *Code,
    ND_UINT64 CodeSize,
    INSTRUX *Out,
    unsigned MaxOut
    )
{
    unsigned n = 0;
    ND_UINT64 off = 0;

    while (off < CodeSize && n < MaxOut)
    {
        NDSTATUS st = NdDecodeEx(&Out[n], Code + off, (ND_SIZET)(CodeSize - off),
                                 ND_CODE_64, ND_CODE_64);
        if (!ND_SUCCESS(st))
        {
            printf("    FAIL: NdDecodeEx at offset %llu returned 0x%x\n",
                   (unsigned long long)off, st);
            gFailures++;
            return n;
        }
        off += Out[n].Length;
        n++;
    }

    if (off != CodeSize)
    {
        printf("    FAIL: decoded lengths sum to %llu, stream is %llu bytes\n",
               (unsigned long long)off, (unsigned long long)CodeSize);
        gFailures++;
    }

    return n;
}

static const ND_OPDESC_REGISTER *
FindRegOperand(
    const INSTRUX *Ix,
    ND_UINT32 Reg
    )
{
    for (ND_UINT32 i = 0; i < Ix->OperandsCount; i++)
    {
        const ND_OPERAND *op = &Ix->Operands[i];
        if (op->Type == ND_OP_REG && op->Info.Register.Type == ND_REG_GPR &&
            op->Info.Register.Reg == Reg)
        {
            return &op->Info.Register;
        }
    }

    return ND_NULL;
}

static const ND_OPDESC_MEMORY *
FindMemOperand(
    const INSTRUX *Ix
    )
{
    for (ND_UINT32 i = 0; i < Ix->OperandsCount; i++)
    {
        if (Ix->Operands[i].Type == ND_OP_MEM)
        {
            return &Ix->Operands[i].Info.Memory;
        }
    }

    return ND_NULL;
}


/*
 * Scenario 1: a 32-bit register write in 64-bit mode zeroes the upper 32 bits.
 *
 *   48 B8 88 77 66 55 44 33 22 11   mov rax, 0x1122334455667788
 *   B8 DD CC BB AA                   mov eax, 0xAABBCCDD
 */
static void
Scenario32BitZeroExtends(
    void
    )
{
    static const ND_UINT8 code[] = {
        0x48,0xB8,0x88,0x77,0x66,0x55,0x44,0x33,0x22,0x11,
        0xB8,0xDD,0xCC,0xBB,0xAA
    };
    const ND_UINT64 sentinel = 0x1122334455667788ULL;
    INSTRUX ix[4];
    SHEMU_CONTEXT ctx;
    ND_UINT8 stack[STACK_SIZE], intbuf[INTBUF_SIZE];
    MEM_LOG log;
    RUN_INPUT in = { 0 };
    unsigned n;
    int before = gFailures;

    in.MaxInstructions = 2;
    BuildContext(&ctx, (ND_UINT8 *)code, sizeof(code), stack, intbuf, &log, &in);
    SHEMU_STATUS st = ShemuEmulate(&ctx);

    n = DecodeAll(code, sizeof(code), ix, 4);
    CHECK(n == 2, "expected 2 instructions, got %u", n);
    if (n == 2)
    {
        const ND_OPDESC_REGISTER *r64 = FindRegOperand(&ix[0], NDR_RAX);
        const ND_OPDESC_REGISTER *r32 = FindRegOperand(&ix[1], NDR_EAX);
        CHECK(ix[0].Instruction == ND_INS_MOV && ix[1].Instruction == ND_INS_MOV,
              "expected two MOV instructions");
        CHECK(r64 != ND_NULL && r64->Size == 8 && r64->IsHigh8 == 0,
              "decoded mov rax operand must be an 8-byte GPR");
        CHECK(r32 != ND_NULL && r32->Size == 4 && r32->IsHigh8 == 0 && ix[1].HasRex == 0,
              "decoded mov eax operand must be a 4-byte GPR without REX");
        CHECK(ix[1].Operands[1].Info.Immediate.Imm == 0xAABBCCDDU,
              "decoded immediate must be 0xAABBCCDD");
    }

    CHECK(st == SHEMU_SUCCESS, "emulation failed with status %u", st);
    /* The loop counter is post-incremented at every iteration, so a clean run
     * that retires exactly MaxInstructionsCount instructions ends one higher. */
    CHECK(ctx.InstructionsCount == in.MaxInstructions + 1,
          "expected %llu retired iterations (counter ends at retired+1), got %llu",
          (unsigned long long)in.MaxInstructions,
          (unsigned long long)ctx.InstructionsCount);
    CHECK((ctx.Arch.X86.Registers.RegRax & 0xFFFFFFFFULL) == 0xAABBCCDDU,
          "low dword must equal the written immediate, got 0x%llx",
          (unsigned long long)ctx.Arch.X86.Registers.RegRax);
    BCHECK(BEH_32BIT_ZERO_EXTEND,
           ctx.Arch.X86.Registers.RegRax == 0x00000000AABBCCDDULL,
           "32-bit write must zero the high dword (sentinel was 0x%llx), got 0x%llx",
           (unsigned long long)sentinel,
           (unsigned long long)ctx.Arch.X86.Registers.RegRax);
    CHECK(log.Count == 0, "register-only scenario must not touch external memory");

    if (gFailures == before) printf("PASS width_32bit_clears_high\n");
}


/*
 * Scenario 2: 16-bit and low-8-bit writes preserve every other bit.
 *
 *   48 B8 88 77 66 55 44 33 22 11   mov rax, 0x1122334455667788
 *   66 B8 34 12                      mov ax, 0x1234
 *   B0 EE                            mov al, 0xEE
 */
static void
ScenarioPartialWritesPreserve(
    void
    )
{
    static const ND_UINT8 code[] = {
        0x48,0xB8,0x88,0x77,0x66,0x55,0x44,0x33,0x22,0x11,
        0x66,0xB8,0x34,0x12,
        0xB0,0xEE
    };
    INSTRUX ix[4];
    SHEMU_CONTEXT ctx;
    ND_UINT8 stack[STACK_SIZE], intbuf[INTBUF_SIZE];
    MEM_LOG log;
    RUN_INPUT in = { 0 };
    unsigned n;
    int before = gFailures;

    in.MaxInstructions = 3;
    BuildContext(&ctx, (ND_UINT8 *)code, sizeof(code), stack, intbuf, &log, &in);
    SHEMU_STATUS st = ShemuEmulate(&ctx);

    n = DecodeAll(code, sizeof(code), ix, 4);
    CHECK(n == 3, "expected 3 instructions, got %u", n);
    if (n == 3)
    {
        const ND_OPDESC_REGISTER *r16 = FindRegOperand(&ix[1], NDR_AX);
        const ND_OPDESC_REGISTER *r8 = FindRegOperand(&ix[2], NDR_AL);
        CHECK(ix[1].HasOpSize == 1, "mov ax must carry the 0x66 operand-size prefix");
        CHECK(r16 != ND_NULL && r16->Size == 2 && r16->IsHigh8 == 0,
              "decoded mov ax must be a 2-byte GPR");
        CHECK(r8 != ND_NULL && r8->Size == 1 && r8->IsHigh8 == 0,
              "decoded mov al must be a 1-byte low GPR");
    }

    CHECK(st == SHEMU_SUCCESS, "emulation failed with status %u", st);
    /* 0x1122334455667788 -> ax: 0x1122334455661234 -> al: 0x11223344556612EE. */
    CHECK(ctx.Arch.X86.Registers.RegRax == 0x11223344556612EEULL,
          "16/8-bit writes must preserve untouched bits, got 0x%llx",
          (unsigned long long)ctx.Arch.X86.Registers.RegRax);
    CHECK((ctx.Arch.X86.Registers.RegRax >> 16) == 0x112233445566ULL,
          "high 6 bytes must be untouched");
    CHECK(log.Count == 0, "register-only scenario must not touch external memory");

    if (gFailures == before) printf("PASS width_16bit_8bit_preserve\n");
}


/*
 * Scenario 3: AH (no REX) and SPL (REX) must not be confused.
 *
 *   B4 99        mov ah, 0x99
 *   40 B4 77     mov spl, 0x77
 *
 * Both encodings use ModRM-free opcode 0xB4 with low id 4; only REX decides
 * between high-byte AH and low-byte SPL.
 */
static void
ScenarioHigh8Selection(
    void
    )
{
    static const ND_UINT8 code[] = {
        0xB4,0x99,
        0x40,0xB4,0x77
    };
    const ND_UINT64 raxInit = 0xAABBCCDD11223344ULL;
    const ND_UINT64 rspInit = 0x5566778899AABBCCULL;
    INSTRUX ix[2];
    SHEMU_CONTEXT ctx;
    ND_UINT8 stack[STACK_SIZE], intbuf[INTBUF_SIZE];
    MEM_LOG log;
    RUN_INPUT in = { 0 };
    unsigned n;
    int before = gFailures;

    in.Rax = raxInit;
    in.Rsp = rspInit;
    in.MaxInstructions = 2;
    BuildContext(&ctx, (ND_UINT8 *)code, sizeof(code), stack, intbuf, &log, &in);
    SHEMU_STATUS st = ShemuEmulate(&ctx);

    n = DecodeAll(code, sizeof(code), ix, 2);
    CHECK(n == 2, "expected 2 instructions, got %u", n);
    if (n == 2)
    {
        const ND_OPDESC_REGISTER *ah = FindRegOperand(&ix[0], NDR_AH);
        const ND_OPDESC_REGISTER *spl = FindRegOperand(&ix[1], NDR_SPL);
        CHECK(ix[0].HasRex == 0, "mov ah must have no REX prefix");
        CHECK(ah != ND_NULL && ah->Size == 1 && ah->IsHigh8 == 1,
              "0xB4 without REX must decode as the high-byte AH register");
        CHECK(ix[1].HasRex == 1 && ix[1].Rex.Rex == 0x40,
              "mov spl must carry REX 0x40");
        CHECK(spl != ND_NULL && spl->Size == 1 && spl->IsHigh8 == 0,
              "0xB4 with REX must decode as the low-byte SPL register");
    }

    CHECK(st == SHEMU_SUCCESS, "emulation failed with status %u", st);
    BCHECK(BEH_HIGH8_SELECTION,
           ctx.Arch.X86.Registers.RegRax == 0xAABBCCDD11229944ULL,
           "AH write must touch only byte 1 of RAX, got 0x%llx",
           (unsigned long long)ctx.Arch.X86.Registers.RegRax);
    BCHECK(BEH_HIGH8_SELECTION,
           ctx.Arch.X86.Registers.RegRsp == 0x5566778899AABB77ULL,
           "SPL write must touch only byte 0 of RSP, got 0x%llx",
           (unsigned long long)ctx.Arch.X86.Registers.RegRsp);
    CHECK(log.Count == 0, "register-only scenario must not touch external memory");

    if (gFailures == before) printf("PASS high8_ah_vs_spl\n");
}


/*
 * Scenario 4: ADC/SBB must observe the incoming carry, and CMP must drive a
 * conditional jump. Data includes zero results, high-bit sentinels and a
 * cross-byte carry out of 0xFFFFFFFF.
 *
 *   83 D0 01      adc eax, 1
 *   83 DB 00      sbb ebx, 0
 */
static void
ScenarioAdcSbbCarry(
    void
    )
{
    static const ND_UINT8 adcSbb[] = { 0x83,0xD0,0x01, 0x83,0xDB,0x00 };
    static const ND_UINT8 clcSbb[] = { 0xF8, 0x83,0xDB,0x00 };
    static const ND_UINT8 stcSbb[] = { 0xF9, 0x83,0xDB,0x00 };
    INSTRUX ix[4];
    SHEMU_CONTEXT ctx;
    ND_UINT8 stack[STACK_SIZE], intbuf[INTBUF_SIZE];
    MEM_LOG log;
    RUN_INPUT in = { 0 };
    unsigned n;
    int before = gFailures;

    n = DecodeAll(adcSbb, sizeof(adcSbb), ix, 4);
    CHECK(n == 2, "expected 2 instructions, got %u", n);
    if (n == 2)
    {
        CHECK(ix[0].Instruction == ND_INS_ADC && ix[1].Instruction == ND_INS_SBB,
              "expected ADC then SBB");
        CHECK((ix[0].FlagsAccess.Modified.Raw & NDR_RFLAG_CF) != 0 &&
              (ix[1].FlagsAccess.Modified.Raw & NDR_RFLAG_CF) != 0,
              "decoder must report CF as modified by ADC/SBB");
    }

    /* Run A: CF clear on entry. 0xFFFFFFFF + 1 + 0 carries across all 4 bytes
     * and truncates to zero; CF is then set, so SBB borrows. */
    memset(&in, 0, sizeof(in));
    in.Rax = 0xFFFFFFFFFFFFFFFFULL;
    in.Rbx = 0x7777777777777777ULL;
    in.Flags = 0;
    in.MaxInstructions = 2;
    BuildContext(&ctx, (ND_UINT8 *)adcSbb, sizeof(adcSbb), stack, intbuf, &log, &in);
    SHEMU_STATUS stA = ShemuEmulate(&ctx);
    CHECK(stA == SHEMU_SUCCESS, "run A emulation failed: %u", stA);
    CHECK(ctx.Arch.X86.Registers.RegRax == 0,
          "ADC without incoming carry must wrap 0xFFFFFFFF to 0, got 0x%llx",
          (unsigned long long)ctx.Arch.X86.Registers.RegRax);
    CHECK(ctx.Arch.X86.Registers.RegRbx == 0x0000000077777776ULL,
          "SBB must consume the carry produced by ADC, got 0x%llx",
          (unsigned long long)ctx.Arch.X86.Registers.RegRbx);
    CHECK((ctx.Arch.X86.Registers.RegFlags & NDR_RFLAG_CF) == 0,
          "SBB 0x77777777-1 must clear CF");
    /* ZF reflects the final SBB result (non-zero); the zero ADC result is
     * asserted directly through RAX == 0 above rather than through ZF, since
     * SBB legitimately overwrites the arithmetic flags afterwards. */
    CHECK((ctx.Arch.X86.Registers.RegFlags & NDR_RFLAG_ZF) == 0,
          "non-zero SBB result must clear ZF");

    /* Run B: CF set on entry. Same low dword, but the extra carry yields 1. */
    memset(&in, 0, sizeof(in));
    in.Rax = 0xFFFFFFFFFFFFFFFFULL;
    in.Rbx = 0x7777777777777777ULL;
    in.Flags = NDR_RFLAG_CF;
    in.MaxInstructions = 2;
    BuildContext(&ctx, (ND_UINT8 *)adcSbb, sizeof(adcSbb), stack, intbuf, &log, &in);
    SHEMU_STATUS stB = ShemuEmulate(&ctx);
    CHECK(stB == SHEMU_SUCCESS, "run B emulation failed: %u", stB);
    CHECK(ctx.Arch.X86.Registers.RegRax == 1,
          "ADC with incoming carry must yield 1, got 0x%llx",
          (unsigned long long)ctx.Arch.X86.Registers.RegRax);
    CHECK((ctx.Arch.X86.Registers.RegFlags & NDR_RFLAG_ZF) == 0,
          "non-zero ADC result must clear ZF");

    /* SBB in isolation, no-borrow path. */
    memset(&in, 0, sizeof(in));
    in.Rbx = 0x7777777777777777ULL;
    in.Flags = NDR_RFLAG_CF;             /* CLC clears it inside the snippet. */
    in.MaxInstructions = 2;
    BuildContext(&ctx, (ND_UINT8 *)clcSbb, sizeof(clcSbb), stack, intbuf, &log, &in);
    SHEMU_STATUS stC = ShemuEmulate(&ctx);
    CHECK(stC == SHEMU_SUCCESS, "CLC/SBB run failed: %u", stC);
    CHECK(ctx.Arch.X86.Registers.RegRbx == 0x0000000077777777ULL,
          "SBB without borrow must preserve EBX, got 0x%llx",
          (unsigned long long)ctx.Arch.X86.Registers.RegRbx);

    /* SBB in isolation, borrow path. */
    memset(&in, 0, sizeof(in));
    in.Rbx = 0x7777777777777777ULL;
    in.Flags = 0;                        /* STC sets it inside the snippet. */
    in.MaxInstructions = 2;
    BuildContext(&ctx, (ND_UINT8 *)stcSbb, sizeof(stcSbb), stack, intbuf, &log, &in);
    SHEMU_STATUS stD = ShemuEmulate(&ctx);
    CHECK(stD == SHEMU_SUCCESS, "STC/SBB run failed: %u", stD);
    CHECK(ctx.Arch.X86.Registers.RegRbx == 0x0000000077777776ULL,
          "SBB with borrow must subtract one, got 0x%llx",
          (unsigned long long)ctx.Arch.X86.Registers.RegRbx);

    if (gFailures == before) printf("PASS adc_sbb_carry_paths\n");
}


/*
 * Scenario 5: CMP flags select whether JE is taken; the two paths leave
 * observably different final state.
 *
 *   off 0  39 D1              cmp ecx, edx
 *   off 2  74 07              je  +7            (target RIP = 0x100B, offset 11)
 *   off 4  B8 01 00 00 00     mov eax, 1        ; not-taken path
 *   off 9  EB 02              jmp +2            (target RIP = 0x100D, stream end)
 *   off 11 66 90              nop               ; 2-byte NOP, taken target
 *
 * The taken target is a real 2-byte NOP so that both paths reach the same
 * final RIP just past the stream, where the loop budget stops emulation.
 */
static void
ScenarioCmpConditionalBranch(
    void
    )
{
    static const ND_UINT8 code[] = {
        0x39,0xD1,
        0x74,0x07,
        0xB8,0x01,0x00,0x00,0x00,
        0xEB,0x02,
        0x66,0x90
    };
    INSTRUX ix[8];
    SHEMU_CONTEXT ctx;
    ND_UINT8 stack[STACK_SIZE], intbuf[INTBUF_SIZE];
    MEM_LOG log;
    RUN_INPUT in = { 0 };
    unsigned n;
    int before = gFailures;

    n = DecodeAll(code, sizeof(code), ix, 8);
    CHECK(n == 5, "expected 5 instructions, got %u", n);
    if (n == 5)
    {
        CHECK(ix[0].Instruction == ND_INS_CMP, "first instruction must be CMP");
        CHECK(ix[1].Instruction == ND_INS_Jcc &&
              ix[1].BranchInfo.IsBranch == 1 &&
              ix[1].BranchInfo.IsConditional == 1 &&
              ix[1].Condition == ND_COND_EQUAL,
              "second instruction must be JE (conditional on ZF)");
        CHECK(ix[1].HasRelOffs == 1 &&
              ix[1].Operands[0].Info.RelativeOffset.Rel == 7,
              "JE must carry rel8 == 7, landing on the NOP at offset 11");
        CHECK(ix[3].Instruction == ND_INS_JMPNR &&
              ix[3].BranchInfo.IsConditional == 0,
              "fourth instruction must be an unconditional JMP");
        CHECK(ix[4].Instruction == ND_INS_NOP && ix[4].Length == 2,
              "taken target must be the 2-byte NOP at offset 11");
    }

    /* Equal operands: ZF set, JE taken, MOV EAX,1 is skipped. Three
     * instructions retire (CMP, JE, NOP); the loop counter ends one higher. */
    memset(&in, 0, sizeof(in));
    in.Rax = 0xDEADBEEFCAFEBABEULL;
    in.Rcx = 0x11223344ULL;
    in.Rdx = 0x11223344ULL;
    in.MaxInstructions = 3;
    BuildContext(&ctx, (ND_UINT8 *)code, sizeof(code), stack, intbuf, &log, &in);
    SHEMU_STATUS stT = ShemuEmulate(&ctx);
    CHECK(stT == SHEMU_SUCCESS,
          "run must stop when the instruction budget is exhausted at stream end, got %u", stT);
    CHECK(ctx.InstructionsCount == 4, "taken path retires 3 instructions, counter=%llu",
          (unsigned long long)ctx.InstructionsCount);
    CHECK(ctx.Arch.X86.Registers.RegRax == 0xDEADBEEFCAFEBABEULL,
          "taken JE must skip the MOV that would overwrite RAX, got 0x%llx",
          (unsigned long long)ctx.Arch.X86.Registers.RegRax);
    CHECK(ctx.Arch.X86.Registers.RegRip == SHELL_BASE + sizeof(code),
          "taken path must end at the first byte after the stream");

    /* Differing operands: ZF clear, JE not taken, MOV EAX,1 executes (and zero
     * extends), then the unconditional JMP merges both paths at the end. Four
     * instructions retire; the loop counter again ends one higher. */
    memset(&in, 0, sizeof(in));
    in.Rax = 0xDEADBEEFCAFEBABEULL;
    in.Rcx = 0x11223344ULL;
    in.Rdx = 0x11223345ULL;
    in.MaxInstructions = 4;
    BuildContext(&ctx, (ND_UINT8 *)code, sizeof(code), stack, intbuf, &log, &in);
    SHEMU_STATUS stN = ShemuEmulate(&ctx);
    CHECK(stN == SHEMU_SUCCESS,
          "run must stop when the instruction budget is exhausted at stream end, got %u", stN);
    CHECK(ctx.InstructionsCount == 5, "not-taken path retires 4 instructions, counter=%llu",
          (unsigned long long)ctx.InstructionsCount);
    CHECK(ctx.Arch.X86.Registers.RegRax == 1,
          "not-taken JE must execute MOV EAX,1, got 0x%llx",
          (unsigned long long)ctx.Arch.X86.Registers.RegRax);
    CHECK(ctx.Arch.X86.Registers.RegRip == SHELL_BASE + sizeof(code),
          "both paths must merge at the same final RIP");

    if (gFailures == before) printf("PASS cmp_cond_branch\n");
}


/*
 * Scenario 6: 0x67 address-size override makes high-bit-nonzero base registers
 * participate as 32-bit in effective-address computation; FS/GS bases are added
 * afterwards, at the linear-address stage.
 *
 *   67 64 8B 87 00 20 00 00   mov eax, fs:[edi + 0x2000]
 *   67 65 8B 0E               mov ecx, gs:[esi]
 *   67 64 83 06 01            add dword fs:[esi], 1
 *
 * The first effective address overflows 32 bits before masking
 * (0xFFFFF000 + 0x2000 = 0x100001000), so the 32-bit truncation is directly
 * observable: a correct engine reaches 0x11000, an engine that skips the mask
 * reaches 0x100011000. In 32-bit addressing the ModRM encoding of ESI (id 6)
 * needs no SIB byte, so the RMW form stays a plain ModRM instruction.
 */
static void
ScenarioAddrSizeOverride(
    void
    )
{
    static const ND_UINT8 code[] = {
        0x67,0x64,0x8B,0x87,0x00,0x20,0x00,0x00,
        0x67,0x65,0x8B,0x0E,
        0x67,0x64,0x83,0x06,0x01
    };
    const ND_UINT64 rdiInit = 0x11112222FFFFF000ULL;
    const ND_UINT64 rsiInit = 0x5555666677778804ULL;
    const ND_UINT64 fsBase = 0x10000ULL;
    const ND_UINT64 gsBase = 0x200000ULL;
    const ND_UINT32 disp = 0x2000U;
    /* Raw 32-bit effective addresses, truncated before any segment base. */
    const ND_UINT64 ea1 = ((rdiInit & 0xFFFFFFFFULL) + disp) & 0xFFFFFFFFULL; /* 0x1000 */
    const ND_UINT64 ea2 = (rsiInit & 0xFFFFFFFFULL);                          /* 0x77778804 */
    /* Linear addresses: segment bases are added only after the truncation. */
    const ND_UINT64 gla1 = ea1 + fsBase;   /* 0x11000 */
    const ND_UINT64 gla2 = ea2 + gsBase;   /* 0x77978804 */
    const ND_UINT64 gla3 = ea2 + fsBase;   /* 0x77788804, RMW through FS */
    INSTRUX ix[4];
    SHEMU_CONTEXT ctx;
    ND_UINT8 stack[STACK_SIZE], intbuf[INTBUF_SIZE];
    MEM_LOG log;
    RUN_INPUT in = { 0 };
    unsigned n;
    int before = gFailures;

    in.Rdi = rdiInit;
    in.Rsi = rsiInit;
    in.FsBase = fsBase;
    in.GsBase = gsBase;
    in.MaxInstructions = 3;
    BuildContext(&ctx, (ND_UINT8 *)code, sizeof(code), stack, intbuf, &log, &in);
    SHEMU_STATUS st = ShemuEmulate(&ctx);

    n = DecodeAll(code, sizeof(code), ix, 4);
    CHECK(n == 3, "expected 3 instructions, got %u", n);
    if (n == 3)
    {
        for (unsigned i = 0; i < n; i++)
        {
            CHECK(ix[i].HasAddrSize == 1 && ix[i].AddrMode == ND_ADDR_32,
                  "instruction %u must decode with 32-bit address mode due to 0x67", i);
        }
        const ND_OPDESC_MEMORY *m0 = FindMemOperand(&ix[0]);
        const ND_OPDESC_MEMORY *m1 = FindMemOperand(&ix[1]);
        const ND_OPDESC_MEMORY *m2 = FindMemOperand(&ix[2]);
        CHECK(m0 != ND_NULL && m0->HasSeg == 1 && m0->Seg == NDR_FS &&
              m0->Base == NDR_EDI && m0->BaseSize == 4 &&
              m0->HasDisp == 1 && m0->Disp == disp && m0->DispSize == 4,
              "mov eax, fs:[edi+0x2000] must decode FS, a 4-byte EDI base and disp32");
        CHECK(m1 != ND_NULL && m1->HasSeg == 1 && m1->Seg == NDR_GS &&
              m1->Base == NDR_ESI && m1->BaseSize == 4,
              "mov ecx, gs:[esi] must decode GS segment and a 4-byte ESI base");
        CHECK(m2 != ND_NULL && m2->HasSeg == 1 && m2->Seg == NDR_FS &&
              m2->Base == NDR_ESI && m2->BaseSize == 4,
              "ADD must target fs:[esi] with a 4-byte ESI base");
        CHECK((ix[2].MemoryAccess & ND_ACCESS_READ) != 0 &&
              (ix[2].MemoryAccess & ND_ACCESS_WRITE) != 0,
              "ADD to memory must decode as read-write");
    }

    CHECK(st == SHEMU_SUCCESS, "emulation failed with status %u", st);
    BCHECK(BEH_ADDRSIZE_OVERRIDE,
           log.Count == 4 &&
           log.Rec[0].Addr == gla1 && log.Rec[0].Store == ND_FALSE && log.Rec[0].Size == 4 &&
           log.Rec[1].Addr == gla2 && log.Rec[1].Store == ND_FALSE && log.Rec[1].Size == 4 &&
           log.Rec[2].Addr == gla3 && log.Rec[2].Store == ND_FALSE && log.Rec[2].Size == 4 &&
           log.Rec[3].Addr == gla3 && log.Rec[3].Store == ND_TRUE  && log.Rec[3].Size == 4,
           "addresses must be 32-bit-truncated before FS/GS bases are added, "
           "got %u accesses starting with 0x%llx/0x%llx",
           log.Count,
           log.Count ? (unsigned long long)log.Rec[0].Addr : 0ULL,
           log.Count > 1 ? (unsigned long long)log.Rec[1].Addr : 0ULL);
    /* Without the 32-bit mask the first load would instead target this address. */
    BCHECK(BEH_ADDRSIZE_OVERRIDE,
           (((rdiInit & 0xFFFFFFFFULL) + disp) + fsBase) != gla1,
           "test invariant: the overflowing unmasked address must differ from the masked one");
    CHECK(ctx.Arch.X86.Registers.RegRax == ExpectedDword(gla1),
          "RAX must receive the callback dword at 0x%llx", (unsigned long long)gla1);
    CHECK(ctx.Arch.X86.Registers.RegRcx == ExpectedDword(gla2),
          "RCX must receive the callback dword at 0x%llx", (unsigned long long)gla2);
    CHECK((ctx.Arch.X86.Registers.RegRax & 0xFFFFFFFF00000000ULL) == 0,
          "32-bit load into EAX must zero-extend RAX");
    /* The RMW store payload must be exactly the read dword plus one. */
    {
        ND_UINT32 expectStore = ExpectedDword(gla3) + 1U;
        ND_UINT32 actualStore = 0;
        memcpy(&actualStore, log.Rec[3].Data, 4);
        CHECK(actualStore == expectStore,
              "write-back must be read value + 1: expected 0x%x, got 0x%x",
              expectStore, actualStore);
    }

    if (gFailures == before) printf("PASS addrsize_override_seg\n");
}


/*
 * Scenario 7: a read-modify-write instruction to external memory must retain
 * both accesses, in order, at the same address and width. The served dword ends
 * in 0xFF, so the increment also exercises a real cross-byte carry
 * (0x1DBB69FF -> 0x1DBB6A00).
 *
 *   FF 00              inc dword [rax]
 */
static void
ScenarioRmwTwoAccesses(
    void
    )
{
    static const ND_UINT8 code[] = { 0xFF,0x00 };
    const ND_UINT64 extAddr = 0x0000000000500026ULL;
    const ND_UINT32 servedValue = 0x1DBB69FFU;
    INSTRUX ix[2];
    SHEMU_CONTEXT ctx;
    ND_UINT8 stack[STACK_SIZE], intbuf[INTBUF_SIZE];
    MEM_LOG log;
    RUN_INPUT in = { 0 };
    unsigned n;
    int before = gFailures;

    in.Rax = extAddr;
    in.MaxInstructions = 1;
    BuildContext(&ctx, (ND_UINT8 *)code, sizeof(code), stack, intbuf, &log, &in);
    SHEMU_STATUS st = ShemuEmulate(&ctx);

    n = DecodeAll(code, sizeof(code), ix, 2);
    CHECK(n == 1, "expected 1 instruction, got %u", n);
    if (n == 1)
    {
        CHECK(ix[0].Instruction == ND_INS_INC, "expected INC");
        CHECK(ix[0].Operands[0].Type == ND_OP_MEM &&
              ix[0].Operands[0].Info.Memory.Base == NDR_RAX &&
              ix[0].Operands[0].Info.Memory.BaseSize == 8,
              "INC must address memory through 8-byte RAX");
        CHECK((ix[0].Operands[0].Access.Access & ND_ACCESS_READ) != 0 &&
              (ix[0].Operands[0].Access.Access & ND_ACCESS_WRITE) != 0,
              "decoded operand must be read-write");
    }

    CHECK(st == SHEMU_SUCCESS, "emulation failed with status %u", st);
    CHECK(log.Count == 2, "RMW must issue exactly two external accesses, got %u", log.Count);
    if (log.Count == 2)
    {
        ND_UINT32 stored = 0;
        CHECK(log.Rec[0].Store == ND_FALSE && log.Rec[1].Store == ND_TRUE,
              "access order must be load then store");
        CHECK(log.Rec[0].Addr == extAddr && log.Rec[1].Addr == extAddr,
              "both accesses must target the same address");
        CHECK(log.Rec[0].Size == 4 && log.Rec[1].Size == 4,
              "both accesses must be 4 bytes wide");
        CHECK(log.Rec[0].Seq == 0 && log.Rec[1].Seq == 1, "sequence numbers must be ordered");
        memcpy(&stored, log.Rec[1].Data, 4);
        CHECK(stored == servedValue + 1U,
              "INC write-back must carry the cross-byte increment (0x%x -> 0x%x), got 0x%x",
              servedValue, servedValue + 1U, stored);
        CHECK(ExpectedDword(extAddr) == servedValue,
              "test invariant: callback must actually serve 0x%x at this address",
              servedValue);
    }

    if (gFailures == before) printf("PASS rmw_read_modify_write_pair\n");
}


/*
 * Scenario 8: rejecting one access must surface SHEMU_ABORT_GLA_OUTSIDE,
 * record only the accesses that happened, and leave registers unmodified.
 * A crash is never an acceptable rejection.
 *
 *   8B 07                  mov eax, [rdi]
 *   B8 78 56 34 12         mov eax, 0x12345678
 */
static void
ScenarioRejectedAccess(
    void
    )
{
    static const ND_UINT8 code[] = {
        0x8B,0x07,
        0xB8,0x78,0x56,0x34,0x12
    };
    const ND_UINT64 extAddr = 0x0000000000600000ULL;
    const ND_UINT64 raxSentinel = 0x0F0F0F0F0F0F0F0FULL;
    INSTRUX ix[4];
    SHEMU_CONTEXT ctx;
    ND_UINT8 stack[STACK_SIZE], intbuf[INTBUF_SIZE];
    MEM_LOG log;
    RUN_INPUT in = { 0 };
    unsigned n;
    int before = gFailures;

    n = DecodeAll(code, sizeof(code), ix, 4);
    CHECK(n == 2, "expected 2 instructions, got %u", n);
    if (n == 2)
    {
        CHECK(ix[0].Operands[1].Type == ND_OP_MEM &&
              ix[0].Operands[1].Info.Memory.Base == NDR_RDI &&
              ix[0].Operands[1].Info.Memory.BaseSize == 8,
              "first instruction must read through 8-byte RDI");
    }

    /* Rejecting run. */
    memset(&in, 0, sizeof(in));
    in.Rax = raxSentinel;
    in.Rdi = extAddr;
    in.MaxInstructions = 1;
    BuildContext(&ctx, (ND_UINT8 *)code, sizeof(code), stack, intbuf, &log, &in);
    log.RejectSeq = 0;
    SHEMU_STATUS stR = ShemuEmulate(&ctx);
    CHECK(stR == SHEMU_ABORT_GLA_OUTSIDE,
          "rejected callback must yield SHEMU_ABORT_GLA_OUTSIDE, got %u", stR);
    CHECK(log.Count == 1, "only the rejected access may be recorded, got %u", log.Count);
    if (log.Count == 1)
    {
        CHECK(log.Rec[0].Addr == extAddr && log.Rec[0].Size == 4 &&
              log.Rec[0].Store == ND_FALSE,
              "the recorded access must be the 4-byte load at 0x%llx",
              (unsigned long long)extAddr);
    }
    CHECK(ctx.InstructionsCount == 1,
          "only the faulting iteration may run; the second MOV must never execute, count=%llu",
          (unsigned long long)ctx.InstructionsCount);
    CHECK(ctx.Arch.X86.Registers.RegRax == raxSentinel,
          "rejected load must leave RAX untouched (the faulting instruction never commits), "
          "got 0x%llx",
          (unsigned long long)ctx.Arch.X86.Registers.RegRax);
    /* RIP is advanced past the faulting instruction before the load executes,
     * but the instruction's register write is never committed (RAX unchanged). */
    CHECK(ctx.Arch.X86.Registers.RegRip == SHELL_BASE + ix[0].Length,
          "RIP must point just past the faulting 2-byte load, got 0x%llx",
          (unsigned long long)ctx.Arch.X86.Registers.RegRip);

    /* Control run: same bytes, callback accepts -> clean success. */
    memset(&in, 0, sizeof(in));
    in.Rax = raxSentinel;
    in.Rdi = extAddr;
    in.MaxInstructions = 2;
    BuildContext(&ctx, (ND_UINT8 *)code, sizeof(code), stack, intbuf, &log, &in);
    SHEMU_STATUS stO = ShemuEmulate(&ctx);
    CHECK(stO == SHEMU_SUCCESS, "accepting callback must succeed, got %u", stO);
    CHECK(log.Count == 1, "control run records only the single external load, got %u",
          log.Count);
    CHECK(ctx.InstructionsCount == 3 &&
          ctx.Arch.X86.Registers.RegRax == 0x0000000012345678ULL,
          "control run must execute both MOVs and zero-extend EAX (counter ends at 3)");

    if (gFailures == before) printf("PASS rejected_access_reports_reason\n");
}


/*
 * Scenario 9: two independently constructed contexts running the same bytes
 * must produce identical snapshots; buffers are pre-filled with distinct garbage
 * so shared mutable state would be detected.
 */
typedef struct _SNAPSHOT
{
    ND_UINT64       Gpr[16];
    ND_UINT64       Flags;
    ND_UINT64       Rip;
    ND_UINT64       InstructionsCount;
    ND_UINT64       ExtMemAccess;
    SHEMU_STATUS    Status;
    MEM_LOG         Log;
} SNAPSHOT;

static void
TakeSnapshot(
    const SHEMU_CONTEXT *Ctx,
    SHEMU_STATUS Status,
    SNAPSHOT *Snap
    )
{
    const ND_UINT64 *gpr = &Ctx->Arch.X86.Registers.RegRax;

    memset(Snap, 0, sizeof(*Snap));
    for (unsigned i = 0; i < 16; i++) Snap->Gpr[i] = gpr[i];
    Snap->Flags = Ctx->Arch.X86.Registers.RegFlags;
    Snap->Rip = Ctx->Arch.X86.Registers.RegRip;
    Snap->InstructionsCount = Ctx->InstructionsCount;
    Snap->ExtMemAccess = Ctx->ExtMemAccess;
    Snap->Status = Status;
    Snap->Log = *(MEM_LOG *)Ctx->AuxData;
}

static void
ScenarioRerunIndependence(
    void
    )
{
    static const ND_UINT8 codeProto[] = {
        0x67,0x64,0x8B,0x87,0x00,0x20,0x00,0x00,
        0x67,0x65,0x8B,0x0E,
        0x67,0x64,0x83,0x06,0x01
    };
    ND_UINT8 codeA[sizeof(codeProto)], codeB[sizeof(codeProto)];
    ND_UINT8 stackA[STACK_SIZE], stackB[STACK_SIZE];
    ND_UINT8 intA[INTBUF_SIZE], intB[INTBUF_SIZE];
    MEM_LOG logA, logB;
    SHEMU_CONTEXT ctxA, ctxB;
    RUN_INPUT inA, inB;
    SNAPSHOT snapA, snapB;
    SHEMU_STATUS stA, stB;
    int before = gFailures;

    memcpy(codeA, codeProto, sizeof(codeProto));
    memcpy(codeB, codeProto, sizeof(codeProto));
    memset(stackA, 0x11, sizeof(stackA));
    memset(stackB, 0x22, sizeof(stackB));
    memset(intA, 0x33, sizeof(intA));
    memset(intB, 0x44, sizeof(intB));

    memset(&inA, 0, sizeof(inA));
    inA.Rdi = 0x11112222FFFFF000ULL;
    inA.Rsi = 0x5555666677778804ULL;
    inA.FsBase = 0x10000ULL;
    inA.GsBase = 0x200000ULL;
    inA.MaxInstructions = 3;
    inB = inA;

    BuildContext(&ctxA, codeA, sizeof(codeA), stackA, intA, &logA, &inA);
    BuildContext(&ctxB, codeB, sizeof(codeB), stackB, intB, &logB, &inB);

    stA = ShemuEmulate(&ctxA);
    stB = ShemuEmulate(&ctxB);
    TakeSnapshot(&ctxA, stA, &snapA);
    TakeSnapshot(&ctxB, stB, &snapB);

    CHECK(stA == SHEMU_SUCCESS && stB == SHEMU_SUCCESS,
          "both reruns must succeed, got %u and %u", stA, stB);
    CHECK(memcmp(snapA.Gpr, snapB.Gpr, sizeof(snapA.Gpr)) == 0,
          "GPR state must be identical across independent contexts");
    CHECK(snapA.Flags == snapB.Flags && snapA.Rip == snapB.Rip &&
          snapA.InstructionsCount == snapB.InstructionsCount &&
          snapA.ExtMemAccess == snapB.ExtMemAccess && snapA.Status == snapB.Status,
          "flags/RIP/counters/status must match across reruns");
    CHECK(snapA.Log.Count == snapB.Log.Count,
          "external access counts must match");
    for (ND_UINT32 i = 0; i < snapA.Log.Count && i < MEM_LOG_MAX; i++)
    {
        CHECK(snapA.Log.Rec[i].Addr == snapB.Log.Rec[i].Addr &&
              snapA.Log.Rec[i].Size == snapB.Log.Rec[i].Size &&
              snapA.Log.Rec[i].Store == snapB.Log.Rec[i].Store &&
              snapA.Log.Rec[i].Seq == snapB.Log.Rec[i].Seq &&
              memcmp(snapA.Log.Rec[i].Data, snapB.Log.Rec[i].Data,
                     sizeof(snapA.Log.Rec[i].Data)) == 0,
              "access record %u must match across reruns", i);
    }
    /* This snippet only performs external callback accesses, never stack
     * accesses; each run's distinct pre-fill must therefore survive untouched,
     * which proves the two contexts do not share a mutable stack buffer. */
    {
        int stackAUntouched = 1, stackBUntouched = 1;
        for (unsigned i = 0; i < STACK_SIZE; i++)
        {
            if (stackA[i] != 0x11) stackAUntouched = 0;
            if (stackB[i] != 0x22) stackBUntouched = 0;
        }
        CHECK(stackAUntouched && stackBUntouched,
              "each context must keep its own untouched, separately pre-filled stack");
    }
    CHECK(memcmp(codeA, codeB, sizeof(codeA)) == 0,
          "shellcode bytes must be unchanged by both runs");
    /* And the distinct pre-fills must genuinely differ, so the check above is
     * not vacuous. */
    CHECK(memcmp(stackA, stackB, 64) != 0,
          "test invariant: the two stack pre-fills must be distinguishable");

    if (gFailures == before) printf("PASS rerun_context_independence\n");
}


int
main(
    void
    )
{
    Scenario32BitZeroExtends();
    ScenarioPartialWritesPreserve();
    ScenarioHigh8Selection();
    ScenarioAdcSbbCarry();
    ScenarioCmpConditionalBranch();
    ScenarioAddrSizeOverride();
    ScenarioRmwTwoAccesses();
    ScenarioRejectedAccess();
    ScenarioRerunIndependence();

    printf("SCENARIOS_EXECUTED 9\n");
    printf("BEHAVIOR_ASSERTIONS 3 %s %s %s\n",
           BEH_32BIT_ZERO_EXTEND, BEH_HIGH8_SELECTION, BEH_ADDRSIZE_OVERRIDE);

    if (gFailures == 0)
    {
        printf("NATIVE_WIDTH_OK\n");
        return 0;
    }

    printf("NATIVE_WIDTH_FAILURES %d\n", gFailures);
    return 1;
}
