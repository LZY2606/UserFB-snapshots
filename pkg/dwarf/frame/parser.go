// Package frame contains data structures and
// related functions for parsing and searching
// through Dwarf .debug_frame data.
package frame

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"io"
	"math"

	"github.com/go-delve/delve/pkg/dwarf"
	"github.com/go-delve/delve/pkg/dwarf/leb128"
)

type parsefunc func(*parseContext) parsefunc

type parseContext struct {
	staticBase uint64

	buf         *bytes.Buffer
	totalLen    int
	entries     FrameDescriptionEntries
	ciemap      map[int]*CommonInformationEntry
	common      *CommonInformationEntry
	frame       *FrameDescriptionEntry
	length      uint64
	dwarf64     bool
	recordStart int
	order       binary.ByteOrder
	ptrSize     int
	ehFrameAddr uint64
	err         error
}

// Parse takes in data (a byte slice) and returns FrameDescriptionEntries,
// which is a slice of FrameDescriptionEntry. Each FrameDescriptionEntry
// has a pointer to CommonInformationEntry.
// If ehFrameAddr is not zero the .eh_frame format will be used, a minor variant of DWARF described at https://www.airs.com/blog/archives/460.
// The value of ehFrameAddr will be used as the address at which eh_frame will be mapped into memory
//
// Each record in data independently selects its length format: either a
// 4-byte length or, when the first 4 bytes are 0xffffffff, a 12-byte
// length field (0xffffffff followed by an 8-byte length). CIE ids and FDE
// CIE pointers use the width of the record that contains them.
func Parse(data []byte, order binary.ByteOrder, staticBase uint64, ptrSize int, ehFrameAddr uint64) (fdes FrameDescriptionEntries, err error) {
	var (
		buf  = bytes.NewBuffer(data)
		pctx = &parseContext{buf: buf, totalLen: len(data), entries: newFrameIndex(), staticBase: staticBase, order: order, ptrSize: ptrSize, ehFrameAddr: ehFrameAddr, ciemap: map[int]*CommonInformationEntry{}}
	)

	// A malformed record must not panic or be reported as a partial result.
	// Decoders such as leb128 panic when their input is truncated, convert
	// those panics into errors that identify the record that caused them.
	defer func() {
		if r := recover(); r != nil {
			fdes = nil
			err = fmt.Errorf("could not parse .debug_frame record at %#x: %v", pctx.recordStart, r)
		}
	}()

	for fn := parselength; buf.Len() != 0; {
		fn = fn(pctx)
		if pctx.err != nil {
			return nil, pctx.err
		}
	}

	for i := range pctx.entries {
		pctx.entries[i].order = order
	}

	return pctx.entries, nil
}

func (ctx *parseContext) parsingEHFrame() bool {
	return ctx.ehFrameAddr > 0
}

// cieMarker returns the value used to identify a CIE for the length format
// of the record currently being parsed.
func (ctx *parseContext) cieMarker() uint64 {
	if ctx.parsingEHFrame() {
		return 0
	}
	if ctx.dwarf64 {
		return math.MaxUint64
	}
	return math.MaxUint32
}

func (ctx *parseContext) offset() int {
	return ctx.totalLen - ctx.buf.Len()
}

// readLength reads the length field of the next record, determining whether
// the record uses the 32-bit or 64-bit DWARF format.
func (ctx *parseContext) readLength() error {
	ctx.recordStart = ctx.offset()
	if ctx.buf.Len() < 4 {
		return ctx.parseError("truncated length field")
	}
	var len32 uint32
	if err := binary.Read(ctx.buf, ctx.order, &len32); err != nil {
		return ctx.parseError("truncated length field")
	}
	ctx.dwarf64 = false
	if len32 == math.MaxUint32 {
		if ctx.buf.Len() < 8 {
			return ctx.parseError("truncated 64-bit length field")
		}
		var len64 uint64
		if err := binary.Read(ctx.buf, ctx.order, &len64); err != nil {
			return ctx.parseError("truncated 64-bit length field")
		}
		ctx.dwarf64 = true
		ctx.length = len64
	} else {
		ctx.length = uint64(len32)
	}
	if ctx.length > uint64(ctx.buf.Len()) {
		return ctx.parseError(fmt.Sprintf("record length %#x exceeds remaining section length %#x", ctx.length, ctx.buf.Len()))
	}
	return nil
}

