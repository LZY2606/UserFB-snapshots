/*
 * Copyright (c) 2020 Bitdefender
 * SPDX-License-Identifier: Apache-2.0
 */

//
// x87 stack shape analysis. Tracks only the TOP pointer and the physical non-empty tag mask; no floating
// point values are computed, no memory is read and no input instruction is executed.
//

#include "../inc/bdx86_x87.h"

#include <stdlib.h>
#include <string.h>


//
// Internal, per-instruction analysis node.
//
typedef struct _X87A_NODE
{
    ND_UINT64           Offset;         // Offset inside the analyzed code buffer.
    ND_UINT8            Length;         // Instruction length, in bytes.
    ND_BOOL             IsBarrier;      // CALL or indirect branch: located analysis barrier.
    ND_BOOL             IsRet;          // ND_CAT_RET: path terminates.
    ND_BOOL             IsJmp;          // Unconditional direct jump: follow target only.
    ND_BOOL             IsJcc;          // Conditional direct jump: follow target and fallthrough.
    ND_BOOL             HasTarget;      // A direct branch target is available.
    ND_SINT64            Target;         // Target offset inside the code buffer (may be negative/overflowing).
    BDX87_STACK_ACTION  Action;         // Ordered stack action of the instruction.
    BDX87_STATE         *States;        // One entry per processed incoming shape.
    ND_UINT32           StateCount;
    ND_UINT32           StateCap;

} X87A_NODE, *PX87A_NODE;


//
// Pending exploration work item.
//
typedef struct _X87A_WORK
{
    ND_UINT64           Offset;         // Instruction offset to explore.
    ND_UINT64           Source;         // Branch source offset, X87A_NO_SOURCE for the entry.
    BDX87_SHAPE         Shape;          // Incoming shape.

} X87A_WORK, *PX87A_WORK;


#define X87A_NO_SOURCE      ((ND_UINT64)~0ULL)


//
// Analysis context.
//
typedef struct _X87A_CTX
{
    const ND_UINT8      *Code;
    ND_UINT64           Size;
    ND_UINT64           Base;

    X87A_NODE           *Nodes;         // Reachable instructions, sorted by Offset.
    ND_UINT32           NodeCount;
    ND_UINT32           NodeCap;

    X87A_WORK           *Work;          // Pending exploration.
    ND_UINT32           WorkCount;
    ND_UINT32           WorkCap;

    BDX87_DIAG          *Diags;         // Collected diagnostics (sorted & deduplicated at the end).
    ND_UINT32           DiagCount;
    ND_UINT32           DiagCap;

    ND_BOOL             OutOfMemory;    // Set on allocation failure; aborts the analysis.

} X87A_CTX, *PX87A_CTX;


//
// X87aGetStReg
//
// Return ND_TRUE if the operand is an x87 stack register, in which case Reg receives the st(i) index.
//
static ND_BOOL
X87aGetStReg(
    const ND_OPERAND *Op,
    ND_UINT8 *Reg
    )
{
    if ((Op->Type != ND_OP_REG) || (Op->Info.Register.Type != ND_REG_FPU))
    {
        return ND_FALSE;
    }

    if (Op->Info.Register.Reg >= BDX87_SLOT_COUNT)
    {
        return ND_FALSE;
    }

    *Reg = (ND_UINT8)Op->Info.Register.Reg;

    return ND_TRUE;
}


//
// X87aAddOp
//
// Append one operation to a stack action.
//
static void
X87aAddOp(
    BDX87_STACK_ACTION *Action,
    ND_UINT8 Kind,
    ND_UINT8 LogicalReg,
    ND_SINT8 TopDelta
    )
{
    BDX87_STACK_OP *op;

    if (Action->OpCount >= BDX87_MAX_OPS)
    {
        return;
    }

    op = &Action->Ops[Action->OpCount++];
    op->Kind = Kind;
    op->LogicalReg = LogicalReg;
    op->TopDelta = TopDelta;
    op->Reserved = 0;
}


