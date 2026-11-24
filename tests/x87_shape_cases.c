//
// x87 stack shape analysis tests. Registered through task_cases/task_case_count; see task_support/check.h.
//
#include "task_support/check.h"

#include <string.h>

#include "bddisasm.h"
#include "bdx86_x87.h"


#define BASE    0x1000ULL


//
// find_insn
//
static const BDX87_INSN *
find_insn(
    const BDX87_REPORT *Report,
    ND_UINT64 Offset
    )
{
    ND_UINT32 i;

    for (i = 0; i < Report->InsnCount; i++)
    {
        if (Report->Insns[i].Offset == Offset)
        {
            return &Report->Insns[i];
        }
    }

    return NULL;
}


//
// find_state
//
static const BDX87_STATE *
find_state(
    const BDX87_INSN *Insn,
    ND_UINT8 Top,
    ND_UINT8 TagMask
    )
{
    ND_UINT32 i;

    for (i = 0; i < Insn->StateCount; i++)
    {
        if ((Insn->States[i].In.Top == Top) && (Insn->States[i].In.TagMask == TagMask))
        {
            return &Insn->States[i];
        }
    }

    return NULL;
}


//
// find_diag
//
static const BDX87_DIAG *
find_diag(
    const BDX87_REPORT *Report,
    BDX87_DIAG_KIND Kind
    )
{
    ND_UINT32 i;

    for (i = 0; i < Report->DiagCount; i++)
    {
        if (Report->Diags[i].Kind == Kind)
        {
            return &Report->Diags[i];
        }
    }

    return NULL;
}


//
// analyze
//
static int
analyze(
    const ND_UINT8 *Code,
    size_t Size,
    ND_UINT8 Top,
    ND_UINT8 TagMask,
    BDX87_REPORT **Report
    )
{
    return ND_SUCCESS(BdX87AnalyzeCode(Code, Size, BASE, 0, Top, TagMask, Report)) && (*Report != NULL);
}


//
// get_action
//
static int
get_action(
    const ND_UINT8 *Code,
    size_t Size,
    BDX87_STACK_ACTION *Action
    )
{
    INSTRUX instrux;

    if (!ND_SUCCESS(NdDecodeEx(&instrux, Code, Size, ND_CODE_64, ND_DATA_64)))
    {
        return 0;
    }

    return ND_SUCCESS(BdX87GetStackAction(&instrux, Action));
}


//
// transfers: FLD st(i) reads the source under the old TOP, and only then pushes.
//
static int
case_fld_st_pushes_after_read(void)
{
    static const ND_UINT8 code[] = { 0xD9, 0xC2 };          // FLD st2
    BDX87_REPORT *rep = NULL;
    BDX87_STACK_ACTION action;
    const BDX87_INSN *insn;
    const BDX87_STATE *st;

    TASK_CHECK(get_action(code, sizeof(code), &action));
    TASK_CHECK(action.Supported);
    TASK_CHECK(action.OpCount == 2);
    TASK_CHECK(action.Ops[0].Kind == BDX87_SOP_READ && action.Ops[0].LogicalReg == 2);
    TASK_CHECK(action.Ops[1].Kind == BDX87_SOP_PUSH);

    // TOP = 3, only physical slot 5 (st2) is tagged.
    TASK_CHECK(analyze(code, sizeof(code), 3, 0x20, &rep));
    TASK_CHECK(rep->InsnCount == 1);
    TASK_CHECK(rep->DiagCount == 0);

    insn = find_insn(rep, 0);
    TASK_CHECK(insn != NULL);
    TASK_CHECK(insn->IsX87);
    TASK_CHECK(insn->StateCount == 1);

    st = find_state(insn, 3, 0x20);
    TASK_CHECK(st != NULL);
    TASK_CHECK(st->Completed);
    TASK_CHECK(st->ReadSlots == 0x20);                      // physical slot 5 read before the push
    TASK_CHECK(st->WrittenSlots == 0x04);                   // physical slot 2 is the push target
    TASK_CHECK(st->ClearedSlots == 0);
    TASK_CHECK(st->TopDelta == -1);
    TASK_CHECK(st->Out.Top == 2 && st->Out.TagMask == 0x24);

    BdX87FreeReport(rep);
    return 0;
}


