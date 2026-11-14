package macho

import "fmt"

// MaxBindings is the maximum number of Binding values a single call to
// ReadBindings will produce. Binding streams may expand one opcode into up
// to a ULEB128 count of binds; this bound keeps that expansion finite.
// Streams that would exceed it fail with a *BindingError.
const MaxBindings = 1 << 20

// Bind type values used by the classic dyld binding opcodes.
const (
	BindTypePointer        uint8 = 1
	BindTypeTextAbsolute32 uint8 = 2
	BindTypeTextPCRel32    uint8 = 3
)

// Additional classic binding opcodes not used by the writer.
const (
	bindOpcodeSetDylibOrdinalULEB     = 0x20
	bindOpcodeSetDylibSpecialImm      = 0x30
	bindOpcodeSetAddendSLEB           = 0x60
	bindOpcodeAddAddrULEB             = 0x80
	bindOpcodeDoBindAddAddrULEB       = 0xA0
	bindOpcodeDoBindAddAddrImmScaled  = 0xB0
	bindOpcodeDoBindULEBTimesSkipping = 0xC0
	bindOpcodeThreaded                = 0xD0
)

// A Binding describes one bind operation decoded from a classic dyld
// binding stream. Values are reported in the order the DO_BIND opcodes
// occur; identical target addresses are never merged.
//
// All offsets are byte offsets. SegmentOffset is measured from the start of
// the selected segment, VirtualAddress is the absolute virtual-memory
// address, and FileOffset is the absolute offset in the Mach-O file.
type Binding struct {
	// Kind is the stream this binding was decoded from (BindNormal,
	// BindWeak or BindLazy).
	Kind BindKind
	// StreamOffset is the byte offset in the binding stream of the opcode
	// that triggered this bind. Repeated binds produced by a single opcode
	// share this offset.
	StreamOffset int
	// Symbol is the symbol name selected by the most recent
	// SET_SYMBOL_TRAILING_FLAGS_IMM opcode.
	Symbol string
	// Flags is the immediate from SET_SYMBOL_TRAILING_FLAGS_IMM.
	Flags uint8
	// DylibOrdinal is the signed dylib ordinal. Special ordinals are
	// sign-extended from the low four bits, so they may be negative.
	DylibOrdinal int
	// Addend is the signed addend set by SET_ADDEND_SLEB, in bytes.
	Addend int64
	// Type is the bind type: BindTypePointer, BindTypeTextAbsolute32 or
	// BindTypeTextPCRel32 (or an uninterpreted opcode immediate).
	Type uint8
	// SegmentIndex is the index of the segment among *Segment load
	// commands in Loads, including __PAGEZERO. Segment names are not
	// unique and are not used as keys.
	SegmentIndex int
	// SegmentName is the name of the selected segment.
	SegmentName string
	// SegmentOffset is the byte offset within the segment.
	SegmentOffset uint64
	// VirtualAddress is Segment.Addr + SegmentOffset.
	VirtualAddress uint64
	// HasFileBytes reports whether the complete bound field lies within
	// the segment's Filesz. Zero-filled memory past Filesz is a valid
	// virtual reference but has no file bytes.
	HasFileBytes bool
	// FileOffset is Segment.Offset + SegmentOffset. It is meaningful only
	// when HasFileBytes is true.
	FileOffset uint64
}

// A BindingError describes a malformed or unsupported binding stream.
// Kind identifies the stream and Offset is the byte offset in that stream
// at which the problem was detected.
type BindingError struct {
	Kind    BindKind
	Offset  int
	Message string
}

func (e *BindingError) Error() string {
	return fmt.Sprintf("macho: %s binding stream offset %d: %s", e.Kind, e.Offset, e.Message)
}

// String returns the stream name used in error messages and documentation.
func (k BindKind) String() string {
	switch k {
	case BindNormal:
		return "normal"
	case BindWeak:
		return "weak"
	case BindLazy:
		return "lazy"
	}
	return fmt.Sprintf("BindKind(%d)", uint8(k))
}