//
// BdX87GetStackAction
//
NDSTATUS
BdX87GetStackAction(
    const INSTRUX *Instrux,
    BDX87_STACK_ACTION *Action
    )
{
    ND_UINT8 dst, src;

    if ((Instrux == NULL) || (Action == NULL))
    {
        return ND_STATUS_INVALID_PARAMETER;
    }

    memset(Action, 0, sizeof(*Action));
    Action->Supported = ND_TRUE;

    //
    // Only x87, MMX and full FP state save/restore instructions interact with the x87 stack. Anything
    // else leaves the stack shape unchanged.
    //
    if ((Instrux->Category != ND_CAT_X87_ALU) &&
        (Instrux->Category != ND_CAT_MMX) &&
        (Instrux->Category != ND_CAT_XSAVE))
    {
        return ND_STATUS_SUCCESS;
    }

    switch (Instrux->Instruction)
    {
    case ND_INS_FLD:
        //
        // FLD st(i) reads the source under the old TOP and only then pushes. FLD mem simply pushes.
        //
        if (X87aGetStReg(&Instrux->Operands[1], &src))
        {
            X87aAddOp(Action, BDX87_SOP_READ, src, 0);
        }
        X87aAddOp(Action, BDX87_SOP_PUSH, 0, 0);
        break;

    case ND_INS_FLD1:
    case ND_INS_FLDZ:
        X87aAddOp(Action, BDX87_SOP_PUSH, 0, 0);
        break;

    case ND_INS_FST:
        //
        // FST reads st0. The st(i) form then writes the destination under the old TOP; writing does not
        // require the old destination to be non-empty.
        //
        X87aAddOp(Action, BDX87_SOP_READ, 0, 0);
        if (X87aGetStReg(&Instrux->Operands[0], &dst))
        {
            X87aAddOp(Action, BDX87_SOP_WRITE, dst, 0);
        }
        break;

    case ND_INS_FSTP:
        //
        // FSTP st(i) locates and writes the destination under the old TOP, then clears the old st0 and
        // pops. This also holds for i == 0. The memory form has no destination write.
        //
        X87aAddOp(Action, BDX87_SOP_READ, 0, 0);
        if (X87aGetStReg(&Instrux->Operands[0], &dst))
        {
            X87aAddOp(Action, BDX87_SOP_WRITE, dst, 0);
        }
        X87aAddOp(Action, BDX87_SOP_POP, 0, 0);
        break;

    case ND_INS_FXCH:
        //
        // FXCH reads both operands and writes both back.
        //
        if (!X87aGetStReg(&Instrux->Operands[0], &dst) ||
            !X87aGetStReg(&Instrux->Operands[1], &src))
        {
            Action->Supported = ND_FALSE;
            Action->OpCount = 0;
            break;
        }
        X87aAddOp(Action, BDX87_SOP_READ, dst, 0);
        X87aAddOp(Action, BDX87_SOP_READ, src, 0);
        X87aAddOp(Action, BDX87_SOP_WRITE, dst, 0);
        X87aAddOp(Action, BDX87_SOP_WRITE, src, 0);
        break;

    case ND_INS_FFREE:
        //
        // FFREE clears the tag of st(i); TOP is not modified.
        //
        if (!X87aGetStReg(&Instrux->Operands[0], &dst))
        {
            Action->Supported = ND_FALSE;
            Action->OpCount = 0;
            break;
        }
        X87aAddOp(Action, BDX87_SOP_FREE, dst, 0);
        break;

    case ND_INS_FINCSTP:
        X87aAddOp(Action, BDX87_SOP_ROTATE, 0, 1);
        break;

    case ND_INS_FDECSTP:
        X87aAddOp(Action, BDX87_SOP_ROTATE, 0, -1);
        break;

    case ND_INS_FNINIT:
        X87aAddOp(Action, BDX87_SOP_RESET, 0, 0);
        break;

    case ND_INS_FADD:
    case ND_INS_FMUL:
        //
        // The destination is read and written; the source is read. No pop.
        //
        if (!X87aGetStReg(&Instrux->Operands[0], &dst))
        {
            Action->Supported = ND_FALSE;
            Action->OpCount = 0;
            break;
        }
        X87aAddOp(Action, BDX87_SOP_READ, dst, 0);
        if (X87aGetStReg(&Instrux->Operands[1], &src))
        {
            X87aAddOp(Action, BDX87_SOP_READ, src, 0);
        }
        X87aAddOp(Action, BDX87_SOP_WRITE, dst, 0);
        break;

    case ND_INS_FADDP:
    case ND_INS_FMULP:
        //
        // Both sources are read, the destination is written back, and only then is the stack popped.
        //
        if (!X87aGetStReg(&Instrux->Operands[0], &dst) ||
            !X87aGetStReg(&Instrux->Operands[1], &src))
        {
            Action->Supported = ND_FALSE;
            Action->OpCount = 0;
            break;
        }
        X87aAddOp(Action, BDX87_SOP_READ, dst, 0);
        X87aAddOp(Action, BDX87_SOP_READ, src, 0);
        X87aAddOp(Action, BDX87_SOP_WRITE, dst, 0);
        X87aAddOp(Action, BDX87_SOP_POP, 0, 0);
        break;

    default:
        //
        // Any other x87/MMX/FP-state instruction (including full FP state restores) is not modeled;
        // its effect on the stack cannot be assumed to be nil.
        //
        Action->Supported = ND_FALSE;
        Action->OpCount = 0;
        break;
    }

    return ND_STATUS_SUCCESS;
}