//
// transfers: FSTP st(i) writes the destination under the old TOP, then clears the old st0 and pops.
//
static int
case_fstp_st_writes_then_pops(void)
{
    static const ND_UINT8 fstp_st2[] = { 0xDD, 0xDA };      // FSTP st2
    static const ND_UINT8 fstp_st0[] = { 0xDD, 0xD8 };      // FSTP st0
    BDX87_REPORT *rep = NULL;
    BDX87_STACK_ACTION action;
    const BDX87_INSN *insn;
    const BDX87_STATE *st;

    TASK_CHECK(get_action(fstp_st2, sizeof(fstp_st2), &action));
    TASK_CHECK(action.OpCount == 3);
    TASK_CHECK(action.Ops[0].Kind == BDX87_SOP_READ && action.Ops[0].LogicalReg == 0);
    TASK_CHECK(action.Ops[1].Kind == BDX87_SOP_WRITE && action.Ops[1].LogicalReg == 2);
    TASK_CHECK(action.Ops[2].Kind == BDX87_SOP_POP);

    // TOP = 1, slots 1 (st0) and 3 (st2) tagged.
    TASK_CHECK(analyze(fstp_st2, sizeof(fstp_st2), 1, 0x0A, &rep));
    TASK_CHECK(rep->DiagCount == 0);
    insn = find_insn(rep, 0);
    TASK_CHECK(insn != NULL);
    st = find_state(insn, 1, 0x0A);
    TASK_CHECK(st != NULL);
    TASK_CHECK(st->Completed);
    TASK_CHECK(st->ReadSlots == 0x02);                      // st0 is physical slot 1
    TASK_CHECK(st->WrittenSlots == 0x08);                   // st2 is physical slot 3
    TASK_CHECK(st->ClearedSlots == 0x02);                   // old st0 tag cleared by the pop
    TASK_CHECK(st->TopDelta == 1);
    TASK_CHECK(st->Out.Top == 2 && st->Out.TagMask == 0x08);
    BdX87FreeReport(rep);
    rep = NULL;

    // i == 0 behaves the same: write st0, then clear it and pop.
    TASK_CHECK(analyze(fstp_st0, sizeof(fstp_st0), 1, 0x02, &rep));
    TASK_CHECK(rep->DiagCount == 0);
    insn = find_insn(rep, 0);
    TASK_CHECK(insn != NULL);
    st = find_state(insn, 1, 0x02);
    TASK_CHECK(st != NULL);
    TASK_CHECK(st->Completed);
    TASK_CHECK(st->ReadSlots == 0x02);
    TASK_CHECK(st->WrittenSlots == 0x02);
    TASK_CHECK(st->ClearedSlots == 0x02);
    TASK_CHECK(st->Out.Top == 2 && st->Out.TagMask == 0x00);
    BdX87FreeReport(rep);

    return 0;
}


//
// transfers: FFREE clears only the tag and leaves a hole; TOP does not move.
//
static int
case_ffree_leaves_hole(void)
{
    static const ND_UINT8 code[] = { 0xDD, 0xC1,            // FFREE st1
                                     0xDD, 0xDA };          // FSTP st2
    BDX87_REPORT *rep = NULL;
    const BDX87_INSN *insn;
    const BDX87_STATE *st;

    TASK_CHECK(analyze(code, sizeof(code), 0, 0x07, &rep));
    TASK_CHECK(rep->InsnCount == 2);
    TASK_CHECK(rep->DiagCount == 0);

    insn = find_insn(rep, 0);                               // FFREE st1
    TASK_CHECK(insn != NULL);
    st = find_state(insn, 0, 0x07);
    TASK_CHECK(st != NULL);
    TASK_CHECK(st->Completed);
    TASK_CHECK(st->ReadSlots == 0 && st->WrittenSlots == 0);
    TASK_CHECK(st->ClearedSlots == 0x02);                   // physical slot 1 freed
    TASK_CHECK(st->TopDelta == 0);
    TASK_CHECK(st->Out.Top == 0 && st->Out.TagMask == 0x05);

    insn = find_insn(rep, 2);                               // FSTP st2
    TASK_CHECK(insn != NULL);
    st = find_state(insn, 0, 0x05);
    TASK_CHECK(st != NULL);
    TASK_CHECK(st->Completed);
    TASK_CHECK(st->ReadSlots == 0x01);
    TASK_CHECK(st->WrittenSlots == 0x04);
    TASK_CHECK(st->ClearedSlots == 0x01);
    TASK_CHECK(st->Out.Top == 1 && st->Out.TagMask == 0x04);    // the slot 1 hole persists

    BdX87FreeReport(rep);
    return 0;
}


