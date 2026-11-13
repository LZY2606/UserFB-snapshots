package main

import (
    "encoding/binary"
    "fmt"
    "os"

    "github.com/go-delve/delve/pkg/dwarf/op"
)

func main() {
    regs := op.NewDwarfRegisters(0, []*op.DwarfRegister{op.DwarfRegisterFromUint64(0x1234)}, binary.LittleEndian, 0, 0, 0, 0)
    expr := []byte{byte(op.DW_OP_reg0), byte(op.DW_OP_piece), 1, byte(op.DW_OP_addr), 0, 0x10, 0, 0, 0, 0, 0, 0, byte(op.DW_OP_piece), 1}
    _, pieces, err := op.ExecuteStackProgram(*regs, expr, 8, nil)
    if err != nil || len(pieces) != 2 || pieces[0].Kind != op.RegPiece || pieces[0].Val != 0 || pieces[0].Size != 1 || pieces[1].Kind != op.AddrPiece || pieces[1].Val != 0x1000 || pieces[1].Size != 1 {
        fmt.Fprintln(os.Stderr, "existing mixed piece path:", pieces, err)
        os.Exit(1)
    }
    fmt.Printf("DWARF_BYTE_PIECES reg=%d bytes=%d memory=%#x bytes=%d\n", pieces[0].Val, pieces[0].Size, pieces[1].Val, pieces[1].Size)
    _, gap, err := op.ExecuteStackProgram(*regs, []byte{byte(op.DW_OP_piece), 1}, 8, nil)
    if err != nil || len(gap) != 1 || gap[0].Kind != op.ImmPiece || gap[0].Val != 0 {
        fmt.Fprintln(os.Stderr, "existing unavailable piece path:", gap, err)
        os.Exit(1)
    }
    fmt.Println("DWARF_LEGACY_GAP kind=immediate value=0")
    _, _, err = op.ExecuteStackProgram(*regs, []byte{byte(op.DW_OP_reg0), byte(op.DW_OP_bit_piece), 3, 1}, 8, nil)
    if err == nil { fmt.Fprintln(os.Stderr, "expected unsupported bit piece"); os.Exit(1) }
    fmt.Println("DWARF_LEGACY_BIT_PIECE_REJECTED")
    addr, _, err := op.ExecuteStackProgram(*regs, []byte{byte(op.DW_OP_addr), 0, 0x10, 0, 0, 0, 0, 0, 0, byte(op.DW_OP_deref)}, 8, func(buf []byte, addr uint64)(int,error) {
        if addr != 0x1000 || len(buf) != 8 { return 0, fmt.Errorf("unexpected deref %#x/%d",addr,len(buf)) }
        binary.LittleEndian.PutUint64(buf,0x2000)
        return 8,nil
    })
    if err != nil || addr != 0x2000 { fmt.Fprintln(os.Stderr,"existing deref path:",addr,err);os.Exit(1) }
    fmt.Println("DWARF_DEREF_ADDRESS=0x2000")
    fmt.Println("DWARF_WRITE_STARTING_POINT_OK")
}