//
// X87aApplyAction
//
// Apply a stack action to an incoming shape. On a stack fault the state is marked as not completed and
// the fault kind/slot are returned; the masks describe the accesses performed up to the fault.
//
static void
X87aApplyAction(
    const BDX87_STACK_ACTION *Action,
    BDX87_SHAPE In,
    BDX87_STATE *State,
    BDX87_DIAG_KIND *Fault,
    ND_UINT8 *FaultSlot
    )
{
    ND_UINT8 top, mask;
    ND_UINT32 i;

    memset(State, 0, sizeof(*State));
    State->In = In;
    State->Out = In;
    State->Completed = ND_TRUE;

    *Fault = BDX87_DIAG_NONE;
    *FaultSlot = BDX87_NO_SLOT;

    top = In.Top;
    mask = In.TagMask;

    for (i = 0; i < Action->OpCount; i++)
    {
        const BDX87_STACK_OP *op = &Action->Ops[i];
        ND_UINT8 slot;

        switch (op->Kind)
        {
        case BDX87_SOP_READ:
            slot = (ND_UINT8)((top + op->LogicalReg) & 7);
            if (0 == (mask & (1u << slot)))
            {
                *Fault = BDX87_DIAG_UNDERFLOW;
                *FaultSlot = slot;
                State->Completed = ND_FALSE;
                return;
            }
            State->ReadSlots |= (ND_UINT8)(1u << slot);
            break;

        case BDX87_SOP_WRITE:
            slot = (ND_UINT8)((top + op->LogicalReg) & 7);
            State->WrittenSlots |= (ND_UINT8)(1u << slot);
            mask |= (ND_UINT8)(1u << slot);
            break;

        case BDX87_SOP_PUSH:
            slot = (ND_UINT8)((top - 1) & 7);
            if (0 != (mask & (1u << slot)))
            {
                *Fault = BDX87_DIAG_OVERFLOW;
                *FaultSlot = slot;
                State->Completed = ND_FALSE;
                return;
            }
            top = slot;
            mask |= (ND_UINT8)(1u << slot);
            State->WrittenSlots |= (ND_UINT8)(1u << slot);
            break;

        case BDX87_SOP_POP:
            State->ClearedSlots |= (ND_UINT8)(1u << top);
            mask &= (ND_UINT8)~(1u << top);
            top = (ND_UINT8)((top + 1) & 7);
            break;

        case BDX87_SOP_FREE:
            slot = (ND_UINT8)((top + op->LogicalReg) & 7);
            State->ClearedSlots |= (ND_UINT8)(1u << slot);
            mask &= (ND_UINT8)~(1u << slot);
            break;

        case BDX87_SOP_ROTATE:
            top = (ND_UINT8)((top + op->TopDelta) & 7);
            break;

        case BDX87_SOP_RESET:
            State->ClearedSlots |= mask;
            top = 0;
            mask = 0;
            break;

        default:
            break;
        }
    }

    State->Out.Top = top;
    State->Out.TagMask = mask;
    State->TopDelta = (ND_SINT8)((ND_SINT8)top - (ND_SINT8)In.Top);
}


