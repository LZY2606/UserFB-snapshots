package wasm

// Inspection-only analysis types. These describe the static memory reference
// graph recovered by decoding and validating a module without instantiating it,
// allocating any linear memory, calling imports or executing functions.

// MemoryDeclaration is one entry of the combined memory index space: imported
// memories in import order followed by the memories defined in the memory
// section. Page counts and the 32/64-bit address width are preserved exactly as
// encoded in the binary.
type MemoryDeclaration struct {
	// Index is the position in the combined memory index space (imports first).
	Index uint32
	// Imported reports whether this memory comes from the import section.
	Imported bool
	// Is64 reports whether this memory uses the 64-bit address type (memory64,
	// limits flag 0x04/0x05). False means the 32-bit address type.
	Is64 bool
	// Min is the encoded minimum number of 64KiB pages.
	Min uint64
	// Max is the encoded maximum number of 64KiB pages. It is zero unless
	// HasMax is true, in which case it is the value encoded in the binary.
	Max uint64
	// HasMax reports whether a maximum was encoded in the limits type.
	HasMax bool
	// ImportModule and ImportName identify the import; both are empty for a
	// memory defined in this module.
	ImportModule string
	ImportName   string
}

// MemoryUse records one memory-referencing instruction found while the
// production function validator walked the function body. Nothing here is
// derived from a separate scanner: every field comes from the validator's
// decoding cursor.
type MemoryUse struct {
	// FunctionIndex is the function in the combined function index space, i.e.
	// including the imported function prefix.
	FunctionIndex uint32
	// BodyOffset is the byte offset of the instruction's first opcode byte in
	// Code.Body (which starts at the first body opcode).
	BodyOffset uint32
	// Opcode is the instruction name, e.g. "i32.load" or "memory.copy".
	Opcode string
	// MemoryIndices holds the referenced memory indices in encoding order.
	// memory.copy carries two (destination, then source); every other
	// memory-referencing instruction carries one.
	MemoryIndices []uint32
	// Offset is the static memarg offset read as an unsigned LEB128 value. It
	// is zero for instructions without a memarg (memory.size, memory.grow,
	// memory.init, memory.copy, memory.fill).
	Offset uint64
}

// inspectionState carries the data used only while validating a module decoded
// via DecodeModuleForInspection. A nil pointer means the module is on the
// ordinary decode/execution path and validation keeps its historical behavior.
type inspectionState struct {
	declarations []MemoryDeclaration
	uses         []MemoryUse
	// validated is set once Validate has completed successfully, so validating
	// the same Module again does not append duplicate MemoryUse records.
	validated bool
}

// MemoryDeclarations returns the combined memory index space (imports first,
// then locally defined memories). It is nil for modules decoded with the
// ordinary DecodeModule entry point.
func (m *Module) MemoryDeclarations() []MemoryDeclaration {
	if m.inspection == nil {
		return nil
	}
	return m.inspection.declarations
}

// MemoryUses returns the memory-referencing instructions recorded by the
// production function validator. The slice is sorted by function index and
// then body offset. It is nil for modules decoded with the ordinary
// DecodeModule entry point.
func (m *Module) MemoryUses() []MemoryUse {
	if m.inspection == nil {
		return nil
	}
	return m.inspection.uses
}

// addressTypeName returns "i32" or "i64" for the address type of the memory at
// the given combined index, or an error string fragment when the index is out
// of range.
func (m *Module) addressTypeName(memoryIndex uint32) (string, bool) {
	if m.inspection == nil || memoryIndex >= uint32(len(m.inspection.declarations)) {
		return "", false
	}
	if m.inspection.declarations[memoryIndex].Is64 {
		return "i64", true
	}
	return "i32", true
}

// SetInspectionMode marks a freshly decoded module as an inspection object.
func (m *Module) SetInspectionMode() {
	m.inspection = &inspectionState{}
}

// IsInspection reports whether the module was decoded for inspection rather
// than execution.
func (m *Module) IsInspection() bool {
	return m.inspection != nil
}

// AppendInspectionMemoryDeclarations appends decoded memory declarations,
// re-indexing them into the combined memory index space. Imported memories are
// appended first (their decoded indices already match the combined space), and
// locally defined memories continue after the imported prefix.
func (m *Module) AppendInspectionMemoryDeclarations(declarations []MemoryDeclaration) {
	if m.inspection == nil || len(declarations) == 0 {
		return
	}
	base := uint32(len(m.inspection.declarations))
	for i := range declarations {
		declarations[i].Index = base + uint32(i)
		m.inspection.declarations = append(m.inspection.declarations, declarations[i])
	}
}

// appendMemoryUse records one memory-referencing instruction from the
// validator cursor.
func (m *Module) appendMemoryUse(use MemoryUse) {
	if m.inspection == nil {
		return
	}
	m.inspection.uses = append(m.inspection.uses, use)
}

// resetMemoryUses drops partial records so a failed validation never leaves a
// seemingly complete reference graph.
func (m *Module) resetMemoryUses() {
	if m.inspection != nil {
		m.inspection.uses = nil
	}
}

func (m *Module) markInspectionValidated() {
	if m.inspection != nil {
		m.inspection.validated = true
	}
}

func (m *Module) inspectionValidated() bool {
	return m.inspection != nil && m.inspection.validated
}