func (ctx *parseContext) parseError(what string) error {
	return fmt.Errorf("%s at %#x", what, ctx.recordStart)
}

// byteOrder is the section byte order, defaulting to little endian when a
// parseContext was built by hand (as some tests do).
func (ctx *parseContext) byteOrder() binary.ByteOrder {
	if ctx.order == nil {
		return binary.LittleEndian
	}
	return ctx.order
}

// sectionError returns an error mentioning the record offset and a target
// offset within the section.
func (ctx *parseContext) sectionError(what string, target int) error {
	return fmt.Errorf("%s at %#x (target %#x)", what, ctx.recordStart, target)
}

func (ctx *parseContext) readOffsetValue() (uint64, error) {
	if ctx.dwarf64 {
		if ctx.buf.Len() < 8 {
			return 0, ctx.parseError("truncated CIE id")
		}
		var v uint64
		if err := binary.Read(ctx.buf, ctx.order, &v); err != nil {
			return 0, ctx.parseError("truncated CIE id")
		}
		return v, nil
	}
	if ctx.buf.Len() < 4 {
		return 0, ctx.parseError("truncated CIE id")
	}
	var v uint32
	if err := binary.Read(ctx.buf, ctx.order, &v); err != nil {
		return 0, ctx.parseError("truncated CIE id")
	}
	return uint64(v), nil
}

func parselength(ctx *parseContext) parsefunc {
	if err := ctx.readLength(); err != nil {
		ctx.err = err
		return nil
	}

	if ctx.length == 0 {
		// ZERO terminator
		return parselength
	}

	// This change only extends .debug_frame to the 64-bit record format; the
	// .eh_frame reader keeps its existing 32-bit-only support.
	if ctx.dwarf64 && ctx.parsingEHFrame() {
		ctx.err = ctx.parseError(".eh_frame 64-bit format is not supported")
		return nil
	}

	// The width of the CIE id / CIE pointer field follows the length format of
	// this record. Check it fits inside the record before reading it, so a
	// truncated record can't borrow bytes from the next one.
	ptrWidth := uint64(4)
	if ctx.dwarf64 {
		ptrWidth = 8
	}
	if ctx.length < ptrWidth {
		ctx.err = ctx.parseError("record too short for CIE id")
		return nil
	}

	cieid, err := ctx.readOffsetValue()
	if err != nil {
		ctx.err = err
		return nil
	}

	// take off the width of the CIE id / CIE pointer field.
	ctx.length -= ptrWidth

	if cieid == ctx.cieMarker() {
		ctx.common = &CommonInformationEntry{Length: uint32(ctx.length), IsDWARF64: ctx.dwarf64, CIE_id: uint32(cieid), staticBase: ctx.staticBase, sectionOffset: ctx.recordStart, order: ctx.order, ptrSize: ctx.ptrSize}
		ctx.ciemap[ctx.recordStart] = ctx.common
		return parseCIE
	}

	if ctx.parsingEHFrame() {
		// In .eh_frame the CIE pointer is relative to the position of the
		// pointer field itself, which is right after the length field.
		lengthFieldSize := 4
		if ctx.dwarf64 {
			lengthFieldSize = 12
		}
		target := ctx.recordStart + lengthFieldSize - int(cieid)
		if cieid > uint64(ctx.recordStart+lengthFieldSize) || target >= ctx.totalLen {
			ctx.err = ctx.sectionError(fmt.Sprintf("invalid CIE pointer %#x", cieid), target)
			return nil
		}
		cieid = uint64(target)
	}

	if cieid >= uint64(ctx.totalLen) {
		ctx.err = ctx.sectionError(fmt.Sprintf("invalid CIE pointer %#x", cieid), int(cieid))
		return nil
	}

	common := ctx.ciemap[int(cieid)]

	if common == nil {
		ctx.err = ctx.sectionError(fmt.Sprintf("unknown CIE_id %#x", cieid), int(cieid))
		return nil
	}

	ctx.frame = &FrameDescriptionEntry{Length: uint32(ctx.length), IsDWARF64: ctx.dwarf64, CIE: common, sectionOffset: ctx.recordStart}
	return parseFDE
}