//
// transfers: FINCSTP rotates TOP only; subsequent reads land on the rotated physical slots.
//
static int
case_fincstp_rotates_then_reads(void)
{
    static const ND_UINT8 code[] = { 0xD9, 0xF7,            // FINCSTP
                                     0xD8, 0xC1 };          // FADD st0, st1
    static const ND_UINT8 fdecstp[] = { 0xD9, 0xF6 };       // FDECSTP
    BDX87_REPORT *rep = NULL;
    BDX87_STACK_ACTION action;
    const BDX87_INSN *insn;
    const BDX87_STATE *st;

    TASK_CHECK(get_action(fdecstp, sizeof(fdecstp), &action));
    TASK_CHECK(action.OpCount == 1);
    TASK_CHECK(action.Ops[0].Kind == BDX87_SOP_ROTATE && action.Ops[0].TopDelta == -1);

    TASK_CHECK(analyze(code, sizeof(code), 3, 0xFF, &rep));
    TASK_CHECK(rep->InsnCount == 2);
    TASK_CHECK(rep->DiagCount == 0);

    insn = find_insn(rep, 0);                               // FINCSTP
    TASK_CHECK(insn != NULL);
    st = find_state(insn, 3, 0xFF);
    TASK_CHECK(st != NULL);
    TASK_CHECK(st->Completed);
    TASK_CHECK(st->ReadSlots == 0 && st->WrittenSlots == 0 && st->ClearedSlots == 0);
    TASK_CHECK(st->TopDelta == 1);
    TASK_CHECK(st->Out.Top == 4 && st->Out.TagMask == 0xFF);    // tags do not rotate

    insn = find_insn(rep, 2);                               // FADD st0, st1
    TASK_CHECK(insn != NULL);
    st = find_state(insn, 4, 0xFF);
    TASK_CHECK(st != NULL);
    TASK_CHECK(st->Completed);
    TASK_CHECK(st->ReadSlots == 0x30);                      // physical slots 4 and 5
    TASK_CHECK(st->WrittenSlots == 0x10);                   // destination st0 is slot 4
    TASK_CHECK(st->TopDelta == 0);
    TASK_CHECK(st->Out.Top == 4 && st->Out.TagMask == 0xFF);

    BdX87FreeReport(rep);
    return 0;
}