// ReadBindings decodes the classic dyld binding opcodes in the stream
// selected by kind: BindNormal (BindingInfoDat), BindWeak (WeakBindingDat)
// or BindLazy (LazyBindingDat). It only interprets bytes already read into
// File.DylinkInfo; it never loads libraries, executes code, or applies a
// binding.
//
// DONE ends a normal or weak stream; any remaining bytes must be zero. In a
// lazy stream DONE ends one record and resets all decoder state, so a
// following record starts from the initial state. Consecutive zero bytes in
// a lazy stream are empty records.
//
// A stream whose declared length does not match its byte slice, an unknown
// opcode (including threaded binding), an out-of-range segment, an
// overflowing value, or a field outside the segment's Memsz is reported as
// a *BindingError. No partial result is returned in that case and the File
// and its byte slices are not modified.
func (f *File) ReadBindings(kind BindKind) ([]Binding, error) {
	if kind != BindNormal && kind != BindWeak && kind != BindLazy {
		return nil, fmt.Errorf("macho: ReadBindings: invalid bind kind %d", uint8(kind))
	}
	if f.DylinkInfo == nil {
		return nil, nil
	}

	var data []byte
	var declared uint32
	switch kind {
	case BindNormal:
		data, declared = f.DylinkInfo.BindingInfoDat, f.DylinkInfo.BindingInfoLen
	case BindWeak:
		data, declared = f.DylinkInfo.WeakBindingDat, f.DylinkInfo.WeakBindingLen
	case BindLazy:
		data, declared = f.DylinkInfo.LazyBindingDat, f.DylinkInfo.LazyBindingLen
	}
	if uint64(len(data)) != uint64(declared) {
		return nil, &BindingError{
			Kind:    kind,
			Offset:  0,
			Message: fmt.Sprintf("declared length %d does not match stream length %d", declared, len(data)),
		}
	}
	if len(data) == 0 {
		return nil, nil
	}

	var segments []*Segment
	for _, load := range f.Loads {
		if seg, ok := load.(*Segment); ok && seg != nil {
			segments = append(segments, seg)
		}
	}

	ptrSize := uint64(4)
	if f.Magic == Magic64 {
		ptrSize = 8
	}

	var out []Binding
	st := bindingState{typ: BindTypePointer}
	pos := 0

	emit := func(trigger int) error {
		if !st.hasSegment {
			return &BindingError{Kind: kind, Offset: trigger, Message: "DO_BIND before segment selection"}
		}
		if st.segIndex < 0 || st.segIndex >= len(segments) {
			return &BindingError{Kind: kind, Offset: trigger, Message: fmt.Sprintf("segment index %d out of range", st.segIndex)}
		}
		seg := segments[st.segIndex]
		fieldSize := bindingFieldSize(st.typ, ptrSize)
		if st.segOffset > seg.Memsz || fieldSize > seg.Memsz-st.segOffset {
			return &BindingError{
				Kind:   kind,
				Offset: trigger,
				Message: fmt.Sprintf("bind field at segment %d offset 0x%x size %d exceeds Memsz 0x%x",
					st.segIndex, st.segOffset, fieldSize, seg.Memsz),
			}
		}
		va, ok := addUint64(seg.Addr, st.segOffset)
		if !ok {
			return &BindingError{Kind: kind, Offset: trigger, Message: "virtual address overflow"}
		}
		if len(out) >= MaxBindings {
			return &BindingError{Kind: kind, Offset: trigger, Message: fmt.Sprintf("binding expansion limit %d exceeded", MaxBindings)}
		}
		b := Binding{
			Kind:           kind,
			StreamOffset:   trigger,
			Symbol:         st.symbol,
			Flags:          st.flags,
			DylibOrdinal:   st.ordinal,
			Addend:         st.addend,
			Type:           st.typ,
			SegmentIndex:   st.segIndex,
			SegmentName:    seg.Name,
			SegmentOffset:  st.segOffset,
			VirtualAddress: va,
		}
		if st.segOffset <= seg.Filesz && fieldSize <= seg.Filesz-st.segOffset {
			if fileOff, ok := addUint64(seg.Offset, st.segOffset); ok && fileOff <= ^uint64(0)-fieldSize {
				b.HasFileBytes = true
				b.FileOffset = fileOff
			}
		}
		out = append(out, b)
		return nil
	}

	addOffset := func(trigger int, delta uint64) error {
		if delta > ^uint64(0)-st.segOffset {
			return &BindingError{Kind: kind, Offset: trigger, Message: "segment offset overflow"}
		}
		st.segOffset += delta
		return nil
	}

	for pos < len(data) {
		opcodePos := pos
		b := data[pos]
		pos++
		op := b & 0xF0
		imm := b & 0x0F

		switch op {
		case bindOpcodeDone:
			if kind == BindLazy {
				// Ends one record; the next record starts from scratch.
				st = bindingState{typ: BindTypePointer}
				continue
			}
			for i := pos; i < len(data); i++ {
				if data[i] != 0 {
					return nil, &BindingError{Kind: kind, Offset: i, Message: "non-zero byte after DONE"}
				}
			}
			pos = len(data)

		case bindOpcodeSetDylibOrdinalImm:
			st.ordinal = int(imm)

		case bindOpcodeSetDylibOrdinalULEB:
			v, err := readULEB128(data, &pos)
			if err != nil {
				return nil, &BindingError{Kind: kind, Offset: opcodePos, Message: "dylib ordinal: " + err.Error()}
			}
			if v > uint64(maxInt) {
				return nil, &BindingError{Kind: kind, Offset: opcodePos, Message: "dylib ordinal overflow"}
			}
			st.ordinal = int(v)

		case bindOpcodeSetDylibSpecialImm:
			// Sign-extend the low four bits: 0 stays 0, 8..15 become
			// -8..-1.
			v := int(imm)
			if v >= 8 {
				v -= 16
			}
			st.ordinal = v

		case bindOpcodeSetSymbolTrailingFlags:
			st.flags = imm
			n := -1
			for i := pos; i < len(data); i++ {
				if data[i] == 0 {
					n = i
					break
				}
			}
			if n < 0 {
				return nil, &BindingError{Kind: kind, Offset: opcodePos, Message: "unterminated symbol name"}
			}
			st.symbol = string(data[pos:n])
			pos = n + 1

		case bindOpcodeSetTypeImm:
			st.typ = imm

		case bindOpcodeSetAddendSLEB:
			v, err := readSLEB128(data, &pos)
			if err != nil {
				return nil, &BindingError{Kind: kind, Offset: opcodePos, Message: "addend: " + err.Error()}
			}
			st.addend = v

		case bindOpcodeSetSegmentAndOffsetULEB:
			segIndex := int(imm)
			if segIndex >= len(segments) {
				return nil, &BindingError{
					Kind:    kind,
					Offset:  opcodePos,
					Message: fmt.Sprintf("segment index %d out of range (%d segments)", segIndex, len(segments)),
				}
			}
			v, err := readULEB128(data, &pos)
			if err != nil {
				return nil, &BindingError{Kind: kind, Offset: opcodePos, Message: "segment offset: " + err.Error()}
			}
			st.segIndex = segIndex
			st.segOffset = v
			st.hasSegment = true

		case bindOpcodeAddAddrULEB:
			v, err := readULEB128(data, &pos)
			if err != nil {
				return nil, &BindingError{Kind: kind, Offset: opcodePos, Message: "address increment: " + err.Error()}
			}
			if err := addOffset(opcodePos, v); err != nil {
				return nil, err
			}

		case bindOpcodeDoBind:
			if err := emit(opcodePos); err != nil {
				return nil, err
			}
			if err := addOffset(opcodePos, ptrSize); err != nil {
				return nil, err
			}

		case bindOpcodeDoBindAddAddrULEB:
			v, err := readULEB128(data, &pos)
			if err != nil {
				return nil, &BindingError{Kind: kind, Offset: opcodePos, Message: "address increment: " + err.Error()}
			}
			if err := emit(opcodePos); err != nil {
				return nil, err
			}
			if v > ^uint64(0)-ptrSize {
				return nil, &BindingError{Kind: kind, Offset: opcodePos, Message: "segment offset overflow"}
			}
			if err := addOffset(opcodePos, v+ptrSize); err != nil {
				return nil, err
			}

		case bindOpcodeDoBindAddAddrImmScaled:
			if err := emit(opcodePos); err != nil {
				return nil, err
			}
			// The scale is always the pointer width, never the field
			// width of text_abs32/text_pcrel32.
			delta := uint64(imm)*ptrSize + ptrSize
			if err := addOffset(opcodePos, delta); err != nil {
				return nil, err
			}

		case bindOpcodeDoBindULEBTimesSkipping:
			count, err := readULEB128(data, &pos)
			if err != nil {
				return nil, &BindingError{Kind: kind, Offset: opcodePos, Message: "repeat count: " + err.Error()}
			}
			skip, err := readULEB128(data, &pos)
			if err != nil {
				return nil, &BindingError{Kind: kind, Offset: opcodePos, Message: "repeat skip: " + err.Error()}
			}
			if count > uint64(MaxBindings-len(out)) {
				return nil, &BindingError{Kind: kind, Offset: opcodePos, Message: fmt.Sprintf("binding expansion limit %d exceeded", MaxBindings)}
			}
			if skip > ^uint64(0)-ptrSize {
				return nil, &BindingError{Kind: kind, Offset: opcodePos, Message: "segment offset overflow"}
			}
			delta := skip + ptrSize
			for i := uint64(0); i < count; i++ {
				if err := emit(opcodePos); err != nil {
					return nil, err
				}
				if err := addOffset(opcodePos, delta); err != nil {
					return nil, err
				}
			}

		case bindOpcodeThreaded:
			return nil, &BindingError{Kind: kind, Offset: opcodePos, Message: fmt.Sprintf("unsupported opcode 0x%02x: threaded binding is not supported", b)}

		default:
			return nil, &BindingError{Kind: kind, Offset: opcodePos, Message: fmt.Sprintf("unsupported opcode 0x%02x", b)}
		}
	}

	return out, nil
}