// readAddress reads an address-sized value using the byte order of the
// section. It reports an error if the record does not contain enough bytes.
func (ctx *parseContext) readAddress(r *bytes.Reader, addrSize int) (uint64, error) {
	if addrSize != 4 && addrSize != 8 {
		return 0, ctx.parseError(fmt.Sprintf("unsupported address size %d", addrSize))
	}
	if r.Len() < addrSize {
		return 0, ctx.parseError("truncated address/range")
	}
	var v uint64
	switch addrSize {
	case 4:
		var v32 uint32
		if err := binary.Read(r, ctx.order, &v32); err != nil {
			return 0, ctx.parseError("truncated address/range")
		}
		v = uint64(v32)
	case 8:
		if err := binary.Read(r, ctx.order, &v); err != nil {
			return 0, ctx.parseError("truncated address/range")
		}
	}
	return v, nil
}

func (ctx *parseContext) fdeAddressSize() int {
	if ctx.parsingEHFrame() {
		return ctx.ptrSize
	}
	if sz := int(ctx.frame.CIE.AddressSize); sz != 0 {
		return sz
	}
	return ctx.ptrSize
}

func parseFDE(ctx *parseContext) parsefunc {
	startOff := ctx.offset()
	r := ctx.buf.Next(int(ctx.length))

	reader := bytes.NewReader(r)

	if ctx.parsingEHFrame() {
		fieldAddr := addrSum(ctx.ehFrameAddr+uint64(startOff), reader)
		num, err := ctx.readEncodedPtr(fieldAddr, reader, ctx.frame.CIE.ptrEncAddr)
		if err != nil {
			ctx.err = ctx.parseError("truncated FDE address: " + err.Error())
			return nil
		}
		ctx.frame.begin = num + ctx.staticBase

		// For the size field in .eh_frame only the size encoding portion of the
		// address pointer encoding is considered.
		// See decode_frame_entry_1 in gdb/dwarf2-frame.c.
		// For .debug_frame ptrEncAddr is always ptrEncAbs and never has flags.
		sizePtrEnc := ctx.frame.CIE.ptrEncAddr & 0x0f
		ctx.frame.size, err = ctx.readEncodedPtr(0, reader, sizePtrEnc)
		if err != nil {
			ctx.err = ctx.parseError("truncated FDE range: " + err.Error())
			return nil
		}
	} else {
		// In .debug_frame the address size from the CIE (or ptrSize for
		// versions before 4) determines the width of both fields.
		addrSize := ctx.fdeAddressSize()
		ctx.frame.addressSize = addrSize
		begin, err := ctx.readAddress(reader, addrSize)
		if err != nil {
			ctx.err = err
			return nil
		}
		size, err := ctx.readAddress(reader, addrSize)
		if err != nil {
			ctx.err = err
			return nil
		}
		ctx.frame.begin = begin + ctx.staticBase
		ctx.frame.size = size
	}

	if ctx.parsingEHFrame() && len(ctx.frame.CIE.Augmentation) > 0 {
		// If we are parsing a .eh_frame and we saw an augmentation string then we
		// need to read the augmentation data, which are encoded as a ULEB128
		// size followed by 'size' bytes.
		n, _ := leb128.DecodeUnsigned(reader)
		if uint64(n) > uint64(reader.Len()) {
			ctx.err = ctx.parseError("truncated augmentation data")
			return nil
		}
		reader.Seek(int64(n), io.SeekCurrent)
	}

	// The rest of this entry consists of the instructions
	// so we can just grab all of the data from the buffer
	// cursor to length.

	off, _ := reader.Seek(0, io.SeekCurrent)
	ctx.frame.Instructions = r[off:]

	// Reject instruction streams whose operands are cut off by the end of the
	// record, so an FDE is never returned with a truncated rule program.
	addrSize := ctx.frame.addressSize
	if addrSize == 0 {
		addrSize = ctx.frame.CIE.addressSize()
	}
	if err := validateInstructions(ctx.frame.Instructions, ctx.byteOrder(), addrSize); err != nil {
		ctx.err = ctx.parseError("truncated CFI instruction: " + err.Error())
		return nil
	}

	// Only insert the FDE once the whole record has been parsed successfully,
	// so malformed records don't leave a partially initialized entry behind.
	ctx.entries = append(ctx.entries, ctx.frame)
	ctx.length = 0

	return parselength
}