//
// transfers: FADDP reads both sources, writes the destination back, and only then pops.
//
static int
case_faddp_writes_then_pops(void)
{
    static const ND_UINT8 code[] = { 0xDE, 0xC2 };          // FADDP st2, st0
    BDX87_REPORT *rep = NULL;
    BDX87_STACK_ACTION action;
    const BDX87_INSN *insn;
    const BDX87_STATE *st;

    TASK_CHECK(get_action(code, sizeof(code), &action));
    TASK_CHECK(action.OpCount == 4);
    TASK_CHECK(action.Ops[0].Kind == BDX87_SOP_READ && action.Ops[0].LogicalReg == 2);
    TASK_CHECK(action.Ops[1].Kind == BDX87_SOP_READ && action.Ops[1].LogicalReg == 0);
    TASK_CHECK(action.Ops[2].Kind == BDX87_SOP_WRITE && action.Ops[2].LogicalReg == 2);
    TASK_CHECK(action.Ops[3].Kind == BDX87_SOP_POP);

    TASK_CHECK(analyze(code, sizeof(code), 0, 0x07, &rep));
    TASK_CHECK(rep->DiagCount == 0);
    insn = find_insn(rep, 0);
    TASK_CHECK(insn != NULL);
    st = find_state(insn, 0, 0x07);
    TASK_CHECK(st != NULL);
    TASK_CHECK(st->Completed);
    TASK_CHECK(st->ReadSlots == 0x05);                      // slots 0 and 2
    TASK_CHECK(st->WrittenSlots == 0x04);                   // slot 2 written back
    TASK_CHECK(st->ClearedSlots == 0x01);                   // old st0 popped
    TASK_CHECK(st->TopDelta == 1);
    TASK_CHECK(st->Out.Top == 1 && st->Out.TagMask == 0x06);

    BdX87FreeReport(rep);
    return 0;
}


//
// transfers: FNINIT resets TOP to 0 and empties all tags.
//
static int
case_fninit_resets(void)
{
    static const ND_UINT8 code[] = { 0xDB, 0xE3 };          // FNINIT
    BDX87_REPORT *rep = NULL;
    const BDX87_INSN *insn;
    const BDX87_STATE *st;

    TASK_CHECK(analyze(code, sizeof(code), 5, 0xB0, &rep));
    TASK_CHECK(rep->DiagCount == 0);
    insn = find_insn(rep, 0);
    TASK_CHECK(insn != NULL);
    st = find_state(insn, 5, 0xB0);
    TASK_CHECK(st != NULL);
    TASK_CHECK(st->Completed);
    TASK_CHECK(st->ClearedSlots == 0xB0);
    TASK_CHECK(st->TopDelta == -5);
    TASK_CHECK(st->Out.Top == 0 && st->Out.TagMask == 0x00);

    BdX87FreeReport(rep);
    return 0;
}


//
// transfers: FLD1/FLDZ push without reading anything.
//
static int
case_load_constants_push(void)
{
    static const ND_UINT8 code[] = { 0xD9, 0xE8,            // FLD1
                                     0xD9, 0xEE };          // FLDZ
    BDX87_REPORT *rep = NULL;
    const BDX87_INSN *insn;
    const BDX87_STATE *st;

    TASK_CHECK(analyze(code, sizeof(code), 4, 0x00, &rep));
    TASK_CHECK(rep->InsnCount == 2);
    TASK_CHECK(rep->DiagCount == 0);

    insn = find_insn(rep, 0);                               // FLD1
    TASK_CHECK(insn != NULL);
    st = find_state(insn, 4, 0x00);
    TASK_CHECK(st != NULL);
    TASK_CHECK(st->Completed);
    TASK_CHECK(st->ReadSlots == 0);
    TASK_CHECK(st->WrittenSlots == 0x08);                   // push target is slot 3
    TASK_CHECK(st->Out.Top == 3 && st->Out.TagMask == 0x08);

    insn = find_insn(rep, 2);                               // FLDZ
    TASK_CHECK(insn != NULL);
    st = find_state(insn, 3, 0x08);
    TASK_CHECK(st != NULL);
    TASK_CHECK(st->Completed);
    TASK_CHECK(st->WrittenSlots == 0x04);                   // push target is slot 2
    TASK_CHECK(st->Out.Top == 2 && st->Out.TagMask == 0x0C);

    BdX87FreeReport(rep);
    return 0;
}