//
// X87aAddDiag
//
static void
X87aAddDiag(
    PX87A_CTX Ctx,
    BDX87_DIAG_KIND Kind,
    ND_UINT64 Address,
    ND_UINT64 Source,
    ND_UINT8 Slot,
    BDX87_SHAPE Shape
    )
{
    BDX87_DIAG *diag;

    if (Ctx->OutOfMemory)
    {
        return;
    }

    if (Ctx->DiagCount == Ctx->DiagCap)
    {
        ND_UINT32 newCap = (Ctx->DiagCap == 0) ? 16 : Ctx->DiagCap * 2;
        BDX87_DIAG *newDiags = realloc(Ctx->Diags, (size_t)newCap * sizeof(*newDiags));

        if (newDiags == NULL)
        {
            Ctx->OutOfMemory = ND_TRUE;
            return;
        }

        Ctx->Diags = newDiags;
        Ctx->DiagCap = newCap;
    }

    diag = &Ctx->Diags[Ctx->DiagCount++];
    diag->Kind = Kind;
    diag->Address = Address;
    diag->Source = Source;
    diag->Slot = Slot;
    diag->Shape = Shape;
}


//
// X87aFindNode
//
// Binary search for a node by offset. If not found, returns -1 and Insert receives the insertion index.
//
static ND_SINT64
X87aFindNode(
    PX87A_CTX Ctx,
    ND_UINT64 Offset,
    ND_UINT32 *Insert
    )
{
    ND_UINT32 lo = 0, hi = Ctx->NodeCount;

    while (lo < hi)
    {
        ND_UINT32 mid = lo + (hi - lo) / 2;

        if (Ctx->Nodes[mid].Offset < Offset)
        {
            lo = mid + 1;
        }
        else
        {
            hi = mid;
        }
    }

    if (Insert != NULL)
    {
        *Insert = lo;
    }

    if ((lo < Ctx->NodeCount) && (Ctx->Nodes[lo].Offset == Offset))
    {
        return lo;
    }

    return -1;
}


