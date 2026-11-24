// Command inspect_memory decodes a Wasm binary in inspection mode, validates it
// without instantiating or executing anything, and prints a TSV memory
// reference map derived from the production decoder/validator: memory
// declarations, raw function bodies, data segments and per-instruction memory
// references.
package main

import (
	"encoding/hex"
	"fmt"
	"os"
	"strconv"
	"strings"

	"github.com/tetratelabs/wazero/api"
	"github.com/tetratelabs/wazero/internal/wasm"
	bf "github.com/tetratelabs/wazero/internal/wasm/binary"
)

// Columns are fixed so downstream tools can address them by name; cells that
// do not apply to a record kind are left empty.
var header = strings.Join([]string{
	"kind",
	"index",
	"function_index",
	"segment_index",
	"memory_index",
	"memory_indices",
	"imported",
	"import_module",
	"import_name",
	"is64",
	"min",
	"max",
	"has_max",
	"body_offset",
	"opcode",
	"offset",
	"address_type",
	"copy_dst_type",
	"copy_src_type",
	"copy_length_type",
	"passive",
	"offset_expression",
	"init_hex",
	"body_hex",
}, "\t")

func main() {
	if len(os.Args) != 2 {
		fmt.Fprintf(os.Stderr, "usage: %s <wasm-path>\n", os.Args[0])
		os.Exit(2)
	}

	raw, err := os.ReadFile(os.Args[1])
	if err != nil {
		fmt.Fprintf(os.Stderr, "read %s: %v\n", os.Args[1], err)
		os.Exit(1)
	}

	// Inspection decoding: no linear memory is allocated, no import is called,
	// no function executes.
	m, err := bf.DecodeModuleForInspection(raw, api.CoreFeaturesV2)
	if err != nil {
		fmt.Fprintf(os.Stderr, "decode: %v\n", err)
		os.Exit(1)
	}
	if err = m.Validate(api.CoreFeaturesV2); err != nil {
		fmt.Fprintf(os.Stderr, "validate: %v\n", err)
		os.Exit(1)
	}

	var rows [][]string
	rows = append(rows, strings.Split(header, "\t"))

	for _, d := range m.MemoryDeclarations() {
		rows = append(rows, declarationRow(d))
	}
	for i := range m.CodeSection {
		rows = append(rows, functionBodyRow(m, i))
	}
	for i := range m.DataSection {
		rows = append(rows, dataSegmentRow(m, i))
	}
	for _, u := range m.MemoryUses() {
		rows = append(rows, memoryUseRow(m, u))
	}

	var b strings.Builder
	for _, row := range rows {
		b.WriteString(strings.Join(row, "\t"))
		b.WriteByte('\n')
	}
	fmt.Print(b.String())
}

func declarationRow(d wasm.MemoryDeclaration) []string {
	row := emptyRow()
	row[0] = "declaration"
	row[1] = strconv.FormatUint(uint64(d.Index), 10)
	row[6] = strconv.FormatBool(d.Imported)
	row[7] = d.ImportModule
	row[8] = d.ImportName
	row[9] = strconv.FormatBool(d.Is64)
	row[10] = strconv.FormatUint(d.Min, 10)
	row[12] = strconv.FormatBool(d.HasMax)
	if d.HasMax {
		row[11] = strconv.FormatUint(d.Max, 10)
	}
	return row
}

func functionBodyRow(m *wasm.Module, codeIndex int) []string {
	row := emptyRow()
	row[0] = "function_body"
	row[2] = strconv.FormatUint(uint64(uint32(codeIndex)+m.ImportFunctionCount), 10)
	row[23] = hex.EncodeToString(m.CodeSection[codeIndex].Body)
	return row
}

func dataSegmentRow(m *wasm.Module, segmentIndex int) []string {
	d := &m.DataSection[segmentIndex]
	row := emptyRow()
	row[0] = "data_segment"
	row[3] = strconv.Itoa(segmentIndex)
	row[20] = strconv.FormatBool(d.Passive)
	row[22] = hex.EncodeToString(d.Init)
	if !d.Passive {
		row[4] = strconv.FormatUint(uint64(d.MemoryIndex), 10)
		row[21] = offsetExpressionString(d)
	}
	return row
}

func memoryUseRow(m *wasm.Module, u wasm.MemoryUse) []string {
	row := emptyRow()
	row[0] = "memory_use"
	row[2] = strconv.FormatUint(uint64(u.FunctionIndex), 10)
	row[13] = strconv.FormatUint(uint64(u.BodyOffset), 10)
	row[14] = u.Opcode
	row[15] = strconv.FormatUint(u.Offset, 10)

	indices := make([]string, len(u.MemoryIndices))
	for i, idx := range u.MemoryIndices {
		indices[i] = strconv.FormatUint(uint64(idx), 10)
	}
	row[5] = strings.Join(indices, ",")
	if len(u.MemoryIndices) == 1 {
		row[4] = indices[0]
		if t, ok := addressTypeOf(m, u.MemoryIndices[0]); ok {
			row[16] = t
		}
	}
	if u.Opcode == "memory.copy" && len(u.MemoryIndices) == 2 {
		if t, ok := addressTypeOf(m, u.MemoryIndices[0]); ok {
			row[17] = t // destination
		}
		if t, ok := addressTypeOf(m, u.MemoryIndices[1]); ok {
			row[18] = t // source
		}
		// The length uses the narrower of the two address types.
		row[19] = "i32"
		if row[17] == "i64" && row[18] == "i64" {
			row[19] = "i64"
		}
	}
	return row
}

func addressTypeOf(m *wasm.Module, memoryIndex uint32) (string, bool) {
	for _, d := range m.MemoryDeclarations() {
		if d.Index == memoryIndex {
			if d.Is64 {
				return "i64", true
			}
			return "i32", true
		}
	}
	return "", false
}

func offsetExpressionString(d *wasm.DataSegment) string {
	expr := d.OffsetExpression
	return wasm.InstructionName(expr.Opcode) + " 0x" + hex.EncodeToString(expr.Data)
}

func emptyRow() []string {
	return make([]string, len(strings.Split(header, "\t")))
}