//
// joins: two branches leave different shapes behind; the join keeps the exact (TOP, mask) pairs
// instead of unioning TOP and tags separately.
//
static int
case_branch_shapes_rejoin(void)
{
    static const ND_UINT8 code[] = {
        0x74, 0x04,                 //  0: JZ 6
        0xDD, 0xD8,                 //  2: FSTP st0        -> (TOP 1, mask 0x02)
        0xEB, 0x02,                 //  4: JMP 8
        0xDD, 0xC1,                 //  6: FFREE st1       -> (TOP 0, mask 0x01)
        0xD9, 0xC0,                 //  8: FLD st0         <- join
        0xC3                        // 10: RET
    };
    BDX87_REPORT *rep = NULL;
    const BDX87_INSN *insn;
    const BDX87_STATE *st;

    TASK_CHECK(analyze(code, sizeof(code), 0, 0x03, &rep));
    TASK_CHECK(rep->InsnCount == 6);
    TASK_CHECK(rep->DiagCount == 0);

    insn = find_insn(rep, 8);                               // the join
    TASK_CHECK(insn != NULL);
    TASK_CHECK(insn->StateCount == 2);

    // The two genuine combinations, and only those.
    st = find_state(insn, 1, 0x02);
    TASK_CHECK(st != NULL);
    TASK_CHECK(st->Completed);
    TASK_CHECK(st->ReadSlots == 0x02);                      // st0 is physical slot 1
    TASK_CHECK(st->WrittenSlots == 0x01);                   // push target is slot 0
    TASK_CHECK(st->Out.Top == 0 && st->Out.TagMask == 0x03);

    st = find_state(insn, 0, 0x01);
    TASK_CHECK(st != NULL);
    TASK_CHECK(st->Completed);
    TASK_CHECK(st->ReadSlots == 0x01);
    TASK_CHECK(st->WrittenSlots == 0x80);                   // push target is slot 7
    TASK_CHECK(st->Out.Top == 7 && st->Out.TagMask == 0x81);

    // Phantom combinations produced by unioning TOP and tags separately must not appear.
    TASK_CHECK(find_state(insn, 0, 0x02) == NULL);
    TASK_CHECK(find_state(insn, 1, 0x01) == NULL);

    // Both outcomes flow into the RET.
    insn = find_insn(rep, 10);
    TASK_CHECK(insn != NULL);
    TASK_CHECK(insn->StateCount == 2);
    TASK_CHECK(find_state(insn, 0, 0x03) != NULL);
    TASK_CHECK(find_state(insn, 7, 0x81) != NULL);

    BdX87FreeReport(rep);
    return 0;
}


//
// joins: a rotating loop reaches a stable set of states and stops expanding.
//
static int
case_loop_states_stabilize(void)
{
    static const ND_UINT8 code[] = {
        0xD9, 0xF7,                 // 0: FINCSTP
        0x75, 0xFC,                 // 2: JNZ 0
        0xC3                        // 4: RET
    };
    BDX87_REPORT *rep = NULL;
    const BDX87_INSN *insn;
    const BDX87_STATE *st;
    ND_UINT8 top;

    TASK_CHECK(analyze(code, sizeof(code), 0, 0x01, &rep));
    TASK_CHECK(rep->InsnCount == 3);
    TASK_CHECK(rep->DiagCount == 0);

    // TOP cycles through all 8 values; the tag mask stays put. 8 states, then stable.
    insn = find_insn(rep, 0);
    TASK_CHECK(insn != NULL);
    TASK_CHECK(insn->StateCount == 8);
    for (top = 0; top < 8; top++)
    {
        st = find_state(insn, top, 0x01);
        TASK_CHECK(st != NULL);
        TASK_CHECK(st->Completed);
        TASK_CHECK(st->Out.Top == ((top + 1) & 7));
        TASK_CHECK(st->Out.TagMask == 0x01);
    }

    // The loop exit is reachable with every rotated shape.
    insn = find_insn(rep, 4);
    TASK_CHECK(insn != NULL);
    TASK_CHECK(insn->StateCount == 8);

    BdX87FreeReport(rep);
    return 0;
}