//
// X87aGetNode
//
// Return the node starting at Offset, decoding it on demand. Branch targets landing in the middle of an
// established instruction, truncated instructions and invalid encodings are reported. Returns NULL if no
// valid instruction starts at Offset.
//
static PX87A_NODE
X87aGetNode(
    PX87A_CTX Ctx,
    ND_UINT64 Offset,
    ND_UINT64 Source
    )
{
    ND_SINT64 idx;
    ND_UINT32 insert, i;
    INSTRUX instrux;
    NDSTATUS status;
    PX87A_NODE node;
    ND_UINT64 srcAddr = (Source == X87A_NO_SOURCE) ? Ctx->Base : Ctx->Base + Source;

    idx = X87aFindNode(Ctx, Offset, &insert);
    if (idx >= 0)
    {
        return &Ctx->Nodes[idx];
    }

    //
    // The offset may fall inside an already decoded instruction.
    //
    if (insert > 0)
    {
        PX87A_NODE prev = &Ctx->Nodes[insert - 1];

        if (Offset < prev->Offset + prev->Length)
        {
            BDX87_SHAPE none = { 0, 0 };
            X87aAddDiag(Ctx, BDX87_DIAG_BAD_TARGET, Ctx->Base + Offset, srcAddr, BDX87_NO_SLOT, none);
            return NULL;
        }
    }

    //
    // Decode the instruction. Truncated or invalid encodings are reported.
    //
    status = NdDecodeEx(&instrux, Ctx->Code + Offset, (ND_SIZET)(Ctx->Size - Offset), ND_CODE_64, ND_DATA_64);
    if (!ND_SUCCESS(status))
    {
        BDX87_SHAPE none = { 0, 0 };
        X87aAddDiag(Ctx, BDX87_DIAG_BAD_ENCODING, Ctx->Base + Offset, srcAddr, BDX87_NO_SLOT, none);
        return NULL;
    }

    //
    // The new instruction must not cover an already established instruction start: such a start would
    // be a legal target landing in the middle of this instruction.
    //
    if ((insert < Ctx->NodeCount) && (Ctx->Nodes[insert].Offset < Offset + instrux.Length))
    {
        BDX87_SHAPE none = { 0, 0 };
        X87aAddDiag(Ctx, BDX87_DIAG_BAD_TARGET, Ctx->Base + Ctx->Nodes[insert].Offset,
                    Ctx->Base + Offset, BDX87_NO_SLOT, none);
        return NULL;
    }

    if (Ctx->NodeCount == Ctx->NodeCap)
    {
        ND_UINT32 newCap = (Ctx->NodeCap == 0) ? 32 : Ctx->NodeCap * 2;
        PX87A_NODE newNodes = realloc(Ctx->Nodes, (size_t)newCap * sizeof(*newNodes));

        if (newNodes == NULL)
        {
            Ctx->OutOfMemory = ND_TRUE;
            return NULL;
        }

        Ctx->Nodes = newNodes;
        Ctx->NodeCap = newCap;
    }

    memmove(&Ctx->Nodes[insert + 1], &Ctx->Nodes[insert],
            (size_t)(Ctx->NodeCount - insert) * sizeof(*Ctx->Nodes));
    Ctx->NodeCount++;

    node = &Ctx->Nodes[insert];
    memset(node, 0, sizeof(*node));
    node->Offset = Offset;
    node->Length = instrux.Length;

    node->IsRet = (instrux.Category == ND_CAT_RET);
    node->IsJmp = (instrux.Category == ND_CAT_UNCOND_BR) && !instrux.BranchInfo.IsIndirect;
    node->IsJcc = (instrux.Category == ND_CAT_COND_BR) && !instrux.BranchInfo.IsIndirect;
    node->IsBarrier = (instrux.Category == ND_CAT_CALL) ||
                      (instrux.BranchInfo.IsBranch && instrux.BranchInfo.IsIndirect);

    //
    // Compute the direct branch target, if any.
    //
    if (instrux.BranchInfo.IsBranch && !instrux.BranchInfo.IsIndirect)
    {
        for (i = 0; i < instrux.OperandsCount; i++)
        {
            if (instrux.Operands[i].Type == ND_OP_OFFS)
            {
                node->HasTarget = ND_TRUE;
                node->Target = (ND_SINT64)Offset + instrux.Length +
                               (ND_SINT64)instrux.Operands[i].Info.RelativeOffset.Rel;
                break;
            }
        }
    }

    //
    // Describe the stack action once; it is shared by every shape that reaches this instruction.
    //
    BdX87GetStackAction(&instrux, &node->Action);

    return node;
}


