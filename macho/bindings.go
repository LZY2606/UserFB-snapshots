package macho

import (
	"fmt"
)

// MaxBindingExpansions is the maximum number of Binding records that a single
// ReadBindings call will produce. Repeated-binding opcodes (for example
// BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB) can expand one instruction into
// a large number of records; a stream that would exceed this limit is rejected
// with an error instead of being expanded.
const MaxBindingExpansions = 1 << 20

// Binding stream opcodes not already declared in reloc_write.go.
const (
	bindOpcodeSetDylibOrdinalULEB         = 0x20
	bindOpcodeSetDylibSpecialImm          = 0x30
	bindOpcodeSetAddendSLEB               = 0x60
	bindOpcodeAddAddrULEB                 = 0x80
	bindOpcodeDoBindAddAddrULEB           = 0xa0
	bindOpcodeDoBindAddAddrImmScaled      = 0xb0
	bindOpcodeDoBindULEBTimesSkippingULEB = 0xc0
	bindOpcodeThreaded                    = 0xd0
)

// Binding describes one dyld bind operation decoded from a Mach-O binding
// stream. A Binding is informational only: ReadBindings never loads a dylib,
// executes machine code, or applies the bind to file contents.
//
// Field units:
//   - StreamOffset is a byte offset into the raw binding stream
//     (File.DylinkInfo.BindingInfoDat / WeakBindingDat / LazyBindingDat).
//   - SegmentOffset is a byte offset from the start of the selected segment's
//     virtual address (segment.Addr).
//   - Address is a virtual address (segment.Addr + SegmentOffset).
//   - FileOffset is a byte offset into the Mach-O file, and is only meaningful
//     when HasFileBytes is true.
//   - Flags are the low four bits of the symbol-trailing-flags immediate.
//   - Ordinal is the signed dylib ordinal; special ordinals are sign-extended
//     from four bits (so -1, -2, ... are possible). Zero means the default
//     lookup.
//   - Addend is a signed 64-bit value.
type Binding struct {
	// StreamOffset is the byte offset of the triggering DO_BIND-family opcode
	// within the raw binding stream. Repeated binds from one opcode share the
	// same StreamOffset.
	StreamOffset uint64

	// Symbol is the symbol name selected by SET_SYMBOL_TRAILING_FLAGS_IMM.
	Symbol string

	// Flags is the low four bits of the symbol-trailing-flags immediate.
	Flags uint8

	// Ordinal is the signed dylib ordinal.
	Ordinal int16

	// Addend is the signed addend.
	Addend int64

	// Type is the bind type (1 = pointer, 2 = text_abs32,
	// 3 = text_pcrel32, 4 = text_abs64).
	Type uint8

	// SegmentIndex is the index of the selected segment among the Segment
	// load commands in File.Loads order, counting every segment including
	// __PAGEZERO. It is always >= 0 for a reported Binding.
	SegmentIndex int

	// SegmentOffset is the byte offset within the selected segment.
	SegmentOffset uint64

	// Address is the virtual address referenced by the binding.
	Address uint64

	// HasFileBytes reports whether the whole bound field lies within the
	// selected segment's Filesz, so that FileOffset points at on-disk bytes.
	// Bindings in a segment's zero-filled tail are valid virtual references
	// but have HasFileBytes false.
	HasFileBytes bool

	// FileOffset is the offset in the Mach-O file of the bound field. It is
	// set only when HasFileBytes is true.
	FileOffset uint64
}

func bindKindName(kind BindKind) string {
	switch kind {
	case BindNormal:
		return "normal"
	case BindWeak:
		return "weak"
	case BindLazy:
		return "lazy"
	default:
		return fmt.Sprintf("unknown(%d)", uint8(kind))
	}
}