//
// joins: a bounded loop keeps both the normal states and the underflow diagnostic of the
// path that pops one item too many.
//
static int
case_loop_underflow_persists(void)
{
    static const ND_UINT8 code[] = {
        0xDD, 0xD8,                 // 0: FSTP st0
        0xD9, 0xF6,                 // 2: FDECSTP
        0x75, 0xFA,                 // 4: JNZ 0
        0xC3                        // 6: RET
    };
    BDX87_REPORT *rep = NULL;
    const BDX87_INSN *insn;
    const BDX87_STATE *st;
    const BDX87_DIAG *diag;

    TASK_CHECK(analyze(code, sizeof(code), 0, 0x01, &rep));
    TASK_CHECK(rep->InsnCount == 4);

    insn = find_insn(rep, 0);
    TASK_CHECK(insn != NULL);
    TASK_CHECK(insn->StateCount == 2);

    // First iteration pops the single item normally.
    st = find_state(insn, 0, 0x01);
    TASK_CHECK(st != NULL);
    TASK_CHECK(st->Completed);
    TASK_CHECK(st->Out.Top == 1 && st->Out.TagMask == 0x00);

    // Second iteration reads the now-empty st0: underflow, and this shape stops.
    st = find_state(insn, 0, 0x00);
    TASK_CHECK(st != NULL);
    TASK_CHECK(!st->Completed);

    TASK_CHECK(rep->DiagCount == 1);
    diag = find_diag(rep, BDX87_DIAG_UNDERFLOW);
    TASK_CHECK(diag != NULL);
    TASK_CHECK(diag->Address == BASE + 0);
    TASK_CHECK(diag->Slot == 0);
    TASK_CHECK(diag->Shape.Top == 0 && diag->Shape.TagMask == 0x00);

    // The loop exit is still reachable with the empty shape.
    insn = find_insn(rep, 6);
    TASK_CHECK(insn != NULL);
    TASK_CHECK(find_state(insn, 0, 0x00) != NULL);

    BdX87FreeReport(rep);
    return 0;
}


//
// joins: edges leaving the code buffer terminate the path quietly.
//
static int
case_outbound_edges_terminate(void)
{
    static const ND_UINT8 jump_out[] = { 0xEB, 0x64,        // JMP +0x64 (outside)
                                         0xD9, 0xE8 };      // FLD1 (unreachable)
    static const ND_UINT8 ret_first[] = { 0xC3,             // RET
                                          0xD9, 0xE8 };     // FLD1 (unreachable)
    BDX87_REPORT *rep = NULL;

    TASK_CHECK(analyze(jump_out, sizeof(jump_out), 0, 0x00, &rep));
    TASK_CHECK(rep->InsnCount == 1);
    TASK_CHECK(rep->DiagCount == 0);
    TASK_CHECK(find_insn(rep, 2) == NULL);
    BdX87FreeReport(rep);
    rep = NULL;

    TASK_CHECK(analyze(ret_first, sizeof(ret_first), 0, 0x00, &rep));
    TASK_CHECK(rep->InsnCount == 1);
    TASK_CHECK(rep->DiagCount == 0);
    TASK_CHECK(find_insn(rep, 1) == NULL);
    BdX87FreeReport(rep);

    return 0;
}


//
// faults: reading an empty slot is an underflow naming the slot, and the shape stops there.
//
static int
case_read_empty_slot_underflows(void)
{
    static const ND_UINT8 code[] = { 0xD8, 0xC1,            // FADD st0, st1
                                     0xD9, 0xE8 };          // FLD1 (unreachable on this shape)
    BDX87_REPORT *rep = NULL;
    const BDX87_INSN *insn;
    const BDX87_STATE *st;
    const BDX87_DIAG *diag;

    // TOP = 2, only slot 2 (st0) tagged; st1 is physical slot 3, which is empty.
    TASK_CHECK(analyze(code, sizeof(code), 2, 0x04, &rep));
    TASK_CHECK(rep->InsnCount == 1);                        // the fault stops the only shape
    TASK_CHECK(rep->DiagCount == 1);

    diag = find_diag(rep, BDX87_DIAG_UNDERFLOW);
    TASK_CHECK(diag != NULL);
    TASK_CHECK(diag->Address == BASE + 0);
    TASK_CHECK(diag->Slot == 3);
    TASK_CHECK(diag->Shape.Top == 2 && diag->Shape.TagMask == 0x04);

    insn = find_insn(rep, 0);
    TASK_CHECK(insn != NULL);
    st = find_state(insn, 2, 0x04);
    TASK_CHECK(st != NULL);
    TASK_CHECK(!st->Completed);
    TASK_CHECK(st->ReadSlots == 0x04);                      // st0 was read before the fault
    TASK_CHECK(st->WrittenSlots == 0x00);                   // the destination was never written

    BdX87FreeReport(rep);
    return 0;
}