// validateInstructions walks a CFI instruction stream and reports an error if
// a known instruction's operands run past the end of the stream. Unknown
// opcodes stop validation: their operand encoding is not known here and will
// be reported when the frame is established.
func validateInstructions(instr []byte, order binary.ByteOrder, addrSize int) error {
	r := bytes.NewReader(instr)
	uleb := func() error {
		var v uint64
		for i := 0; i < 10; i++ {
			b, err := r.ReadByte()
			if err != nil {
				return io.ErrUnexpectedEOF
			}
			v |= uint64(b&0x7f) << (7 * i)
			if b&0x80 == 0 {
				return nil
			}
		}
		return fmt.Errorf("ULEB128 value exceeds 64 bits")
	}
	block := func() error {
		var l uint64
		for i := 0; i < 10; i++ {
			b, err := r.ReadByte()
			if err != nil {
				return io.ErrUnexpectedEOF
			}
			l |= uint64(b&0x7f) << (7 * i)
			if b&0x80 == 0 {
				if uint64(r.Len()) < l {
					return io.ErrUnexpectedEOF
				}
				r.Seek(int64(l), io.SeekCurrent)
				return nil
			}
		}
		return fmt.Errorf("ULEB128 value exceeds 64 bits")
	}
	fixedN := func(n int) error {
		if r.Len() < n {
			return io.ErrUnexpectedEOF
		}
		r.Seek(int64(n), io.SeekCurrent)
		return nil
	}

	for r.Len() > 0 {
		b, _ := r.ReadByte()
		var err error
		switch b & 0xc0 {
		case DW_CFA_advance_loc, DW_CFA_restore:
			// The operand is in the low 6 bits of the opcode.
		case DW_CFA_offset:
			err = uleb()
		default:
			switch b {
			case DW_CFA_nop, DW_CFA_remember_state, DW_CFA_restore_state:
			case DW_CFA_set_loc:
				err = fixedN(addrSize)
			case DW_CFA_advance_loc1:
				err = fixedN(1)
			case DW_CFA_advance_loc2:
				err = fixedN(2)
			case DW_CFA_advance_loc4:
				err = fixedN(4)
			case DW_CFA_offset_extended, DW_CFA_def_cfa, DW_CFA_register, DW_CFA_val_offset:
				if err = uleb(); err == nil {
					err = uleb()
				}
			case DW_CFA_restore_extended, DW_CFA_undefined, DW_CFA_same_value,
				DW_CFA_def_cfa_register, DW_CFA_def_cfa_offset, DW_CFA_GNU_args_size:
				err = uleb()
			case DW_CFA_def_cfa_expression:
				err = block()
			case DW_CFA_expression, DW_CFA_val_expression:
				if err = uleb(); err == nil {
					err = block()
				}
			case DW_CFA_offset_extended_sf:
				if err = uleb(); err == nil {
					// SLEB is only checked for presence.
					err = uleb()
				}
			case DW_CFA_def_cfa_sf:
				if err = uleb(); err == nil {
					err = uleb()
				}
			case DW_CFA_def_cfa_offset_sf:
				err = uleb()
			case DW_CFA_val_offset_sf:
				if err = uleb(); err == nil {
					err = uleb()
				}
			default:
				// Unknown opcode (including vendor extensions): stop validating.
				// Its operand encoding is not known here and will be reported when
				// the frame is established.
				return nil
			}
		}
		if err != nil {
			return err
		}
	}
	return nil
}

func addrSum(base uint64, buf *bytes.Reader) uint64 {
	n, _ := buf.Seek(0, io.SeekCurrent)
	return base + uint64(n)
}