//
// X87aPushWork
//
static void
X87aPushWork(
    PX87A_CTX Ctx,
    ND_UINT64 Offset,
    ND_UINT64 Source,
    BDX87_SHAPE Shape
    )
{
    X87A_WORK *work;

    if (Ctx->OutOfMemory)
    {
        return;
    }

    if (Ctx->WorkCount == Ctx->WorkCap)
    {
        ND_UINT32 newCap = (Ctx->WorkCap == 0) ? 32 : Ctx->WorkCap * 2;
        X87A_WORK *newWork = realloc(Ctx->Work, (size_t)newCap * sizeof(*newWork));

        if (newWork == NULL)
        {
            Ctx->OutOfMemory = ND_TRUE;
            return;
        }

        Ctx->Work = newWork;
        Ctx->WorkCap = newCap;
    }

    work = &Ctx->Work[Ctx->WorkCount++];
    work->Offset = Offset;
    work->Source = Source;
    work->Shape = Shape;
}


//
// X87aFollowEdge
//
// Follow an outgoing edge. Edges leaving the code buffer terminate the path silently.
//
static void
X87aFollowEdge(
    PX87A_CTX Ctx,
    ND_SINT64 Target,
    ND_UINT64 Source,
    BDX87_SHAPE Shape
    )
{
    if ((Target < 0) || ((ND_UINT64)Target >= Ctx->Size))
    {
        return;
    }

    X87aPushWork(Ctx, (ND_UINT64)Target, Source, Shape);
}


//
// X87aProcess
//
// Process one (offset, shape) pair. If the shape was already seen at this instruction, nothing happens;
// this is what makes loops converge.
//
static void
X87aProcess(
    PX87A_CTX Ctx,
    ND_UINT64 Offset,
    ND_UINT64 Source,
    BDX87_SHAPE Shape
    )
{
    PX87A_NODE node;
    BDX87_STATE state, *states;
    BDX87_DIAG_KIND fault;
    ND_UINT8 faultSlot;
    ND_UINT32 i;

    node = X87aGetNode(Ctx, Offset, Source);
    if ((node == NULL) || Ctx->OutOfMemory)
    {
        return;
    }

    //
    // Ignore shapes that have already been processed at this instruction.
    //
    for (i = 0; i < node->StateCount; i++)
    {
        if ((node->States[i].In.Top == Shape.Top) && (node->States[i].In.TagMask == Shape.TagMask))
        {
            return;
        }
    }

    if (!node->Action.Supported)
    {
        //
        // An x87/MMX/FP-state instruction that is not modeled: report it and stop this path.
        //
        memset(&state, 0, sizeof(state));
        state.In = Shape;
        state.Out = Shape;
        state.Completed = ND_FALSE;

        X87aAddDiag(Ctx, BDX87_DIAG_UNSUPPORTED, Ctx->Base + Offset, Ctx->Base + Offset,
                    BDX87_NO_SLOT, Shape);
    }
    else
    {
        X87aApplyAction(&node->Action, Shape, &state, &fault, &faultSlot);

        if (!state.Completed)
        {
            X87aAddDiag(Ctx, fault, Ctx->Base + Offset, Ctx->Base + Offset, faultSlot, Shape);
        }
    }

    //
    // Record the state.
    //
    if (node->StateCount == node->StateCap)
    {
        ND_UINT32 newCap = (node->StateCap == 0) ? 4 : node->StateCap * 2;
        BDX87_STATE *newStates = realloc(node->States, (size_t)newCap * sizeof(*newStates));

        if (newStates == NULL)
        {
            Ctx->OutOfMemory = ND_TRUE;
            return;
        }

        node->States = newStates;
        node->StateCap = newCap;
    }

    states = node->States;
    states[node->StateCount++] = state;

    if (!state.Completed)
    {
        //
        // This shape stops here; other shapes reaching the instruction continue normally.
        //
        return;
    }

    if (node->IsRet || node->IsBarrier)
    {
        //
        // RET terminates the path. CALL and indirect branches are located barriers: they are not
        // followed, and the post-return stack shape is not guessed.
        //
        return;
    }

    if (node->IsJmp)
    {
        //
        // Direct JMP follows only the target.
        //
        if (node->HasTarget)
        {
            X87aFollowEdge(Ctx, node->Target, Offset, state.Out);
        }
        return;
    }

    if (node->IsJcc)
    {
        //
        // Conditional branches are followed on both edges; the condition is not guessed.
        //
        if (node->HasTarget)
        {
            X87aFollowEdge(Ctx, node->Target, Offset, state.Out);
        }
        X87aFollowEdge(Ctx, (ND_SINT64)(Offset + node->Length), Offset, state.Out);
        return;
    }

    //
    // Regular fallthrough.
    //
    X87aFollowEdge(Ctx, (ND_SINT64)(Offset + node->Length), Offset, state.Out);
}