//
// faults: pushing onto a non-empty slot is an overflow naming the slot.
//
static int
case_push_onto_full_slot_overflows(void)
{
    static const ND_UINT8 code[] = { 0xD9, 0xE8,            // FLD1
                                     0x90 };                // NOP (unreachable on this shape)
    BDX87_REPORT *rep = NULL;
    const BDX87_INSN *insn;
    const BDX87_STATE *st;
    const BDX87_DIAG *diag;

    // TOP = 1, all slots tagged: the push target is slot 0, which is non-empty.
    TASK_CHECK(analyze(code, sizeof(code), 1, 0xFF, &rep));
    TASK_CHECK(rep->InsnCount == 1);
    TASK_CHECK(rep->DiagCount == 1);

    diag = find_diag(rep, BDX87_DIAG_OVERFLOW);
    TASK_CHECK(diag != NULL);
    TASK_CHECK(diag->Address == BASE + 0);
    TASK_CHECK(diag->Slot == 0);
    TASK_CHECK(diag->Shape.Top == 1 && diag->Shape.TagMask == 0xFF);

    insn = find_insn(rep, 0);
    TASK_CHECK(insn != NULL);
    st = find_state(insn, 1, 0xFF);
    TASK_CHECK(st != NULL);
    TASK_CHECK(!st->Completed);
    TASK_CHECK(st->WrittenSlots == 0x00);

    BdX87FreeReport(rep);
    return 0;
}


//
// faults: unmodeled x87/MMX/state-restore instructions are reported and stop the path.
//
static int
case_unsupported_stops_path(void)
{
    static const ND_UINT8 code[] = { 0xDE, 0xD9,            // FCOMPP
                                     0xD9, 0xE8 };          // FLD1 (unreachable)
    static const ND_UINT8 emms[] = { 0x0F, 0x77 };          // EMMS
    static const ND_UINT8 frstor[] = { 0xDD, 0x20 };        // FRSTOR [rax]
    static const ND_UINT8 fild[] = { 0xDB, 0x00 };          // FILD dword [rax]
    static const ND_UINT8 fldenv[] = { 0xD9, 0x20 };        // FLDENV [rax]
    static const ND_UINT8 add[] = { 0x48, 0x01, 0xD8 };     // ADD rax, rbx
    BDX87_REPORT *rep = NULL;
    BDX87_STACK_ACTION action;
    const BDX87_INSN *insn;
    const BDX87_STATE *st;
    const BDX87_DIAG *diag;

    TASK_CHECK(analyze(code, sizeof(code), 0, 0x03, &rep));
    TASK_CHECK(rep->InsnCount == 1);                        // FLD1 is never reached
    TASK_CHECK(rep->DiagCount == 1);

    diag = find_diag(rep, BDX87_DIAG_UNSUPPORTED);
    TASK_CHECK(diag != NULL);
    TASK_CHECK(diag->Address == BASE + 0);

    insn = find_insn(rep, 0);
    TASK_CHECK(insn != NULL);
    st = find_state(insn, 0, 0x03);
    TASK_CHECK(st != NULL);
    TASK_CHECK(!st->Completed);
    BdX87FreeReport(rep);

    // The instruction-level interface agrees on what cannot be modeled.
    TASK_CHECK(get_action(emms, sizeof(emms), &action));
    TASK_CHECK(!action.Supported);
    TASK_CHECK(get_action(frstor, sizeof(frstor), &action));
    TASK_CHECK(!action.Supported);
    TASK_CHECK(get_action(fild, sizeof(fild), &action));
    TASK_CHECK(!action.Supported);
    TASK_CHECK(get_action(fldenv, sizeof(fldenv), &action));
    TASK_CHECK(!action.Supported);

    // Plain integer instructions are supported and leave the stack alone.
    TASK_CHECK(get_action(add, sizeof(add), &action));
    TASK_CHECK(action.Supported);
    TASK_CHECK(action.OpCount == 0);

    return 0;
}