// ReadBindings decodes the classic dyld binding opcodes in the stream selected
// by kind: BindNormal, BindWeak, or BindLazy.
//
// If File.DylinkInfo is nil, or the selected stream has zero length, the
// result is empty. An invalid kind, or a stream whose declared length in
// DylinkInfo differs from the length of the corresponding byte slice, is an
// error. ReadBindings never modifies the file or the input bytes; on error no
// partial list is returned.
//
// The decoder understands the classic (non-threaded) binding opcodes:
// ordinal immediate/ULEB/special, symbol trailing flags, type, SLEB addend,
// segment+ULEB offset, ADD_ADDR_ULEB, and the four DO_BIND-family opcodes.
// BIND_OPCODE_THREADED and unknown opcodes are reported as unsupported.
//
// In the normal and weak streams, DONE ends the stream and only zero padding
// may follow. In the lazy stream, DONE ends a single record and only resets
// the decoder state, so later records are decoded independently; runs of zero
// bytes are treated as empty records. Reaching the end of any stream without a
// DONE is accepted.
//
// Expansion of repeated bindings is capped at MaxBindingExpansions records.
func (f *File) ReadBindings(kind BindKind) ([]Binding, error) {
	var (
		dat      []byte
		declared uint32
	)
	switch kind {
	case BindNormal:
		if f.DylinkInfo == nil {
			return nil, nil
		}
		dat = f.DylinkInfo.BindingInfoDat
		declared = f.DylinkInfo.BindingInfoLen
	case BindWeak:
		if f.DylinkInfo == nil {
			return nil, nil
		}
		dat = f.DylinkInfo.WeakBindingDat
		declared = f.DylinkInfo.WeakBindingLen
	case BindLazy:
		if f.DylinkInfo == nil {
			return nil, nil
		}
		dat = f.DylinkInfo.LazyBindingDat
		declared = f.DylinkInfo.LazyBindingLen
	default:
		return nil, fmt.Errorf("macho: invalid bind kind %d", uint8(kind))
	}
	if uint64(declared) != uint64(len(dat)) {
		return nil, fmt.Errorf("macho: %s binding stream: declared length %d does not match stream length %d at stream offset 0x0",
			bindKindName(kind), declared, len(dat))
	}
	if len(dat) == 0 {
		return nil, nil
	}

	ptrSize := uint64(4)
	switch f.Magic {
	case Magic32:
	case Magic64:
		ptrSize = 8
	default:
		return nil, fmt.Errorf("macho: %s binding stream: unsupported magic %#x at stream offset 0x0",
			bindKindName(kind), f.Magic)
	}

	var segments []*Segment
	for _, load := range f.Loads {
		if seg, ok := load.(*Segment); ok {
			segments = append(segments, seg)
		}
	}

	p := &bindingParser{
		kind:     kind,
		dat:      dat,
		lazy:     kind == BindLazy,
		segments: segments,
		ptrSize:  ptrSize,
		state:    newBindingState(),
	}
	return p.parse()
}

// bindingState is the decoder state carried between binding opcodes.
type bindingState struct {
	ordinal       int16
	symbol        string
	flags         uint8
	typ           uint8
	addend        int64
	hasSegment    bool
	segmentIndex  int
	segmentOffset uint64
}

func newBindingState() bindingState {
	return bindingState{typ: bindTypePointer, segmentIndex: -1}
}

type bindingParser struct {
	kind     BindKind
	dat      []byte
	lazy     bool
	segments []*Segment
	ptrSize  uint64
	state    bindingState
	out      []Binding
}

func (p *bindingParser) fail(off int, format string, args ...interface{}) error {
	return fmt.Errorf("macho: %s binding stream: %s at stream offset %#x",
		bindKindName(p.kind), fmt.Sprintf(format, args...), off)
}

// fieldSize returns the number of bytes occupied by the bound field. Pointer
// and text_abs64 fields are pointer sized; text_abs32 and text_pcrel32 fields
// are four bytes even on 64-bit files. The post-bind stride is always the
// pointer size and does not depend on the field size.
func (p *bindingParser) fieldSize() uint64 {
	switch p.state.typ {
	case 2, 3: // text_abs32, text_pcrel32
		return 4
	default:
		return p.ptrSize
	}
}

func (p *bindingParser) advance(opOff int, delta uint64) error {
	next, ok := addUint64(p.state.segmentOffset, delta)
	if !ok {
		return p.fail(opOff, "segment offset overflow")
	}
	p.state.segmentOffset = next
	return nil
}

// emit appends one Binding using the state before the triggering opcode
// advances the location.
func (p *bindingParser) emit(opOff int) error {
	if !p.state.hasSegment {
		return p.fail(opOff, "DO_BIND with no segment selected")
	}
	si := p.state.segmentIndex
	if si < 0 || si >= len(p.segments) {
		return p.fail(opOff, "segment index %d out of range (%d segments)", si, len(p.segments))
	}
	seg := p.segments[si]
	off := p.state.segmentOffset
	fieldSize := p.fieldSize()
	if off > seg.Memsz || fieldSize > seg.Memsz-off {
		return p.fail(opOff, "binding field at segment %d offset %d (size %d) exceeds segment memory size %d",
			si, off, fieldSize, seg.Memsz)
	}
	addr, ok := addUint64(seg.Addr, off)
	if !ok {
		return p.fail(opOff, "virtual address overflow in segment %d", si)
	}

	b := Binding{
		StreamOffset:  uint64(opOff),
		Symbol:        p.state.symbol,
		Flags:         p.state.flags,
		Ordinal:       p.state.ordinal,
		Addend:        p.state.addend,
		Type:          p.state.typ,
		SegmentIndex:  si,
		SegmentOffset: off,
		Address:       addr,
	}
	// Only mark file bytes when the complete field lies inside Filesz. The
	// zero-filled tail and fields crossing the file end remain virtual-only.
	if off <= seg.Filesz && fieldSize <= seg.Filesz-off {
		fileOff, ok := addUint64(seg.Offset, off)
		if !ok {
			return p.fail(opOff, "file offset overflow in segment %d", si)
		}
		b.HasFileBytes = true
		b.FileOffset = fileOff
	}

	if len(p.out) >= MaxBindingExpansions {
		return p.fail(opOff, "binding expansion exceeds limit %d", MaxBindingExpansions)
	}
	p.out = append(p.out, b)
	return nil
}