func parseCIE(ctx *parseContext) parsefunc {
	data := ctx.buf.Next(int(ctx.length))
	buf := bytes.NewBuffer(data)
	// parse version
	if buf.Len() < 1 {
		ctx.err = ctx.parseError("truncated CIE version")
		return nil
	}
	ctx.common.Version, _ = buf.ReadByte()

	// parse augmentation
	augmentation, err := dwarf.ReadString(buf)
	if err != nil {
		ctx.err = ctx.parseError("truncated CIE augmentation")
		return nil
	}
	ctx.common.Augmentation = augmentation

	if ctx.parsingEHFrame() {
		if ctx.common.Augmentation == "eh" {
			ctx.err = fmt.Errorf("unsupported 'eh' augmentation at %#x", ctx.recordStart)
			return nil
		}
		if len(ctx.common.Augmentation) > 0 && ctx.common.Augmentation[0] != 'z' {
			ctx.err = fmt.Errorf("unsupported augmentation at %#x (does not start with 'z')", ctx.recordStart)
			return nil
		}
	}

	if ctx.common.Version >= 4 {
		// Version 4 CIEs carry the address and segment selector sizes.
		if buf.Len() < 2 {
			ctx.err = ctx.parseError("truncated CIE v4 address size")
			return nil
		}
		ctx.common.AddressSize, _ = buf.ReadByte()
		ctx.common.SegmentSelectorSize, _ = buf.ReadByte()
		if ctx.common.SegmentSelectorSize != 0 {
			ctx.err = fmt.Errorf("unsupported segment selector size %d at %#x", ctx.common.SegmentSelectorSize, ctx.recordStart)
			return nil
		}
		if ctx.common.AddressSize != 4 && ctx.common.AddressSize != 8 {
			ctx.err = fmt.Errorf("unsupported address size %d at %#x", ctx.common.AddressSize, ctx.recordStart)
			return nil
		}
	}

	// parse code alignment factor
	ctx.common.CodeAlignmentFactor, err = decodeUnsigned(buf)
	if err != nil {
		ctx.err = ctx.parseError("truncated code alignment factor")
		return nil
	}

	// parse data alignment factor
	ctx.common.DataAlignmentFactor, err = decodeSigned(buf)
	if err != nil {
		ctx.err = ctx.parseError("truncated data alignment factor")
		return nil
	}

	// parse return address register
	// Version 1 stores it as a single byte, version 3 and later as a ULEB128.
	if ctx.common.Version == 1 {
		if buf.Len() < 1 {
			ctx.err = ctx.parseError("truncated return address register")
			return nil
		}
		b, _ := buf.ReadByte()
		ctx.common.ReturnAddressRegister = uint64(b)
	} else {
		ctx.common.ReturnAddressRegister, err = decodeUnsigned(buf)
		if err != nil {
			ctx.err = ctx.parseError("truncated return address register")
			return nil
		}
	}

	ctx.common.ptrEncAddr = ptrEncAbs

	if ctx.parsingEHFrame() && len(ctx.common.Augmentation) > 0 {
		_, _ = leb128.DecodeUnsigned(buf) // augmentation data length
		for i := 1; i < len(ctx.common.Augmentation); i++ {
			switch ctx.common.Augmentation[i] {
			case 'L':
				_, _ = buf.ReadByte() // LSDA pointer encoding, we don't support this.
			case 'R':
				// Pointer encoding, describes how begin and size fields of FDEs are encoded.
				b, err := buf.ReadByte()
				if err != nil {
					ctx.err = ctx.parseError("truncated pointer encoding")
					return nil
				}
				ctx.common.ptrEncAddr = ptrEnc(b)
				if !ctx.common.ptrEncAddr.Supported() {
					ctx.err = fmt.Errorf("pointer encoding not supported %#x at %#x", ctx.common.ptrEncAddr, ctx.recordStart)
					return nil
				}
			case 'S':
				// Signal handler invocation frame, we don't support this but there is no associated data to read.
			case 'P':
				// Personality function encoded as a pointer encoding byte followed by
				// the pointer to the personality function encoded as specified by the
				// pointer encoding.
				// We don't support this but have to read it anyway.
				b, err := buf.ReadByte()
				if err != nil {
					ctx.err = ctx.parseError("truncated personality pointer encoding")
					return nil
				}
				e := ptrEnc(b) &^ ptrEncIndirect
				if !e.Supported() {
					ctx.err = fmt.Errorf("pointer encoding not supported %#x at %#x", e, ctx.recordStart)
					return nil
				}
				if _, err := ctx.readEncodedPtr(0, buf, e); err != nil {
					ctx.err = ctx.parseError("truncated personality pointer: " + err.Error())
					return nil
				}
			default:
				ctx.err = fmt.Errorf("unsupported augmentation character %c at %#x", ctx.common.Augmentation[i], ctx.recordStart)
				return nil
			}
		}
	}

	// parse initial instructions
	// The rest of this entry consists of the instructions
	// so we can just grab all of the data from the buffer
	// cursor to length.
	ctx.common.InitialInstructions = buf.Bytes() //ctx.buf.Next(int(ctx.length))

	// Reject truncated operands in the CIE's initial instructions too.
	if err := validateInstructions(ctx.common.InitialInstructions, ctx.byteOrder(), ctx.common.addressSize()); err != nil {
		ctx.err = ctx.parseError("truncated CFI instruction: " + err.Error())
		return nil
	}
	ctx.length = 0

	return parselength
}