type bindingState struct {
	ordinal    int
	symbol     string
	flags      uint8
	typ        uint8
	addend     int64
	segIndex   int
	segOffset  uint64
	hasSegment bool
}

func bindingFieldSize(typ uint8, ptrSize uint64) uint64 {
	switch typ {
	case BindTypeTextAbsolute32, BindTypeTextPCRel32:
		return 4
	default:
		return ptrSize
	}
}

func addUint64(a, b uint64) (uint64, bool) {
	if b > ^uint64(0)-a {
		return 0, false
	}
	return a + b, true
}

const maxInt = int(^uint(0) >> 1)

// readULEB128 reads an unsigned LEB128 value, advancing *pos past it.
// Encodings that do not fit in a uint64 are rejected.
func readULEB128(data []byte, pos *int) (uint64, error) {
	var result uint64
	var shift uint
	for {
		if *pos >= len(data) {
			return 0, fmt.Errorf("truncated ULEB128")
		}
		b := data[*pos]
		*pos++
		if shift >= 64 {
			if b&0x80 != 0 || b&0x7f != 0 {
				return 0, fmt.Errorf("ULEB128 overflow")
			}
			return result, nil
		}
		if shift == 63 {
			// Only the top value bit remains.
			if b&0x80 != 0 || b&0x7f > 1 {
				return 0, fmt.Errorf("ULEB128 overflow")
			}
			return result | uint64(b&0x7f)<<63, nil
		}
		result |= uint64(b&0x7f) << shift
		shift += 7
		if b&0x80 == 0 {
			return result, nil
		}
	}
}

// readSLEB128 reads a signed LEB128 value, advancing *pos past it.
// Encodings that do not fit in an int64 are rejected.
func readSLEB128(data []byte, pos *int) (int64, error) {
	var result int64
	var shift uint
	for {
		if *pos >= len(data) {
			return 0, fmt.Errorf("truncated SLEB128")
		}
		b := data[*pos]
		*pos++
		if shift >= 64 {
			// Only a canonical sign-extension terminator may remain.
			if b&0x80 != 0 || (b != 0x00 && b != 0x7f) {
				return 0, fmt.Errorf("SLEB128 overflow")
			}
			return result, nil
		}
		if shift == 63 {
			// Only the top value bit remains. A canonical terminator is
			// 0x00 for a non-negative value or 0x7f for a negative one.
			if b&0x80 != 0 || (b != 0x00 && b != 0x7f) {
				return 0, fmt.Errorf("SLEB128 overflow")
			}
			if b == 0x7f {
				result |= -1 << 63
			}
			return result, nil
		}
		result |= int64(b&0x7f) << shift
		shift += 7
		if b&0x80 == 0 {
			if b&0x40 != 0 {
				result |= -1 << shift
			}
			return result, nil
		}
	}
}
