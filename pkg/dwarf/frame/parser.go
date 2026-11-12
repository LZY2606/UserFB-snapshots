// Package frame contains data structures and
// related functions for parsing and searching
// through Dwarf .debug_frame data.
package frame

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"io"

	"github.com/go-delve/delve/pkg/dwarf"
	"github.com/go-delve/delve/pkg/dwarf/leb128"
)

type parsefunc func(*parseContext) parsefunc

type parseContext struct {
	staticBase uint64

	buf      *bytes.Buffer
	totalLen int
	entries  FrameDescriptionEntries
	ciemap   map[uint64]*CommonInformationEntry
	common   *CommonInformationEntry
	frame    *FrameDescriptionEntry
	length   uint64
	dwarf64  bool // the record currently being parsed uses the 64-bit DWARF format
	recoff   int  // offset of the record currently being parsed
	ptrSize  int
	order    binary.ByteOrder

	ehFrameAddr uint64
	err         error
}

// Parse takes in data (a byte slice) and returns FrameDescriptionEntries,
// which is a slice of FrameDescriptionEntry. Each FrameDescriptionEntry
// has a pointer to CommonInformationEntry.
// If ehFrameAddr is not zero the .eh_frame format will be used, a minor variant of DWARF described at https://www.airs.com/blog/archives/460.
// The value of ehFrameAddr will be used as the address at which eh_frame will be mapped into memory
//
// Each record in a .debug_frame section independently selects the 32-bit or
// the 64-bit DWARF format through its length field (a 4-byte length, or
// 0xffffffff followed by an 8-byte length); the two formats can be freely
// intermixed and the 64-bit format does not imply 8-byte addresses. All
// fixed-width fields are read using the byte order specified by order.
func Parse(data []byte, order binary.ByteOrder, staticBase uint64, ptrSize int, ehFrameAddr uint64) (entries FrameDescriptionEntries, err error) {
	var (
		buf  = bytes.NewBuffer(data)
		pctx = &parseContext{buf: buf, totalLen: len(data), entries: newFrameIndex(), staticBase: staticBase, ptrSize: ptrSize, order: order, ehFrameAddr: ehFrameAddr, ciemap: map[uint64]*CommonInformationEntry{}}
	)

	// Malformed records must never cause a panic, convert any to an error
	// tied to the section offset of the record being parsed.
	defer func() {
		if r := recover(); r != nil {
			entries, err = nil, fmt.Errorf("error parsing frame record at offset %#x: %v", pctx.recoff, r)
		}
	}()

	for fn := parselength; buf.Len() != 0; {
		fn = fn(pctx)
		if pctx.err != nil {
			return nil, pctx.err
		}
		if fn == nil {
			return nil, fmt.Errorf("internal error parsing frame record at offset %#x", pctx.recoff)
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

func (ctx *parseContext) cieEntry(cieid uint64) bool {
	if ctx.parsingEHFrame() {
		return cieid == 0x00
	}
	if ctx.dwarf64 {
		return cieid == 0xffffffffffffffff
	}
	return cieid == 0xffffffff
}

func (ctx *parseContext) offset() int {
	return ctx.totalLen - ctx.buf.Len()
}

func parselength(ctx *parseContext) parsefunc {
	start := ctx.offset()
	ctx.recoff = start
	ctx.dwarf64 = false

	var length32 uint32
	if err := binary.Read(ctx.buf, ctx.order, &length32); err != nil {
		ctx.err = fmt.Errorf("could not read record length at offset %#x: %w", start, err)
		return nil
	}

	if length32 == 0 {
		// ZERO terminator
		return parselength
	}

	ctx.length = uint64(length32)
	if length32 == 0xffffffff {
		// 64-bit DWARF format: the length is stored as an 8-byte value
		// after the 0xffffffff marker. This only changes the width of the
		// length, CIE id and CIE pointer fields of this record, not the
		// address size.
		if ctx.parsingEHFrame() {
			ctx.err = fmt.Errorf("64-bit DWARF format not supported in .eh_frame, record at offset %#x", start)
			return nil
		}
		if err := binary.Read(ctx.buf, ctx.order, &ctx.length); err != nil {
			ctx.err = fmt.Errorf("could not read 64-bit record length at offset %#x: %w", start, err)
			return nil
		}
		ctx.dwarf64 = true
	}

	idLen := uint64(4)
	if ctx.dwarf64 {
		idLen = 8
	}
	if ctx.length < idLen || ctx.length-idLen > uint64(ctx.buf.Len()) {
		ctx.err = fmt.Errorf("record at offset %#x has length %d which exceeds the remaining section data (%d bytes)", start, ctx.length, ctx.buf.Len())
		return nil
	}

	var cieid uint64
	if ctx.dwarf64 {
		if err := binary.Read(ctx.buf, ctx.order, &cieid); err != nil {
			ctx.err = fmt.Errorf("could not read CIE id of record at offset %#x: %w", start, err)
			return nil
		}
	} else {
		var cieid32 uint32
		if err := binary.Read(ctx.buf, ctx.order, &cieid32); err != nil {
			ctx.err = fmt.Errorf("could not read CIE id of record at offset %#x: %w", start, err)
			return nil
		}
		cieid = uint64(cieid32)
	}

	ctx.length -= idLen // take off the length of the CIE id / CIE pointer.

	if ctx.cieEntry(cieid) {
		ctx.common = &CommonInformationEntry{Length: uint32(ctx.length), staticBase: ctx.staticBase, CIE_id: uint32(cieid), addrSize: ctx.ptrSize}
		ctx.ciemap[uint64(start)] = ctx.common
		return parseCIE
	}

	cieOffset := cieid
	if ctx.parsingEHFrame() {
		// In .eh_frame the CIE pointer is a backward offset from the
		// position of the pointer itself.
		cieOffset = uint64(start+4) - cieid
	}

	common := ctx.ciemap[cieOffset]

	if common == nil {
		ctx.err = fmt.Errorf("unknown CIE_id %#x at %#x", cieOffset, start)
		return nil
	}

	ctx.frame = &FrameDescriptionEntry{Length: uint32(ctx.length), CIE: common}
	return parseFDE
}

func parseFDE(ctx *parseContext) parsefunc {
	startOff := ctx.offset()
	r := ctx.buf.Next(int(ctx.length))

	reader := bytes.NewReader(r)
	addrSize := ctx.frame.CIE.addrSize
	num := ctx.readEncodedPtr(addrSum(ctx.ehFrameAddr+uint64(startOff), reader), reader, ctx.frame.CIE.ptrEncAddr, addrSize)
	if ctx.err != nil {
		return nil
	}
	ctx.frame.begin = num + ctx.staticBase

	// For the size field in .eh_frame only the size encoding portion of the
	// address pointer encoding is considered.
	// See decode_frame_entry_1 in gdb/dwarf2-frame.c.
	// For .debug_frame ptrEncAddr is always ptrEncAbs and never has flags.
	sizePtrEnc := ctx.frame.CIE.ptrEncAddr & 0x0f
	ctx.frame.size = ctx.readEncodedPtr(0, reader, sizePtrEnc, addrSize)
	if ctx.err != nil {
		return nil
	}

	// Insert into the tree after setting address range begin
	// otherwise compares won't work.
	ctx.entries = append(ctx.entries, ctx.frame)

	if ctx.parsingEHFrame() && len(ctx.frame.CIE.Augmentation) > 0 {
		// If we are parsing a .eh_frame and we saw an augmentation string then we
		// need to read the augmentation data, which are encoded as a ULEB128
		// size followed by 'size' bytes.
		n, _ := leb128.DecodeUnsigned(reader)
		if n > uint64(reader.Len()) {
			ctx.err = fmt.Errorf("truncated augmentation data in record at offset %#x", ctx.recoff)
			return nil
		}
		reader.Seek(int64(n), io.SeekCurrent)
	}

	// The rest of this entry consists of the instructions
	// so we can just grab all of the data from the buffer
	// cursor to length.

	off, _ := reader.Seek(0, io.SeekCurrent)
	ctx.frame.Instructions = r[off:]
	ctx.length = 0

	return parselength
}

func addrSum(base uint64, buf *bytes.Reader) uint64 {
	n, _ := buf.Seek(0, io.SeekCurrent)
	return base + uint64(n)
}

func parseCIE(ctx *parseContext) parsefunc {
	data := ctx.buf.Next(int(ctx.length))
	buf := bytes.NewBuffer(data)
	recoff := ctx.recoff

	truncated := func(what string) parsefunc {
		ctx.err = fmt.Errorf("truncated CIE at offset %#x: could not read %s", recoff, what)
		return nil
	}

	// parse version
	version, err := buf.ReadByte()
	if err != nil {
		return truncated("version")
	}
	ctx.common.Version = version

	// parse augmentation
	ctx.common.Augmentation, err = dwarf.ReadString(buf)
	if err != nil {
		return truncated("augmentation string")
	}

	if ctx.parsingEHFrame() {
		if ctx.common.Augmentation == "eh" {
			ctx.err = fmt.Errorf("unsupported 'eh' augmentation at %#x", recoff)
			return nil
		}
		if len(ctx.common.Augmentation) > 0 && ctx.common.Augmentation[0] != 'z' {
			ctx.err = fmt.Errorf("unsupported augmentation at %#x (does not start with 'z')", recoff)
			return nil
		}
	}

	if ctx.common.Version == 4 {
		// Version 4 CIEs declare the address size and the segment
		// selector size, which determine the width of the addresses in
		// the associated FDEs.
		addrSize, err := buf.ReadByte()
		if err != nil {
			return truncated("address_size")
		}
		segmentSize, err := buf.ReadByte()
		if err != nil {
			return truncated("segment_selector_size")
		}
		if segmentSize != 0 {
			ctx.err = fmt.Errorf("segmented CIEs are not supported (segment_selector_size %d), CIE at offset %#x", segmentSize, recoff)
			return nil
		}
		if addrSize != 4 && addrSize != 8 {
			ctx.err = fmt.Errorf("unsupported address_size %d in CIE at offset %#x", addrSize, recoff)
			return nil
		}
		ctx.common.addrSize = int(addrSize)
	}

	// parse code alignment factor
	if buf.Len() == 0 {
		return truncated("code alignment factor")
	}
	ctx.common.CodeAlignmentFactor, _ = leb128.DecodeUnsigned(buf)

	// parse data alignment factor
	if buf.Len() == 0 {
		return truncated("data alignment factor")
	}
	ctx.common.DataAlignmentFactor, _ = leb128.DecodeSigned(buf)

	// parse return address register
	if buf.Len() == 0 {
		return truncated("return address register")
	}
	if ctx.common.Version == 1 {
		b, _ := buf.ReadByte()
		ctx.common.ReturnAddressRegister = uint64(b)
	} else {
		ctx.common.ReturnAddressRegister, _ = leb128.DecodeUnsigned(buf)
	}

	ctx.common.ptrEncAddr = ptrEncAbs

	if ctx.parsingEHFrame() && len(ctx.common.Augmentation) > 0 {
		if buf.Len() == 0 {
			return truncated("augmentation data length")
		}
		_, _ = leb128.DecodeUnsigned(buf) // augmentation data length
		for i := 1; i < len(ctx.common.Augmentation); i++ {
			switch ctx.common.Augmentation[i] {
			case 'L':
				if _, err := buf.ReadByte(); err != nil {
					return truncated("LSDA pointer encoding")
				}
			case 'R':
				// Pointer encoding, describes how begin and size fields of FDEs are encoded.
				b, err := buf.ReadByte()
				if err != nil {
					return truncated("FDE pointer encoding")
				}
				ctx.common.ptrEncAddr = ptrEnc(b)
				if !ctx.common.ptrEncAddr.Supported() {
					ctx.err = fmt.Errorf("pointer encoding not supported %#x at %#x", ctx.common.ptrEncAddr, recoff)
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
					return truncated("personality pointer encoding")
				}
				e := ptrEnc(b) &^ ptrEncIndirect
				if !e.Supported() {
					ctx.err = fmt.Errorf("pointer encoding not supported %#x at %#x", e, recoff)
					return nil
				}
				ctx.readEncodedPtr(0, buf, e, ctx.common.addrSize)
				if ctx.err != nil {
					return nil
				}
			default:
				ctx.err = fmt.Errorf("unsupported augmentation character %c at %#x", ctx.common.Augmentation[i], recoff)
				return nil
			}
		}
	}

	// parse initial instructions
	// The rest of this entry consists of the instructions
	// so we can just grab all of the data from the buffer
	// cursor to length.
	ctx.common.InitialInstructions = buf.Bytes() //ctx.buf.Next(int(ctx.length))
	ctx.length = 0

	return parselength
}

// readEncodedPtr reads a pointer from buf encoded as specified by ptrEnc.
// This function is used to read pointers from a .eh_frame section, when
// used to parse a .debug_frame section ptrEnc will always be ptrEncAbs.
// The parameter addr is the address that the current byte of 'buf' will be
// mapped to when the executable file containing the eh_frame section being
// parse is loaded in memory.
// The ptrSize argument is the size of a pointer-sized (absolute) encoding,
// for .debug_frame it is the address size declared by the CIE.
func (ctx *parseContext) readEncodedPtr(addr uint64, buf leb128.Reader, ptrEnc ptrEnc, ptrSize int) uint64 {
	if ptrEnc == ptrEncOmit {
		return 0
	}

	var (
		ptr uint64
		err error
	)

	switch ptrEnc & 0xf {
	case ptrEncAbs, ptrEncSigned:
		ptr, err = dwarf.ReadUintRaw(buf, ctx.order, ptrSize)
	case ptrEncUleb:
		ptr, _ = leb128.DecodeUnsigned(buf)
	case ptrEncUdata2:
		ptr, err = dwarf.ReadUintRaw(buf, ctx.order, 2)
	case ptrEncSdata2:
		ptr, err = dwarf.ReadUintRaw(buf, ctx.order, 2)
		ptr = uint64(int16(ptr))
	case ptrEncUdata4:
		ptr, err = dwarf.ReadUintRaw(buf, ctx.order, 4)
	case ptrEncSdata4:
		ptr, err = dwarf.ReadUintRaw(buf, ctx.order, 4)
		ptr = uint64(int32(ptr))
	case ptrEncUdata8, ptrEncSdata8:
		ptr, err = dwarf.ReadUintRaw(buf, ctx.order, 8)
	case ptrEncSleb:
		n, _ := leb128.DecodeSigned(buf)
		ptr = uint64(n)
	}

	if err != nil {
		ctx.err = fmt.Errorf("could not read encoded pointer in record at offset %#x: %w", ctx.recoff, err)
		return 0
	}

	if ptrEnc&0xf0 == ptrEncPCRel {
		ptr += addr
	}

	return ptr
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
