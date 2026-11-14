package macho

// Legacy LC_DYLD_INFO(_ONLY) threaded bind/rebase chains, as emitted by ld64 for
// early arm64e images before LC_DYLD_CHAINED_FIXUPS existed. Everything here is
// a static, read-only interpretation of the file's bytes: no dependency library
// is loaded, no image is executed, and pointer authentication is neither
// verified nor generated.

import (
	"fmt"

	"github.com/blacktop/go-macho/types"
)

// Bit layout of an arm64e threaded chain slot, shared by the four slot kinds.
const (
	threadedBindBit    = uint64(1) << 62 // 0: rebase, 1: bind
	threadedAuthBit    = uint64(1) << 63 // 1: authenticated pointer metadata
	threadedNextMask   = uint64(0x7FF)
	threadedNextShift  = 51
	threadedOrdinalMax = uint64(0xFFFF)
)

// threadedFixups holds the bind ordinal table built by the threaded opcodes and
// every rebase/bind record produced while walking its APPLY chains.
type threadedFixups struct {
	ordinalTable []types.Bind
	binds        []types.Bind
	rebases      []types.Rebase
	// rawRebases maps a slot's raw encoded value to its decoded info, so
	// value-only callers (SlidePointer) can recognize threaded rebase slots.
	rawRebases map[uint64]*types.ThreadedPointerInfo
}

// signExtend19 sign-extends a 19-bit unsigned slot addend.
func signExtend19(value uint64) int64 {
	const mask = uint64(0x7FFFF)
	addend := value & mask
	if addend&0x40000 != 0 {
		addend |= ^mask
	}
	return int64(addend)
}

// signExtend43 sign-extends the low 43 bits of a plain rebase slot.
func signExtend43(value uint64) int64 {
	const mask = uint64(0x7FFFFFFFFFF)
	low := value & mask
	if low&0x40000000000 != 0 {
		low |= ^mask
	}
	return int64(low)
}

// decodeThreadedPlainRebaseTarget decodes a non-authenticated rebase slot using
// the legacy arm64e encoding: a split 51-bit absolute target whose low 43 bits
// are signed. Like ld64/dyld this yields the preferred VM address directly;
// only the runtime slide would be added later, never the image base.
func decodeThreadedPlainRebaseTarget(raw uint64) uint64 {
	top8Bits := raw & 0x0007F80000000000
	bottom43Bits := raw & 0x000007FFFFFFFFFF
	return (top8Bits << 13) | (uint64(signExtend43(bottom43Bits)) & 0x00FFFFFFFFFFFFFF)
}

func threadedIsBind(raw uint64) bool { return raw&threadedBindBit != 0 }
func threadedIsAuth(raw uint64) bool { return raw&threadedAuthBit != 0 }
func threadedNext(raw uint64) uint64 {
	return (raw >> threadedNextShift) & threadedNextMask
}

// threadedAuthFields extracts the authenticated pointer metadata carried by an
// auth bind or auth rebase slot.
func threadedAuthFields(raw uint64) (key uint8, diversity uint16, addressDiversity bool) {
	key = uint8((raw >> 49) & 0x3)
	diversity = uint16((raw >> 32) & 0xFFFF)
	addressDiversity = raw&(1<<48) != 0
	return
}