func (p *bindingParser) uleb(off int) (uint64, int, error) {
	v, n, err := readULEB128(p.dat, off)
	if err != nil {
		return 0, 0, p.fail(off, "%v", err)
	}
	return v, n, nil
}

func (p *bindingParser) sleb(off int) (int64, int, error) {
	v, n, err := readSLEB128(p.dat, off)
	if err != nil {
		return 0, 0, p.fail(off, "%v", err)
	}
	return v, n, nil
}

func (p *bindingParser) parse() ([]Binding, error) {
	dat := p.dat
	off := 0
	for off < len(dat) {
		opOff := off
		op := dat[off]
		off++

		switch op & 0xf0 {
		case bindOpcodeDone: // 0x00
			if op != bindOpcodeDone {
				return nil, p.fail(opOff, "unsupported binding opcode %#02x", op)
			}
			if p.lazy {
				// In lazy streams DONE terminates one record and resets all
				// state; the next byte starts a fresh record.
				p.state = newBindingState()
				continue
			}
			// In normal/weak streams DONE ends the whole stream. Only zero
			// padding may follow.
			for i := off; i < len(dat); i++ {
				if dat[i] != 0 {
					return nil, p.fail(i, "non-zero byte after DONE opcode")
				}
			}
			return p.out, nil

		case bindOpcodeSetDylibOrdinalImm: // 0x10 | imm
			p.state.ordinal = int16(op & 0x0f)

		case bindOpcodeSetDylibOrdinalULEB: // 0x20
			if op != bindOpcodeSetDylibOrdinalULEB {
				return nil, p.fail(opOff, "unsupported binding opcode %#02x", op)
			}
			v, n, err := p.uleb(off)
			if err != nil {
				return nil, err
			}
			off += n
			if v > 0x7fff {
				return nil, p.fail(opOff, "dylib ordinal %d out of range", v)
			}
			p.state.ordinal = int16(v)

		case bindOpcodeSetDylibSpecialImm: // 0x30 | imm
			imm := op & 0x0f
			if imm == 0 {
				p.state.ordinal = 0
			} else {
				// Sign-extend the low four bits.
				p.state.ordinal = int16(int8(imm<<4) >> 4)
			}

		case bindOpcodeSetSymbolTrailingFlags: // 0x40 | flags
			p.state.flags = op & 0x0f
			start := off
			end := -1
			for i := start; i < len(dat); i++ {
				if dat[i] == 0 {
					end = i
					break
				}
			}
			if end < 0 {
				return nil, p.fail(opOff, "unterminated symbol string")
			}
			p.state.symbol = string(dat[start:end])
			off = end + 1

		case bindOpcodeSetTypeImm: // 0x50 | imm
			p.state.typ = op & 0x0f

		case bindOpcodeSetAddendSLEB: // 0x60
			if op != bindOpcodeSetAddendSLEB {
				return nil, p.fail(opOff, "unsupported binding opcode %#02x", op)
			}
			v, n, err := p.sleb(off)
			if err != nil {
				return nil, err
			}
			off += n
			p.state.addend = v

		case bindOpcodeSetSegmentAndOffsetULEB: // 0x70 | segment
			si := int(op & 0x0f)
			v, n, err := p.uleb(off)
			if err != nil {
				return nil, err
			}
			off += n
			if si >= len(p.segments) {
				return nil, p.fail(opOff, "segment index %d out of range (%d segments)", si, len(p.segments))
			}
			p.state.hasSegment = true
			p.state.segmentIndex = si
			p.state.segmentOffset = v

		case bindOpcodeAddAddrULEB: // 0x80
			if op != bindOpcodeAddAddrULEB {
				return nil, p.fail(opOff, "unsupported binding opcode %#02x", op)
			}
			v, n, err := p.uleb(off)
			if err != nil {
				return nil, err
			}
			off += n
			if err := p.advance(opOff, v); err != nil {
				return nil, err
			}

		case bindOpcodeDoBind: // 0x90
			if op != bindOpcodeDoBind {
				return nil, p.fail(opOff, "unsupported binding opcode %#02x", op)
			}
			if err := p.emit(opOff); err != nil {
				return nil, err
			}
			if err := p.advance(opOff, p.ptrSize); err != nil {
				return nil, err
			}

		case bindOpcodeDoBindAddAddrULEB: // 0xa0
			if op != bindOpcodeDoBindAddAddrULEB {
				return nil, p.fail(opOff, "unsupported binding opcode %#02x", op)
			}
			if err := p.emit(opOff); err != nil {
				return nil, err
			}
			v, n, err := p.uleb(off)
			if err != nil {
				return nil, err
			}
			off += n
			if err := p.advance(opOff, v); err != nil {
				return nil, err
			}
			if err := p.advance(opOff, p.ptrSize); err != nil {
				return nil, err
			}

		case bindOpcodeDoBindAddAddrImmScaled: // 0xb0 | imm
			if err := p.emit(opOff); err != nil {
				return nil, err
			}
			// The immediate is scaled by the pointer size, not the field size.
			if err := p.advance(opOff, uint64(op&0x0f)*p.ptrSize); err != nil {
				return nil, err
			}
			if err := p.advance(opOff, p.ptrSize); err != nil {
				return nil, err
			}

		case bindOpcodeDoBindULEBTimesSkippingULEB: // 0xc0
			if op != bindOpcodeDoBindULEBTimesSkippingULEB {
				return nil, p.fail(opOff, "unsupported binding opcode %#02x", op)
			}
			count, n, err := p.uleb(off)
			if err != nil {
				return nil, err
			}
			off += n
			skip, n, err := p.uleb(off)
			if err != nil {
				return nil, err
			}
			off += n
			if count > uint64(MaxBindingExpansions)-uint64(len(p.out)) {
				return nil, p.fail(opOff, "repeated bind count %d exceeds expansion limit %d", count, MaxBindingExpansions)
			}
			for i := uint64(0); i < count; i++ {
				if err := p.emit(opOff); err != nil {
					return nil, err
				}
				if err := p.advance(opOff, skip); err != nil {
					return nil, err
				}
				if err := p.advance(opOff, p.ptrSize); err != nil {
					return nil, err
				}
			}

		case bindOpcodeThreaded: // 0xd0
			return nil, p.fail(opOff, "unsupported threaded binding opcode %#02x", op)

		default:
			// 0xe0/0xf0 and any other unassigned opcode.
			return nil, p.fail(opOff, "unsupported binding opcode %#02x", op)
		}
	}
	return p.out, nil
}