//
// X87aCmpState
//
static int
X87aCmpState(
    const void *A,
    const void *B
    )
{
    const BDX87_STATE *a = A, *b = B;

    if (a->In.Top != b->In.Top)
    {
        return (int)a->In.Top - (int)b->In.Top;
    }

    return (int)a->In.TagMask - (int)b->In.TagMask;
}


//
// X87aCmpDiag
//
static int
X87aCmpDiag(
    const void *A,
    const void *B
    )
{
    const BDX87_DIAG *a = A, *b = B;

    if (a->Address != b->Address)
    {
        return (a->Address < b->Address) ? -1 : 1;
    }
    if (a->Source != b->Source)
    {
        return (a->Source < b->Source) ? -1 : 1;
    }
    if (a->Kind != b->Kind)
    {
        return (int)a->Kind - (int)b->Kind;
    }
    if (a->Shape.Top != b->Shape.Top)
    {
        return (int)a->Shape.Top - (int)b->Shape.Top;
    }
    if (a->Shape.TagMask != b->Shape.TagMask)
    {
        return (int)a->Shape.TagMask - (int)b->Shape.TagMask;
    }

    return (int)a->Slot - (int)b->Slot;
}


//
// BdX87AnalyzeCode
//
NDSTATUS
BdX87AnalyzeCode(
    const ND_UINT8 *Code,
    ND_UINT64 CodeSize,
    ND_UINT64 BaseAddress,
    ND_UINT64 EntryOffset,
    ND_UINT8 InitialTop,
    ND_UINT8 InitialTagMask,
    BDX87_REPORT **Report
    )
{
    X87A_CTX ctx;
    BDX87_REPORT *report;
    BDX87_SHAPE entry;
    ND_UINT32 i, j, out;

    if ((Code == NULL) || (Report == NULL))
    {
        return ND_STATUS_INVALID_PARAMETER;
    }

    *Report = NULL;

    if ((CodeSize == 0) || (EntryOffset >= CodeSize) || (InitialTop >= BDX87_SLOT_COUNT))
    {
        return ND_STATUS_INVALID_PARAMETER;
    }

    memset(&ctx, 0, sizeof(ctx));
    ctx.Code = Code;
    ctx.Size = CodeSize;
    ctx.Base = BaseAddress;

    entry.Top = InitialTop;
    entry.TagMask = InitialTagMask;

    X87aPushWork(&ctx, EntryOffset, X87A_NO_SOURCE, entry);

    //
    // Explore until no new (instruction, shape) pair is produced. The shape space is finite
    // (8 TOP values x 256 tag masks), so this always terminates.
    //
    while (ctx.WorkCount > 0)
    {
        X87A_WORK work = ctx.Work[--ctx.WorkCount];

        X87aProcess(&ctx, work.Offset, work.Source, work.Shape);

        if (ctx.OutOfMemory)
        {
            break;
        }
    }

    if (ctx.OutOfMemory)
    {
        for (i = 0; i < ctx.NodeCount; i++)
        {
            free(ctx.Nodes[i].States);
        }
        free(ctx.Nodes);
        free(ctx.Work);
        free(ctx.Diags);

        return ND_STATUS_INTERNAL_ERROR;
    }

    //
    // Build the independently owned report.
    //
    report = calloc(1, sizeof(*report));
    if (report == NULL)
    {
        for (i = 0; i < ctx.NodeCount; i++)
        {
            free(ctx.Nodes[i].States);
        }
        free(ctx.Nodes);
        free(ctx.Work);
        free(ctx.Diags);

        return ND_STATUS_INTERNAL_ERROR;
    }

    report->BaseAddress = BaseAddress;
    report->CodeSize = CodeSize;
    report->InsnCount = ctx.NodeCount;

    if (ctx.NodeCount > 0)
    {
        report->Insns = calloc(ctx.NodeCount, sizeof(*report->Insns));
        if (report->Insns == NULL)
        {
            BdX87FreeReport(report);
            for (i = 0; i < ctx.NodeCount; i++)
            {
                free(ctx.Nodes[i].States);
            }
            free(ctx.Nodes);
            free(ctx.Work);
            free(ctx.Diags);

            return ND_STATUS_INTERNAL_ERROR;
        }
    }

    for (i = 0; i < ctx.NodeCount; i++)
    {
        PX87A_NODE node = &ctx.Nodes[i];
        BDX87_INSN *insn = &report->Insns[i];

        insn->Address = BaseAddress + node->Offset;
        insn->Offset = node->Offset;
        insn->Length = node->Length;
        insn->IsX87 = (node->Action.OpCount > 0);
        insn->IsBarrier = node->IsBarrier;
        insn->StateCount = node->StateCount;

        if (node->StateCount > 0)
        {
            qsort(node->States, node->StateCount, sizeof(*node->States), X87aCmpState);

            insn->States = malloc((size_t)node->StateCount * sizeof(*insn->States));
            if (insn->States == NULL)
            {
                BdX87FreeReport(report);
                for (j = 0; j < ctx.NodeCount; j++)
                {
                    free(ctx.Nodes[j].States);
                }
                free(ctx.Nodes);
                free(ctx.Work);
                free(ctx.Diags);

                return ND_STATUS_INTERNAL_ERROR;
            }

            memcpy(insn->States, node->States, (size_t)node->StateCount * sizeof(*insn->States));
        }
    }

    //
    // Sort and deduplicate the diagnostics.
    //
    if (ctx.DiagCount > 0)
    {
        qsort(ctx.Diags, ctx.DiagCount, sizeof(*ctx.Diags), X87aCmpDiag);

        out = 0;
        for (i = 1; i < ctx.DiagCount; i++)
        {
            if (X87aCmpDiag(&ctx.Diags[out], &ctx.Diags[i]) != 0)
            {
                ctx.Diags[++out] = ctx.Diags[i];
            }
        }
        ctx.DiagCount = out + 1;

        report->Diags = malloc((size_t)ctx.DiagCount * sizeof(*report->Diags));
        if (report->Diags == NULL)
        {
            BdX87FreeReport(report);
            for (i = 0; i < ctx.NodeCount; i++)
            {
                free(ctx.Nodes[i].States);
            }
            free(ctx.Nodes);
            free(ctx.Work);
            free(ctx.Diags);

            return ND_STATUS_INTERNAL_ERROR;
        }

        memcpy(report->Diags, ctx.Diags, (size_t)ctx.DiagCount * sizeof(*report->Diags));
        report->DiagCount = ctx.DiagCount;
    }

    for (i = 0; i < ctx.NodeCount; i++)
    {
        free(ctx.Nodes[i].States);
    }
    free(ctx.Nodes);
    free(ctx.Work);
    free(ctx.Diags);

    *Report = report;

    return ND_STATUS_SUCCESS;
}


//
// BdX87FreeReport
//
void
BdX87FreeReport(
    BDX87_REPORT *Report
    )
{
    ND_UINT32 i;

    if (Report == NULL)
    {
        return;
    }

    for (i = 0; i < Report->InsnCount; i++)
    {
        free(Report->Insns[i].States);
    }

    free(Report->Insns);
    free(Report->Diags);
    free(Report);
}
