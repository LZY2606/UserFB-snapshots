/*
 * Copyright (c) 2020 Bitdefender
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef BDSHEMU_H
#define BDSHEMU_H


#include "bddisasm.h"
#include "bdshemu_x86.h"


//
// Print function. Used to log emulation traces.
//
typedef void
(*ShemuPrint)(
    char *Data,         // Data to be printed.
    void *Context       // Optional, caller-defined, context.
    );


//
// Access memory function. Simply return true if the access was handled, or false if it wasn't.
// If the function is not provided in SHEMU_CONTEXT, or if it returns false, the emulation will be terminated.
// Note that the integrator is free to handle external memory access as it pleases. 
// Loads could always yield the same value (0), a random value or they could return actual memory values.
// Stores could be discarded, or they could be buffered in a store-buffer like structure.
// Note that by using the ShemuContext, the integrator knows whether the access is user or supervisor (the Ring field
// inside ShemuContext), and he knows whether it is 16/32/64 bit mode (Mode field inside ShemuContext).
// 
typedef ND_BOOL
(*ShemuMemAccess)(
    void *ShemuContext, // Shemu emulation context.
    ND_UINT64 Gla,      // Linear address to be accessed.
    ND_SIZET Size,      // Number of bytes to access.
    ND_UINT8 *Buffer,   // Contains the read content (if Store is false), or the value to be stored at Gla.
    ND_BOOL Store       // If false, read content at Gla. Otherwise, write content at Gla.
    );


typedef enum _SHEMU_ARCH_TYPE
{
    SHEMU_ARCH_TYPE_NONE = 0,

    // X86 includes both IA-32 and x86-64. 
    // All SHEMU flags are supported.
    SHEMU_ARCH_TYPE_X86,

} SHEMU_ARCH_TYPE;


// These match the maximums from bddisasm.
#define SHEMU_X86_GPR_REG_COUNT         ND_MAX_GPR_REGS
#define SHEMU_X86_GPR_REG_SIZE          sizeof(ND_UINT64)
#define SHEMU_X86_MMX_REG_COUNT         ND_MAX_MMX_REGS
#define SHEMU_X86_MMX_REG_SIZE          sizeof(ND_UINT64)
#define SHEMU_X86_SSE_REG_COUNT         ND_MAX_SSE_REGS
#define SHEMU_X86_SSE_REG_SIZE          ND_MAX_REGISTER_SIZE


typedef struct _SHEMU_X86_CTX
{
    // Current instruction. Doesn't have to be provided; it always contains the currently emulated instruction.
    // When #ShemuEmulate returns, this will contain the last emulated instruction. In case of an emulation failure,
    // it can be inspected, to gather more info about what went wrong.
    INSTRUX             Instruction;

    // General purpose registers state. On input, the initial state. Will be updated after each emulated instruction.
    SHEMU_X86_GPR_REGS  Registers;

    // Segment registers state. On input, the initial state. May be updated after some instructions.
    SHEMU_X86_SEG_REGS  Segments;

    // MMX register state. 8 x 8 bytes = 64 bytes for the MMX registers. Can be provided on input, if needed.
    ND_UINT64           MmxRegisters[SHEMU_X86_MMX_REG_COUNT];

    // SSE registers state. 32 x 64 bytes = 2048 bytes for the SSE registers. Can be provided on input, if needed.
    ND_UINT8            SseRegisters[SHEMU_X86_SSE_REG_COUNT][SHEMU_X86_SSE_REG_SIZE];

    // General purpose registers write bitmap. After the first write, a register will be marked dirty in here. 
    // Should be 0 on input.
    ND_UINT8            GprTracker[SHEMU_X86_GPR_REG_COUNT];

    // Operating mode (ND_CODE_16, ND_CODE_32 or ND_CODE_64). Must be provided as input.
    ND_UINT8            Mode;

    // Operating ring (0, 1, 2, 3). Must be provided as input.
    ND_UINT8            Ring;

} SHEMU_X86_CTX;


#define SHEMU_ICACHE_SIZE       0x100

typedef struct SHEMU_ICACHE
{
    // Instruction cache.
    ND_UINT8            Icache[SHEMU_ICACHE_SIZE];

    // The first address that is cached.
    ND_UINT64           Address;

    // Number of valid bytes inside the cache. Maximum SHEMU_ICACHE_SIZE.
    ND_UINT64           Size;
} SHEMU_ICACHE;


typedef struct SHEMU_LOOP_TRACK
{
    // The address of the loop instruction. The loop instruction can be any taken conditional or unconditional
    // branch that goes backwards.
    ND_UINT64           Address;

    // The target of the loop instructions (the first instruction of the loop).
    ND_UINT64           Target;

    // The current iteration number.
    ND_UINT64           Iteration;

    // ND_TRUE whether tracking is active, and we are inside a loop.
    ND_BOOL             Active;

} SHEMU_LOOP_TRACK;


//
// Emulation context. All of these fields must be provided as input, although most of them can be 0. 
//
typedef struct _SHEMU_CONTEXT
{
    union
    {
        // Used when ArchType is SHEMU_ARCH_TYPE_X86.
        SHEMU_X86_CTX   X86;

    } Arch;

    // Indicates architecture mode. Must be provided as input.
    SHEMU_ARCH_TYPE     ArchType;

    // Instruction cache. Note that this caches instruction bytes, not decoded instructions.
    SHEMU_ICACHE        Icache;

    // Tracks emulated loops.
    SHEMU_LOOP_TRACK    LoopTrack;

    // The suspicious code to be emulated. Must be provided as input, as follows:
    // - This buffer must be allocated by the caller, and it must be writeable. It should NOT point to process memory,
    //   as it will be modified by shemu in case of self-modifying code.
    // - However, if the SHEMU_OPT_DIRECT_MAPPED_SHELL option is used, this field can point directly to process memory,
    //   but the AccessShellcode callback must also be provided. In this case, the buffer will NOT be modified by 
    //   shemu.
    ND_UINT8            *Shellcode;

    // Virtual stack. RSP will point somewhere inside. Must be allocated as input, and it can be initialized with
    // actual stack contents. Can also be 0-filled.
    // This buffer must be allocated by the caller, and it must be writeable. It should not point to process memory,
    // as it will be modified by shemu.
    ND_UINT8            *Stack;

    // Internal use. Must be at least the size of the shell + stack. Needs not be initialized, but must be allocated
    // and accessible on input.
    ND_UINT8            *Intbuf;

    // Shellcode base address (the address the shellcode would see). Must be provided as input.
    ND_UINT64           ShellcodeBase;

    // Stack base address (the stack the shellcode would see). Must be provided as input.
    ND_UINT64           StackBase;

    // Shellcode size. Must be provided as input. Usually just a page in size, but can be larger.
    ND_UINT64           ShellcodeSize;

    // Stack size. Must be provided as input. Minimum two pages.
    ND_UINT64           StackSize;

    // Internal buffer size. Must be provided as input. Must be at least the size of the shell + stack.
    ND_UINT64           IntbufSize;
     
    // Number of consecutive NOPs encountered at the beginning of the code. Should be 0 on input.
    ND_UINT64           NopCount;

    // Number of '00 00' (ADD [rax], al) instructions encountered. Should be 0 on input.
    ND_UINT64           NullCount;

    // The length of the string constructed on the stack, if any. Should be 0 on input.
    ND_UINT64           StrLength;

    // Number of external memory access (outside stack/shellcode). Should be 0 on input.
    ND_UINT64           ExtMemAccess;

    // Number of emulated instructions. Should be 0 on input. Once InstructionsCount reaches MaxInstructionsCount,
    // emulation will stop.
    ND_UINT64           InstructionsCount;

    // Number of distinct addresses executed. Will be less than or equal to InstructionsCount. In case of an infinite
    // loop (JMP $), this field will be 1, but the InstructionsCount will be infinite. In case of two overlapping 
    // instructions, this field will be incremented twice (for example, JMP $+1).
    ND_UINT64           UniqueCount;

    // Max number of instructions that should be emulated. Once this limit has been reached, emulation will stop. 
    // Lower values will mean faster processing, but less chances of detection. Higher values mean low performance,
    // but very high chances of yielding useful results. Must be provided as input. 
    ND_UINT64           MaxInstructionsCount;

    // Base address of the Thread Information Block (the TIB the shellcode would normally see). Must be provided as 
    // input.
    ND_UINT64           TibBase;

    // Shellcode Flags (see SHEMU_FLAG_*). Should be 0 on input. Will be non-zero on output if a shellcode indicator 
    // has been met (check SHEMU_FLAG_* values for shellcode indicators).
    // Note that this field should always be checked for detection. No matter the return value of the emulator,
    // if this field is non-zero, a potential shellcode has been detected. This is valid even if 
    // SHEMU_OPT_STOP_ON_EXPLOIT is set: this option only guarantees that emulation will not continue once a shellcode
    // has been encountered, but it does not guarantee that SHEMU_ABORT_SHELLCODE_DETECTED will be returned.
    ND_UINT64           Flags;

    // Emulation options. See SHEMU_OPT_* for possible options. Must be provided as input.
    ND_UINT64           Options;

    // Percent of NOPs (out of total instructions emulated) that trigger NOP sled detection. Must be provided as input. 
    // Defaults to SHEMU_DEFAULT_NOP_THRESHOLD.
    ND_UINT32           NopThreshold;

    // Stack string length threshold. Stack-constructed strings must be at least this long to trigger stack string
    // detection. Must be provided as input. Defaults to SHEMU_DEFAULT_STR_THRESHOLD.
    ND_UINT32           StrThreshold;

    // Number of external mem accesses threshold. No more than this number of external accesses will be issued. Must 
    // be provided as input. Defaults to SHEMU_DEFAULT_MEM_THRESHOLD.
    ND_UINT32           MemThreshold;

    // Optional auxiliary data, provided by the integrator. Can be NULL, or can point to integrator specific data.
    // Shemu will not use this data in any way, but callbacks that receive a SHEMU_CONTEXT pointer (such as
    // #AccessMemory) can use it to reference integrator private information.
    void                *AuxData;

    // If provided, will be used for tracing. Can be NULL.
    ShemuPrint          Log;

    // If provided, will try to access additional memory. Can be NULL.
    ShemuMemAccess      AccessMemory;

    // Must be provided if the the SHEMU_OPT_DIRECT_MAPPED_SHELL option is used. This callback will be used to proxy
    // all accesses made to the shellcode memory, including fetches, loads & stores. The AccessMemory callback is
    // used only for accesses to memory that are not part of the Shellcode or the Stack.
    ShemuMemAccess      AccessShellcode;

    // Optional context to be passed to Log. Can be NULL.
    void                *LogContext;

    // Internal use. When non-NULL, points to an opaque dynamic instruction graph builder which records
    // the executed instructions. Must be NULL on input; ShemuEmulateGraph sets it up internally for the
    // duration of the emulation. Integrators must not use this field in any way.
    void                *GraphBuilder;

} SHEMU_CONTEXT, *PSHEMU_CONTEXT;


typedef unsigned int SHEMU_STATUS;



//
// Emulation abort reasons.
//
#define SHEMU_SUCCESS                           0           // Successfully emulated up to MaxInstructions.
#define SHEMU_ABORT_GLA_OUTSIDE                 1           // A load or store outside the shellcode or the stack.
#define SHEMU_ABORT_RIP_OUTSIDE                 2           // A part of the instruction lies outside the shellcode.
#define SHEMU_ABORT_INSTRUX_NOT_SUPPORTED       3           // An unsupported instruction was encountered.
#define SHEMU_ABORT_OPERAND_NOT_SUPPORTED       4           // An unsupported operand was encountered.
#define SHEMU_ABORT_ADDRESSING_NOT_SUPPORTED    5           // An unsupported addressing scheme used (ie, VSIB).
#define SHEMU_ABORT_REGISTER_NOT_SUPPORTED      6           // An unsupported register was used (ie, DR).
#define SHEMU_ABORT_INVALID_PARAMETER           7           // An invalid parameter was supplied.
#define SHEMU_ABORT_NO_PRIVILEGE                9           // A privileged instruction outside kernel mode.
#define SHEMU_ABORT_CANT_EMULATE                10          // A valid, but only partially handled instruction.
#define SHEMU_ABORT_INVALID_SELECTOR            11          // An invalid selector is loaded.
#define SHEMU_ABORT_UNDEFINED                   12          // Valid encoding, but undefined cominbation of bits.
#define SHEMU_ABORT_UNPREDICTABLE               13          // Instruction behavior is unpredictable.
#define SHEMU_ABORT_MISALIGNED_PC               14          // PC is not aligned to a word.
#define SHEMU_ABORT_FETCH_ERROR                 15          // Could not fetch instruction bytes.
#define SHEMU_ABORT_DECODE_ERROR                16          // Could not decode the instruction.

#define SHEMU_ABORT_SHELLCODE_DETECTED          0xFFFFFFFF  // Shellcode criteria met (see the shellcode flags). 
                                                            // Note that this status may be returned if and only if 
                                                            // the SHEMU_OPT_STOP_ON_EXPLOIT is used.


typedef enum SHEMU_FLAG_ID
{
    shemuFlagIdNopSled,
    shemuFlagIdLoadRip,
    shemuFlagIdWriteSelf,
    shemuFlagIdTebAccessPeb,
    shemuFlagIdSyscall,
    shemuFlagIdStackStr,
    shemuFlagIdTebAccessWow32,
    shemuFlagIdHeavensGate,
    shemuFlagIdStackPivot,
    shemuFlagIdSudAccess,

    // Kernel specific flags.
    shemuFlagIdKpcrAccess = 32,
    shemuFlagIdSwapgs,
    shemuFlagIdSyscallMsrRead,
    shemuFlagIdSyscallMsrWrite,
    shemuFlagIdSidt,
} SHEMU_FLAG_ID;

#define SHEMU_FLAG(id)          (1ull << (id))


//
// Shellcode flags.
//

// General and user-mode flags.

// Long sequence of NOP instructions. Generally present before the actual shellcode. This flag will only be set if:
// 1. Minimum MaxInstructions / 2 instructions have been emulated;
// 2. Minimum NopThreshold fraction (percent) of the emulated instructions are NOPs;
// 3. No other abort condition is met during emulation.
#define SHEMU_FLAG_NOP_SLED                     SHEMU_FLAG(shemuFlagIdNopSled)

// The code loads RIP (CALL/POP, FNSTENV/POP, etc.). Almost always used by shellcodes in order to determine their
// position in memory. This flag will be set when the value of the instruction pointer is loaded into a general
// purpose register by any means. Techniques covered include, but are not limited to:
// 1. CALL + POP reg;
// 2. FP instruction + FNSTENV + loading the saved RIP from the saved FPU state.
// Loading the RIP via RIP relative addressing on x64 does not set this flag.
#define SHEMU_FLAG_LOAD_RIP                     SHEMU_FLAG(shemuFlagIdLoadRip)

// The code writes itself (decryption, unpacking, etc.). Commonly seen if the shellcode decrypts itself in memory.
// This flag will only be set if previously written data is executed. This flag will not be set if, for example,
// chunks of data are written within the shellcode but never executed.
#define SHEMU_FLAG_WRITE_SELF                   SHEMU_FLAG(shemuFlagIdWriteSelf)

// The code accesses the PEB field inside TEB. This is achieved via "FS:[0x30]" or "GS:[0x60]" accesses. Inside
// bdshemu, accesses to the linear address inside TEB is detected no mater how obfuscated - for example, the 
// following instructions will all set this flag:
// 1. MOV eax, gs:[0x30]
// 2. MOV eax, 0x30; MOV eax, fs:[eax]
// 3. MOV eax, 0; MOV eax, fs:[eax+0x30]
#define SHEMU_FLAG_TIB_ACCESS                   SHEMU_FLAG(shemuFlagIdTebAccessPeb)
#define SHEMU_FLAG_TIB_ACCESS_PEB               SHEMU_FLAG_TIB_ACCESS

// The code does a direct syscall/sysenter/int 0x2e|0x80. This should never happen outside the legitimate ntdll
// module. However, payloads may issue direct system calls in order to avoid detection, or to simply avoid fixing
// imports manually.
// Note that this flag will be set when the SYSCALL, SYSENTER, INT 0x2E or INT 0x80 is executed, but only if
// the EAX register contains a value that resembles a valid system call (< 0x1000).
#define SHEMU_FLAG_SYSCALL                      SHEMU_FLAG(shemuFlagIdSyscall)

// The code constructs & uses strings on the stack. The flag will be set only if:
// 1. The length of the string constructed on the stack is at least StrThreshold bytes long (default 8);
// 2. The constructed string is referenced by loading its address anywhere (including a register or memory).
#define SHEMU_FLAG_STACK_STR                    SHEMU_FLAG(shemuFlagIdStackStr)

// The code accesses the Wow32Reserved field inside TIB. This is generally used to issue system calls from Wow64.
#define SHEMU_FLAG_TIB_ACCESS_WOW32             SHEMU_FLAG(shemuFlagIdTebAccessWow32)

// The code uses Heaven's gate to switch into 64 bit mode. This can be abused by shellcodes in order to avoid 
// detection by switching from legacy 32 bit mode to 64 bit mode.
#define SHEMU_FLAG_HEAVENS_GATE                 SHEMU_FLAG(shemuFlagIdHeavensGate)

// The code switches the stack using XCHG esp, *. This is commonly executed by a shellcode once it receives
// control after a stack pivot. By itself, this flag is FP prone, and should generally not be used alone.
// This flag will only be set if several conditions are met:
// 1. The XCHG instruction is used to load a new value in the RSP register
// 2. The new value is naturally aligned (8 bytes in 64-bit mode, 4 bytes in 32-bit mode)
// 3. The new value points either inside the shellcode or the stack area, and at least 64 bytes are valid
#define SHEMU_FLAG_STACK_PIVOT                  SHEMU_FLAG(shemuFlagIdStackPivot)

// The code accesses the KUSER_SHARED_DATA page. Commonly used by shellcodes which wish to issue direct system
// cals or to access various data located inside the SharedUserData page. Only accesses to the following fields
// will set this flag:
// 1. KdDebuggerEnabled (offset 0x2D4)
// 2. SystemCall (offset 0x308)
// 3. Cookie (offset 0x300)
#define SHEMU_FLAG_SUD_ACCESS                   SHEMU_FLAG(shemuFlagIdSudAccess)


// Kernel specific flags.

// KPCR current thread access via gs:[0x188]/fs:[0x124]. Commonly used by kernel shellcodes in order to get the 
// currently exeucting thread.
#define SHEMU_FLAG_KPCR_ACCESS                  SHEMU_FLAG(shemuFlagIdKpcrAccess)

// SWAPGS was executed. Shellcodes may use this if they intercept a low-level event such as the SYSCALL.
#define SHEMU_FLAG_SWAPGS                       SHEMU_FLAG(shemuFlagIdSwapgs)

// A SYSCALL/SYSENTER MSR was read. Commonly used to locate the nt image in order to manually fix imports.
#define SHEMU_FLAG_SYSCALL_MSR_READ             SHEMU_FLAG(shemuFlagIdSyscallMsrRead)

// A SYSCALL/SYSENTER MSR was written. Commonly used to intercept events such as SYSCALLs.
#define SHEMU_FLAG_SYSCALL_MSR_WRITE            SHEMU_FLAG(shemuFlagIdSyscallMsrWrite)

// SIDT was executed. Commonly used to locate the nt image in order to manually fix imports.
#define SHEMU_FLAG_SIDT                         SHEMU_FLAG(shemuFlagIdSidt)



//
// Emulation thresholds.
//

// Percent of emulated instructions that must be NOP to consider a NOP sled is present.
#define SHEMU_DEFAULT_NOP_THRESHOLD             75

// Consecutive printable characters on stack to consider a stack string access.
#define SHEMU_DEFAULT_STR_THRESHOLD             8

// Will not emulate more than this number of external memory accesses. Once this threshold is exceeded, any external
// access will abort the emulation.
#define SHEMU_DEFAULT_MEM_THRESHOLD             0




//
// Emulation options.
//

// Trace each emulated instruction.
#define SHEMU_OPT_TRACE_EMULATION               0x0000000000000001

// When shellcode indications are confirmed, stop emulation. Note that this flag only guarantees that emulation
// will stop once we set any flag, but it does not guarantee that SHEMU_ABORT_SHELLCODE_DETECTED will be returned,
// as an emulation error may take place at any moment. Always check the Flags field of the SHEMU_CONTEXT structure
// to determine whether a detection took place or not.
#define SHEMU_OPT_STOP_ON_EXPLOIT               0x0000000000000002

// When a shellcode self-modifies, the modification will not be committed. Use this when emulating an already
// decoded shellcode, where emulating the decryption again will in fact scramble the shellcode and make it useless.
#define SHEMU_OPT_BYPASS_SELF_WRITES            0x0000000000000004

// Trace each memory access.
#define SHEMU_OPT_TRACE_MEMORY                  0x0000000000000008

// Trace each identified dynamically constructed string.
#define SHEMU_OPT_TRACE_STRINGS                 0x0000000000000010

// Shellcode is directly mapped, and it is not read in a dedicated buffer. No stores can be done to it. This
// allows for arbitrarly sized shellcodes to be emulated without the need to allocate separate memory & do
// copied of the target shellcode. Internally, pieces of the shellcode may still be cached.
// The size of IntBuf must be equal only to the size of the stack plus one page, and no extra memory
// needs to be allocated for the shellcode. When using this flag, the following features will not be available:
// 1. UniqueCount - it will simply indicate the total number of instructions emulated, NOT the number of unique
//    instructions emulated
// 2. WRITE_SELF - self-write detection will be disabled
// Other features will work normally, as they don't require state tracking inside the IntBuf.
// When using this option, the SHEMU_OPT_BYPASS_SELF_WRITES is forced as well.
#define SHEMU_OPT_DIRECT_MAPPED_SHELL           0x0000000000000020

// Trace each identified loop.
#define SHEMU_OPT_TRACE_LOOPS                   0x0000000000000080

// Indicates that AES instructions are supported, and therefore, the AES intrinsics can be used to emulate 
// AES decryption.
#define SHEMU_OPT_SUPPORT_AES                   0x0000000100000000
// Emulate with APX support enabled. If not provided, APX and REX2 prefixed instructions will cause emulation to
// stop.
#define SHEMU_OPT_SUPPORT_APX                   0x0000000200000000




//
// At least this amount must be allocated for internal use.
//
#define SHEMU_INTERNAL_BUFFER_SIZE(ctx)         ((ctx)->ShellcodeSize + (ctx)->StackSize)



//
// Dynamic instruction graph, as recorded by ShemuEmulateGraph. The graph reflects the paths that were
// actually executed during one emulation; paths that were not taken are not present, and no static
// exploration is attempted.
//

// Maximum number of bytes a single x86 instruction can have.
#define SHEMU_GRAPH_MAX_INSTR_BYTES     16


//
// Transfer class of an executed instruction.
//
typedef enum _SHEMU_GRAPH_TRANSFER
{
    // The instruction falls through to the next one.
    SHEMU_GRAPH_TRANSFER_SEQUENTIAL = 0,

    // Conditional branch: Jcc, JrCXZ, LOOP, LOOPZ, LOOPNZ.
    SHEMU_GRAPH_TRANSFER_CONDITIONAL,

    // Unconditional near jump.
    SHEMU_GRAPH_TRANSFER_JUMP,

    // Near call.
    SHEMU_GRAPH_TRANSFER_CALL,

    // Near return.
    SHEMU_GRAPH_TRANSFER_RETURN,

} SHEMU_GRAPH_TRANSFER;


//
// Kind of an edge that was actually executed. Note that the kind reflects the dynamic outcome of the
// instruction, not the relation between the source and destination addresses: a conditional branch
// whose target happens to be the next sequential address still yields SHEMU_GRAPH_EDGE_COND_TAKEN.
//
typedef enum _SHEMU_GRAPH_EDGE_KIND
{
    // Sequential fall-through.
    SHEMU_GRAPH_EDGE_SEQUENTIAL = 0,

    // A conditional branch was taken.
    SHEMU_GRAPH_EDGE_COND_TAKEN,

    // A conditional branch was not taken.
    SHEMU_GRAPH_EDGE_COND_NOT_TAKEN,

    // An unconditional near jump.
    SHEMU_GRAPH_EDGE_JUMP,

    // A near call.
    SHEMU_GRAPH_EDGE_CALL,

    // A near return.
    SHEMU_GRAPH_EDGE_RETURN,

} SHEMU_GRAPH_EDGE_KIND;


//
// Reason the emulation stopped.
//
typedef enum _SHEMU_GRAPH_STOP
{
    // No termination was recorded.
    SHEMU_GRAPH_STOP_NONE = 0,

    // The emulation stopped normally, by reaching MaxInstructionsCount. Status is SHEMU_SUCCESS.
    SHEMU_GRAPH_STOP_LIMIT,

    // The emulation aborted in the middle of a dispatch (unsupported or invalid instruction, denied
    // memory access, RIP outside the shellcode, etc). Status holds the original abort code.
    SHEMU_GRAPH_STOP_ABORT,

} SHEMU_GRAPH_STOP;


//
// A graph node. A node is identified by the original RIP, the decoded instruction length and the full
// instruction bytes as fetched before execution. Executions of the same address with the same bytes
// are merged into a single node; if the bytes differ (self-modifying code), each version is a distinct
// node, even if the length is unchanged.
//
typedef struct _SHEMU_GRAPH_NODE
{
    // Linear address the instruction was fetched from.
    ND_UINT64               Rip;

    // Decoded instruction length, in bytes.
    ND_UINT32               Length;

    // The complete instruction bytes, copied at fetch time. Later self-modifications of the shellcode
    // do not alter this snapshot. Only the first Length bytes are valid.
    ND_UINT8                Bytes[SHEMU_GRAPH_MAX_INSTR_BYTES];

    // The transfer class of the instruction.
    SHEMU_GRAPH_TRANSFER    Transfer;

    // Number of times this exact instruction version was executed.
    ND_UINT64               ExecCount;

} SHEMU_GRAPH_NODE;


//
// A graph edge. Connects two successfully emulated instructions. Edges are recorded per outer
// instruction dispatch; the internal iterations of a repeated (REP) string instruction do not
// create nodes or edges.
//
typedef struct _SHEMU_GRAPH_EDGE
{
    // Index of the source node inside the Nodes array.
    ND_UINT64               From;

    // Index of the destination node inside the Nodes array.
    ND_UINT64               To;

    // The kind of the transition, as produced by the execution of the source (From) instruction.
    SHEMU_GRAPH_EDGE_KIND   Kind;

    // Number of times this exact transition was executed.
    ND_UINT64               Count;

} SHEMU_GRAPH_EDGE;


//
// Termination record. Describes how the emulation ended. No successful successor is fabricated for
// a dispatch that did not complete.
//
typedef struct _SHEMU_GRAPH_TERMINATION
{
    // Why the emulation stopped.
    SHEMU_GRAPH_STOP        Reason;

    // The status returned by the emulation itself.
    SHEMU_STATUS            Status;

    // The address of the dispatch that was being attempted when the emulation stopped. If the
    // emulation stopped normally (SHEMU_GRAPH_STOP_LIMIT), this is the address that would have
    // been dispatched next.
    ND_UINT64               Address;

    // The next RIP as left behind by the last successfully emulated instruction.
    ND_UINT64               NextRip;

} SHEMU_GRAPH_TERMINATION;


//
// The dynamic instruction graph of one emulation. The report independently owns its storage: it
// remains valid after the emulation context, the shellcode and the stack buffers are released.
// Must be freed with ShemuGraphFree.
//
typedef struct _SHEMU_GRAPH
{
    // The executed instruction versions.
    SHEMU_GRAPH_NODE        *Nodes;

    // Number of entries inside Nodes.
    ND_SIZET                NodeCount;

    // The executed transitions.
    SHEMU_GRAPH_EDGE        *Edges;

    // Number of entries inside Edges.
    ND_SIZET                EdgeCount;

    // Total number of successfully emulated instructions (sum of all ExecCount values). This is
    // tracked by the graph recorder itself, not derived from the context InstructionsCount.
    ND_UINT64               ExecutedCount;

    // How the emulation ended.
    SHEMU_GRAPH_TERMINATION Termination;

} SHEMU_GRAPH;




#ifdef __cplusplus 
extern "C" {
#endif

//
// API
//
SHEMU_STATUS
ShemuX86Emulate(
    SHEMU_CONTEXT *Context
    );

SHEMU_STATUS
ShemuEmulate(
    SHEMU_CONTEXT *Context
    );

//
// ShemuEmulateGraph - emulates the shellcode exactly like ShemuEmulate (same executor, same context
// side-effects, same returned status), and additionally records the dynamic instruction graph of the
// emulation. On return, Graph points to a newly allocated report that is owned by the caller and is
// independent of the context and of the input buffers; it must be released with ShemuGraphFree.
// The successfully emulated prefix of the graph is available even if the emulation aborts. If the
// graph cannot be recorded (for example, out of memory), *Graph is set to NULL, but the emulation
// still proceeds normally and its status is returned.
//
SHEMU_STATUS
ShemuEmulateGraph(
    SHEMU_CONTEXT *Context,
    SHEMU_GRAPH **Graph
    );

//
// ShemuGraphFree - releases a graph report obtained from ShemuEmulateGraph. A NULL report is
// accepted, and is simply ignored.
//
void
ShemuGraphFree(
    SHEMU_GRAPH *Graph
    );

#ifdef __cplusplus 
}
#endif

#endif // BDSHEMU_H