// applyThreadedChain walks one threaded chain starting at slotOffset in the
// segment selected by segIndex. The ordinal table only supplies template data
// (symbol, dylib, flags, addend); every record's segment, section, virtual
// address and file location come from the slot currently being applied.
func (f *File) applyThreadedChain(
	tf *threadedFixups, segIndex int, slotOffset uint64, kind types.BindKind,
) error {
	segments := f.Segments()
	if segIndex < 0 || segIndex >= len(segments) {
		return fmt.Errorf("threaded APPLY segment index %d out of range", segIndex)
	}
	seg := segments[segIndex]
	base := f.GetBaseAddress()

	// Each slot occupies eight bytes and the chain only advances, so the number
	// of slots is bounded by the segment size.
	maxSlots := seg.Memsz/8 + 1
	for iteration := uint64(0); ; iteration++ {
		if iteration > maxSlots {
			return fmt.Errorf("threaded chain at segment %q offset %#x exceeded segment bounds",
				seg.Name, slotOffset)
		}
		if seg.Filesz < 8 || slotOffset > seg.Filesz-8 {
			return fmt.Errorf("threaded chain slot at segment %q offset %#x is outside file data",
				seg.Name, slotOffset)
		}

		var rawBuf [8]byte
		if _, err := f.cr.ReadAt(rawBuf[:], int64(seg.Offset+slotOffset)); err != nil {
			return fmt.Errorf("failed to read threaded pointer: %v", err)
		}
		raw := f.ByteOrder.Uint64(rawBuf[:])
		vmAddr := seg.Addr + slotOffset

		if threadedIsBind(raw) {
			ordinal := raw & threadedOrdinalMax
			if int(ordinal) >= len(tf.ordinalTable) {
				return fmt.Errorf("threaded bind ordinal %d out of range (table has %d entries)",
					ordinal, len(tf.ordinalTable))
			}
			template := tf.ordinalTable[ordinal]
			record := template // the table entry is a template, not a located record
			record.Kind = kind
			record.Segment = seg.Name
			record.SegStart = seg.Offset
			record.Start = seg.Addr
			record.SegOffset = slotOffset
			record.Value = raw
			if sec := f.FindSectionForVMAddr(vmAddr); sec != nil {
				record.Section = sec.Name
			}

			info := &types.ThreadedPointerInfo{Ordinal: uint16(ordinal)}
			if threadedIsAuth(raw) {
				info.Authenticated = true
				info.Key, info.Diversity, info.AddressDiversity = threadedAuthFields(raw)
				// Authenticated binds carry no slot addend; the template addend is used as is.
			} else {
				record.Addend = template.Addend + signExtend19(raw>>32)
			}
			record.Threaded = info
			tf.binds = append(tf.binds, record)
		} else {
			info := &types.ThreadedPointerInfo{}
			if threadedIsAuth(raw) {
				// Authenticated rebase slots hold a 32-bit target relative to
				// the image base.
				info.Authenticated = true
				info.Key, info.Diversity, info.AddressDiversity = threadedAuthFields(raw)
				info.Target = base + (raw & 0xFFFFFFFF)
			} else {
				// Plain rebase slots hold the absolute preferred VM address.
				info.Target = decodeThreadedPlainRebaseTarget(raw)
			}
			record := types.Rebase{
				Type:     types.REBASE_TYPE_POINTER,
				Segment:  seg.Name,
				Start:    seg.Addr,
				Offset:   slotOffset,
				Value:    raw,
				Threaded: info,
			}
			if sec := f.FindSectionForVMAddr(vmAddr); sec != nil {
				record.Section = sec.Name
			}
			tf.rebases = append(tf.rebases, record)
			tf.rawRebases[raw] = info
		}

		delta := threadedNext(raw)
		if delta == 0 {
			return nil
		}
		slotOffset += delta * f.pointerSize()
	}
}

// ensureThreadedFixups parses the bind bytecode stream when it has not been
// parsed yet and returns the threaded state, or nil for a classic bind stream.
func (f *File) ensureThreadedFixups() (*threadedFixups, error) {
	if f.threaded != nil {
		return f.threaded, nil
	}
	if _, err := f.GetBindInfo(); err != nil {
		return nil, err
	}
	return f.threaded, nil
}

// decodeThreadedRebaseValue decodes a raw slot value when it belongs to a
// threaded rebase chain. It never matches bind slots or ordinary pointers.
func (f *File) decodeThreadedRebaseValue(raw uint64) (uint64, bool) {
	if f.threaded == nil {
		if _, err := f.ensureThreadedFixups(); err != nil || f.threaded == nil {
			return 0, false
		}
	}
	if info, ok := f.threaded.rawRebases[raw]; ok {
		return info.Target, true
	}
	return 0, false
}