//
// faults: a branch target landing in the middle of an instruction is reported.
//
static int
case_bad_target_reported(void)
{
    static const ND_UINT8 code[] = {
        0x74, 0x01,                 // 0: JZ 3 - lands inside FLD1
        0xD9, 0xE8                  // 2: FLD1
    };
    BDX87_REPORT *rep = NULL;
    const BDX87_DIAG *diag;

    TASK_CHECK(analyze(code, sizeof(code), 0, 0x00, &rep));
    TASK_CHECK(rep->InsnCount == 2);                        // JZ and the fallthrough FLD1
    TASK_CHECK(rep->DiagCount == 1);

    diag = find_diag(rep, BDX87_DIAG_BAD_TARGET);
    TASK_CHECK(diag != NULL);
    TASK_CHECK(diag->Address == BASE + 3);
    TASK_CHECK(diag->Source == BASE + 0);

    BdX87FreeReport(rep);
    return 0;
}


//
// faults: truncated or undecodable targets are reported, including at the entry itself.
//
static int
case_bad_encoding_reported(void)
{
    static const ND_UINT8 code[] = {
        0xEB, 0x02,                 // 0: JMP 4
        0x90, 0x90,                 // 2: NOP, NOP (unreachable)
        0xD9                        // 4: truncated FLD
    };
    static const ND_UINT8 truncated[] = { 0xD9 };           // truncated entry
    BDX87_REPORT *rep = NULL;
    const BDX87_DIAG *diag;

    TASK_CHECK(analyze(code, sizeof(code), 0, 0x00, &rep));
    TASK_CHECK(rep->InsnCount == 1);
    TASK_CHECK(rep->DiagCount == 1);

    diag = find_diag(rep, BDX87_DIAG_BAD_ENCODING);
    TASK_CHECK(diag != NULL);
    TASK_CHECK(diag->Address == BASE + 4);
    TASK_CHECK(diag->Source == BASE + 0);
    BdX87FreeReport(rep);
    rep = NULL;

    TASK_CHECK(analyze(truncated, sizeof(truncated), 0, 0x00, &rep));
    TASK_CHECK(rep->InsnCount == 0);
    TASK_CHECK(rep->DiagCount == 1);
    diag = find_diag(rep, BDX87_DIAG_BAD_ENCODING);
    TASK_CHECK(diag != NULL);
    TASK_CHECK(diag->Address == BASE + 0);
    BdX87FreeReport(rep);

    return 0;
}


const struct task_case task_cases[] = {
    { "transfers", "fld_st_pushes_after_read", case_fld_st_pushes_after_read },
    { "transfers", "fstp_st_writes_then_pops", case_fstp_st_writes_then_pops },
    { "transfers", "ffree_leaves_hole", case_ffree_leaves_hole },
    { "transfers", "fincstp_rotates_then_reads", case_fincstp_rotates_then_reads },
    { "transfers", "faddp_writes_then_pops", case_faddp_writes_then_pops },
    { "transfers", "fninit_resets", case_fninit_resets },
    { "transfers", "load_constants_push", case_load_constants_push },
    { "joins", "branch_shapes_rejoin", case_branch_shapes_rejoin },
    { "joins", "loop_states_stabilize", case_loop_states_stabilize },
    { "joins", "loop_underflow_persists", case_loop_underflow_persists },
    { "joins", "outbound_edges_terminate", case_outbound_edges_terminate },
    { "faults", "read_empty_slot_underflows", case_read_empty_slot_underflows },
    { "faults", "push_onto_full_slot_overflows", case_push_onto_full_slot_overflows },
    { "faults", "unsupported_stops_path", case_unsupported_stops_path },
    { "faults", "bad_target_reported", case_bad_target_reported },
    { "faults", "bad_encoding_reported", case_bad_encoding_reported },
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