// decodeUnsigned decodes a ULEB128 value, returning an error instead of
// panicking if the input is truncated.
func decodeUnsigned(buf *bytes.Buffer) (uint64, error) {
	if buf.Len() == 0 {
		return 0, io.ErrUnexpectedEOF
	}
	var (
		result uint64
		shift  uint
	)
	for {
		b, err := buf.ReadByte()
		if err != nil {
			return 0, io.ErrUnexpectedEOF
		}
		if shift < 64 {
			result |= uint64(b&0x7f) << shift
		}
		if b&0x80 == 0 {
			return result, nil
		}
		shift += 7
		if shift >= 64 {
			return 0, fmt.Errorf("ULEB128 value exceeds 64 bits")
		}
	}
}

// decodeSigned decodes a SLEB128 value, returning an error instead of
// panicking if the input is truncated.
func decodeSigned(buf *bytes.Buffer) (int64, error) {
	if buf.Len() == 0 {
		return 0, io.ErrUnexpectedEOF
	}
	var (
		b      byte
		result int64
		shift  uint
	)
	for {
		var err error
		b, err = buf.ReadByte()
		if err != nil {
			return 0, io.ErrUnexpectedEOF
		}
		if shift < 64 {
			result |= (int64(b) & 0x7f) << shift
		}
		shift += 7
		if b&0x80 == 0 {
			break
		}
		if shift >= 64 {
			return 0, fmt.Errorf("SLEB128 value exceeds 64 bits")
		}
	}
	if shift < 64 && b&0x40 != 0 {
		result |= -1 << shift
	}
	return result, nil
}

// readEncodedPtr reads a pointer from buf encoded as specified by ptrEnc.
// This function is used to read pointers from a .eh_frame section, when
// used to parse a .debug_frame section ptrEnc will always be ptrEncAbs.
// The parameter addr is the address that the current byte of 'buf' will be
// mapped to when the executable file containing the eh_frame section being
// parse is loaded in memory.
func (ctx *parseContext) readEncodedPtr(addr uint64, buf leb128.Reader, ptrEnc ptrEnc) (uint64, error) {
	if ptrEnc == ptrEncOmit {
		return 0, nil
	}

	var (
		ptr uint64
		err error
	)

	switch ptrEnc & 0xf {
	case ptrEncAbs, ptrEncSigned:
		ptr, err = dwarf.ReadUintRaw(buf, ctx.byteOrder(), ctx.ptrSize)
	case ptrEncUleb:
		ptr, _ = leb128.DecodeUnsigned(buf)
	case ptrEncUdata2:
		ptr, err = dwarf.ReadUintRaw(buf, ctx.byteOrder(), 2)
	case ptrEncSdata2:
		ptr, err = dwarf.ReadUintRaw(buf, ctx.byteOrder(), 2)
		ptr = uint64(int16(ptr))
	case ptrEncUdata4:
		ptr, err = dwarf.ReadUintRaw(buf, ctx.byteOrder(), 4)
	case ptrEncSdata4:
		ptr, err = dwarf.ReadUintRaw(buf, ctx.byteOrder(), 4)
		ptr = uint64(int32(ptr))
	case ptrEncUdata8, ptrEncSdata8:
		ptr, err = dwarf.ReadUintRaw(buf, ctx.byteOrder(), 8)
	case ptrEncSleb:
		n, _ := leb128.DecodeSigned(buf)
		ptr = uint64(n)
	default:
		return 0, fmt.Errorf("unsupported pointer encoding %#x", ptrEnc)
	}

	if err != nil {
		return 0, err
	}

	if ptrEnc&0xf0 == ptrEncPCRel {
		ptr += addr
	}

	return ptr, nil
}

// DwarfEndian determines the endianness of the DWARF by using the version number field in the debug_info section
// Trick borrowed from "debug/dwarf".New()
func DwarfEndian(infoSec []byte) binary.ByteOrder {
	if len(infoSec) < 6 {
		return binary.BigEndian
	}
	x, y := infoSec[4], infoSec[5]
	switch {
	case x == 0 && y == 0:
		return binary.BigEndian
	case x == 0:
		return binary.BigEndian
	case y == 0:
		return binary.LittleEndian
	default:
		return binary.BigEndian
	}
}