func addUint64(a, b uint64) (uint64, bool) {
	sum := a + b
	if sum < a {
		return 0, false
	}
	return sum, true
}

// readULEB128 reads an unsigned LEB128 value from dat at off. It rejects
// truncated and overflowing encodings instead of silently wrapping.
func readULEB128(dat []byte, off int) (uint64, int, error) {
	var result uint64
	var shift uint
	start := off
	for i := 0; i < 10; i++ {
		if off >= len(dat) {
			return 0, 0, fmt.Errorf("truncated ULEB128")
		}
		b := dat[off]
		off++
		if b < 0x80 {
			if i == 9 && b > 1 {
				return 0, 0, fmt.Errorf("ULEB128 overflow")
			}
			result |= uint64(b) << shift
			return result, off - start, nil
		}
		if i == 9 {
			return 0, 0, fmt.Errorf("ULEB128 overflow")
		}
		result |= uint64(b&0x7f) << shift
		shift += 7
	}
	return 0, 0, fmt.Errorf("ULEB128 overflow")
}

// readSLEB128 reads a signed LEB128 value from dat at off. It rejects
// truncated and overflowing encodings instead of silently wrapping.
func readSLEB128(dat []byte, off int) (int64, int, error) {
	var result int64
	var shift uint
	start := off
	for i := 0; i < 10; i++ {
		if off >= len(dat) {
			return 0, 0, fmt.Errorf("truncated SLEB128")
		}
		b := dat[off]
		off++
		if b < 0x80 {
			if i == 9 {
				// The tenth byte contributes only bit 63. Positive results
				// must terminate with 0x00 and sign-extended negative
				// results with 0x7f; anything else overflows int64.
				if b != 0x00 && b != 0x7f {
					return 0, 0, fmt.Errorf("SLEB128 overflow")
				}
			}
			result |= int64(b) << shift
			if shift+7 < 64 && b&0x40 != 0 {
				result |= -1 << (shift + 7)
			}
			return result, off - start, nil
		}
		if i == 9 {
			return 0, 0, fmt.Errorf("SLEB128 overflow")
		}
		result |= int64(b&0x7f) << shift
		shift += 7
	}
	return 0, 0, fmt.Errorf("SLEB128 overflow")
}
