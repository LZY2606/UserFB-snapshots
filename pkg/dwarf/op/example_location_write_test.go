package op_test

import (
	"encoding/binary"
	"fmt"

	"github.com/go-delve/delve/pkg/dwarf/op"
)

// ExamplePlanLocationWrite shows an offline bit write to one register. The
// program is never started: Plan reads the snapshot while preparing, and
// Commit sends the complete register image through ChangeFunc.
func ExamplePlanLocationWrite() {
	initial := op.DwarfRegisterFromUint64(0x1122334455667788)
	regs := *op.NewDwarfRegisters(0, []*op.DwarfRegister{initial}, binary.LittleEndian, 0, 0, 0, 0)

	// DW_OP_reg0 DW_OP_bit_piece <16 bits> <source bit offset 0>.
	instructions := []byte{byte(op.DW_OP_reg0), byte(op.DW_OP_bit_piece), 16, 0}

	// Replace logical bits 0..15 with 0x1234; all sixteen mask bits are set.
	data := []byte{0x34, 0x12}
	mask := []byte{0xff, 0xff}

	plan, err := op.PlanLocationWrite(regs, instructions, 8, 16, data, mask, nil)
	if err != nil {
		panic(err)
	}

	records := plan.RegisterWrites()
	fmt.Printf("records=%d data=% x\n", len(records), records[0].Data)

	current := regs
	current.ChangeFunc = func(regNum uint64, reg *op.DwarfRegister) error {
		fmt.Printf("change reg=%d bytes=% x\n", regNum, reg.Bytes)
		return nil
	}
	result, err := plan.Commit(current, nil, nil)
	fmt.Printf("commit err=%v completed=%v\n", err, result.CompletedRecords)

	// Output:
	// records=1 data=34 12 66 55 44 33 22 11
	// change reg=0 bytes=34 12 66 55 44 33 22 11
	// commit err=<nil> completed=[0]
}
