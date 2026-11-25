package op

import (
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"math"
	"sort"
	"sync/atomic"
)

// MaxLocationWriteBits is the largest variable that PlanLocationWrite will
// plan. It keeps validation and planning work bounded.
const MaxLocationWriteBits uint64 = 1 << 20

// MemoryWriteFunc writes exactly len(data) bytes at addr. It returns the
// number of bytes accepted and the first write error, if any.
type MemoryWriteFunc func(addr uint64, data []byte) (int, error)

// PhysicalWriteKind identifies the address space of a PhysicalWrite.
type PhysicalWriteKind uint8

const (
	// PhysicalRegisterWrite is a complete native-width register update.
	PhysicalRegisterWrite PhysicalWriteKind = iota + 1
	// PhysicalMemoryWrite is one contiguous run of affected memory bytes.
	PhysicalMemoryWrite
)

// String returns a short name for the physical write kind.
func (k PhysicalWriteKind) String() string {
	switch k {
	case PhysicalRegisterWrite:
		return "register"
	case PhysicalMemoryWrite:
		return "memory"
	default:
		return "unknown"
	}
}

// RegisterWrite is a complete register image. Bits not selected by the
// logical write are copied from the prepared original value.
type RegisterWrite struct {
	// RegNum is the DWARF register number.
	RegNum uint64
	// Original is the complete native-width value observed while planning.
	Original []byte
	// Data is the complete native-width value to submit through ChangeFunc.
	Data []byte
}

// MemoryWrite is one contiguous run of affected memory bytes. Bytes between
// two separated affected addresses are not included.
type MemoryWrite struct {
	// Address is the physical address of the first byte in Data.
	Address uint64
	// Original is the prepared value of the contiguous byte run.
	Original []byte
	// Data is the value to write at Address.
	Data []byte
}

// PhysicalWrite is a register or memory record in the plan's fixed commit
// order: register numbers ascending, then memory addresses ascending.
type PhysicalWrite struct {
	Kind     PhysicalWriteKind
	RegNum   uint64
	Address  uint64
	Original []byte
	Data     []byte
}

// LocationWritePlan is a validated set of idempotent physical bit updates.
//
// A plan owns its original and replacement byte slices. Accessor methods
// return copies, so mutating a returned record cannot change Commit.
//
// Call Commit at most once. A rejected preflight, callback failure, short
// write, or successful commit consumes the plan; create a new plan to retry.
type LocationWritePlan struct {
	bitSize   uint64
	registers []RegisterWrite
	memories  []MemoryWrite
	used      atomic.Bool
}

// BitSource identifies one selected logical bit and the expression fragment
// that located it. Fragment is zero-based in DWARF expression order.
type BitSource struct {
	LogicalBit  uint64
	Fragment    int
	FragmentBit uint64
}

// PhysicalTarget identifies one aliased physical bit. For registers, Bit is
// counted from the register's least-significant bit. For memory, Address is
// the byte address and Bit is in the range 0..7, counted from that byte's
// least-significant bit.
type PhysicalTarget struct {
	Kind    PhysicalWriteKind
	RegNum  uint64
	Address uint64
	Bit     uint64
}

// AliasConflictError reports two selected logical bits that reach the same
// physical bit but request different values.
type AliasConflictError struct {
	Target PhysicalTarget
	First  BitSource
	Second BitSource
}

func (e *AliasConflictError) Error() string {
	switch e.Target.Kind {
	case PhysicalRegisterWrite:
		return fmt.Sprintf("conflicting writes to register %d bit %d: logical bit %d requests one value and logical bit %d requests the other",
			e.Target.RegNum, e.Target.Bit, e.First.LogicalBit, e.Second.LogicalBit)
	default:
		return fmt.Sprintf("conflicting writes to memory %#x bit %d: logical bit %d requests one value and logical bit %d requests the other",
			e.Target.Address, e.Target.Bit, e.First.LogicalBit, e.Second.LogicalBit)
	}
}

// ConstantMismatchError reports a selected stack_value or implicit_value bit
// whose requested replacement differs from the immutable source value.
type ConstantMismatchError struct {
	LogicalBit     uint64
	Fragment       int
	SourceBit      uint64
	OriginalValue  byte
	RequestedValue byte
}

func (e *ConstantMismatchError) Error() string {
	return fmt.Sprintf("cannot change constant fragment for logical bit %d: source bit %d is %d but requested %d",
		e.LogicalBit, e.SourceBit, e.OriginalValue, e.RequestedValue)
}

// UnavailableBitError reports a selected logical bit with no writable source.
type UnavailableBitError struct {
	LogicalBit uint64
	Fragment   int
	Reason     string
}

func (e *UnavailableBitError) Error() string {
	if e.Fragment >= 0 {
		return fmt.Sprintf("logical bit %d is unavailable (fragment %d): %s", e.LogicalBit, e.Fragment, e.Reason)
	}
	return fmt.Sprintf("logical bit %d is unavailable: %s", e.LogicalBit, e.Reason)
}

// MissingRegisterError reports a selected register that is not present after
// DwarfRegisters.Reg has run its lazy loader.
type MissingRegisterError struct {
	RegNum uint64
}

func (e *MissingRegisterError) Error() string {
	return fmt.Sprintf("DWARF register %d is not available", e.RegNum)
}

// ExpressionError annotates a malformed new-path location expression with its
// byte offset. Offset is the offset of the relevant opcode or terminator.
type ExpressionError struct {
	Offset int
	Err    error
}

func (e *ExpressionError) Error() string {
	return fmt.Sprintf("DWARF expression error at offset %d: %v", e.Offset, e.Err)
}

func (e *ExpressionError) Unwrap() error { return e.Err }

// ExpressionOffset returns the expression offset carried by err, if any.
func ExpressionOffset(err error) (int, bool) {
	var exprErr *ExpressionError
	if errors.As(err, &exprErr) {
		return exprErr.Offset, true
	}
	return 0, false
}

// CommitStage says whether a CommitResult failure happened during preflight
// or while issuing writes.
type CommitStage string

const (
	CommitPreflight CommitStage = "preflight"
	CommitWriting   CommitStage = "writing"
)

// CommitResult describes how much of a commit completed.
type CommitResult struct {
	// Stage is CommitPreflight or CommitWriting.
	Stage CommitStage
	// CompletedRecords contains indexes into Writes() that completed.
	CompletedRecords []int
	// FailedIndex is the Writes() index that failed, or -1 on success.
	FailedIndex int
	// BytesWritten is the byte count returned by the current memory write.
	// Register ChangeFunc does not return a byte count.
	BytesWritten int
	// RecordLength is the full byte length of the failed record.
	RecordLength int
	// Err is the first failure. It is also returned by Commit.
	Err error
}

// Succeeded reports whether every record committed.
func (r *CommitResult) Succeeded() bool {
	return r != nil && r.Err == nil && r.FailedIndex < 0
}

var (
	// ErrBitPieceUnsupported is returned by the legacy ExecuteStackProgram
	// path when it encounters DW_OP_bit_piece.
	ErrBitPieceUnsupported = errors.New("DW_OP_bit_piece is not supported by ExecuteStackProgram; use PlanLocationWrite")
	// ErrExpressionStepsExhausted reports the shared interpreter's step cap.
	ErrExpressionStepsExhausted = errors.New("DWARF expression execution step limit exhausted")
	// ErrLengthOverflow reports a piece length that cannot be represented.
	ErrLengthOverflow = errors.New("DWARF piece length overflow")
	// ErrPlanAlreadyCommitted is returned when Commit is called twice.
	ErrPlanAlreadyCommitted = errors.New("location write plan can only be committed once")
	// ErrPreflight indicates that a current value differs from preparation.
	ErrPreflight = errors.New("location write preflight failed")
	// ErrShortWrite reports a memory callback that accepted too few bytes.
	ErrShortWrite = errors.New("short memory write")
)

type locationFragment struct {
	piece           Piece
	unavailable     bool
	explicit        bool
	bitLength       uint64
	sourceBitOffset uint64
	offset          int
}

type logicalFragment struct {
	locationFragment
	index  int
	start  uint64
	length uint64
}

type physicalKey struct {
	kind    PhysicalWriteKind
	regNum  uint64
	address uint64
	bit     uint64
}

type bitVote struct {
	value  byte
	source BitSource
}

// PlanLocationWrite evaluates instructions and converts the selected bits of
// a logical variable write into consistent register and memory updates.
//
// data and mask are packed least-significant bit first; bit i is bit i%8 of
// byte i/8. Both slices must have (bitSize+7)/8 bytes, and mask bits at or
// above bitSize must be zero. Only little-endian register images and ptrSize
// 4 or 8 are supported. bitSize must not exceed MaxLocationWriteBits.
//
// The regs value is used only for evaluation, lazy register loading, and
// preparation. Existing DwarfRegister byte slices are not filled or
// overwritten. readMemory is used for expression dereferences and for reading
// the merged physical memory records during preparation.
//
// Typical use:
//
//	plan, err := op.PlanLocationWrite(regs, expr, 8, 32, data, mask, readMem)
//	if err != nil { return err }
//	for _, r := range plan.Writes() { _ = r } // optional inspection only
//	result, err := plan.Commit(currentRegs, readMem, writeMem)
//	if err != nil { log.Fatalf("commit failed: %+v", result) }
//
// Commit rechecks all prepared original values before its first write.
func PlanLocationWrite(regs DwarfRegisters, instructions []byte, ptrSize int, bitSize uint64, data, mask []byte, readMemory ReadMemoryFunc) (*LocationWritePlan, error) {
	if ptrSize != 4 && ptrSize != 8 {
		return nil, fmt.Errorf("ptrSize must be 4 or 8, got %d", ptrSize)
	}
	if bitSize > MaxLocationWriteBits {
		return nil, fmt.Errorf("bitSize %d exceeds supported maximum %d", bitSize, MaxLocationWriteBits)
	}
	byteCount := int((bitSize + 7) / 8)
	if len(data) != byteCount {
		return nil, fmt.Errorf("data length %d does not match ceil(bitSize/8)=%d", len(data), byteCount)
	}
	if len(mask) != byteCount {
		return nil, fmt.Errorf("mask length %d does not match ceil(bitSize/8)=%d", len(mask), byteCount)
	}
	if bitSize%8 != 0 {
		validBits := uint(bitSize % 8)
		if bad := mask[byteCount-1] & (byte(0xff) << validBits); bad != 0 {
			return nil, fmt.Errorf("mask has nonzero bits above bitSize %d: %#02x", bitSize, bad)
		}
	}
	if regs.ByteOrder != nil && regs.ByteOrder != binary.LittleEndian {
		return nil, fmt.Errorf("only little-endian register images are supported, got %v", regs.ByteOrder)
	}

	ctxt, err := runStackProgram(regs, instructions, ptrSize, readMemory, true)
	if err != nil {
		return nil, err
	}

	fragments, totalBits, err := resolveLogicalFragments(ctxt, bitSize)
	if err != nil {
		return nil, err
	}

	p := &LocationWritePlan{bitSize: bitSize}
	votes := make(map[physicalKey]bitVote)
	regOriginals := make(map[uint64][]byte)

	for _, frag := range fragments {
		for fragmentBit := uint64(0); fragmentBit < frag.length; fragmentBit++ {
			logicalBit := frag.start + fragmentBit
			if logicalBit >= bitSize || !packedBit(mask, logicalBit) {
				continue
			}
			requested := packedBitValue(data, logicalBit)
			sourceBit, overflow := addUint64(frag.sourceBitOffset, fragmentBit)
			if overflow {
				return nil, ctxt.exprError(frag.offset, fmt.Errorf("source bit offset overflow at logical bit %d", logicalBit))
			}
			source := BitSource{LogicalBit: logicalBit, Fragment: frag.index, FragmentBit: fragmentBit}

			if frag.unavailable {
				return nil, &UnavailableBitError{LogicalBit: logicalBit, Fragment: frag.index, Reason: "empty DWARF location piece"}
			}

			switch frag.piece.Kind {
			case RegPiece:
				regNum := frag.piece.Val
				original, err := registerOriginal(&ctxt.DwarfRegisters, regNum, regOriginals)
				if err != nil {
					return nil, err
				}
				if sourceBit >= uint64(len(original))*8 {
					return nil, &UnavailableBitError{LogicalBit: logicalBit, Fragment: frag.index, Reason: fmt.Sprintf("register %d source bit %d is outside %d-byte register", regNum, sourceBit, len(original))}
				}
				key := physicalKey{kind: PhysicalRegisterWrite, regNum: regNum, bit: sourceBit}
				if err := addVote(votes, key, requested, source); err != nil {
					return nil, err
				}

			case AddrPiece:
				byteOffset := sourceBit / 8
				physicalBit := sourceBit % 8
				if frag.piece.Val > math.MaxUint64-byteOffset {
					return nil, ctxt.exprError(frag.offset, fmt.Errorf("memory address overflow for logical bit %d", logicalBit))
				}
				address := frag.piece.Val + byteOffset
				key := physicalKey{kind: PhysicalMemoryWrite, address: address, bit: physicalBit}
				if err := addVote(votes, key, requested, source); err != nil {
					return nil, err
				}

			case ImmPiece:
				originalBit, err := pieceSourceBit(frag.piece, sourceBit)
				if err != nil {
					return nil, &UnavailableBitError{LogicalBit: logicalBit, Fragment: frag.index, Reason: err.Error()}
				}
				if originalBit != requested {
					return nil, &ConstantMismatchError{
						LogicalBit: logicalBit, Fragment: frag.index, SourceBit: sourceBit,
						OriginalValue: originalBit, RequestedValue: requested,
					}
				}

			default:
				return nil, ctxt.exprError(frag.offset, fmt.Errorf("unsupported location piece kind %d", frag.piece.Kind))
			}
		}
	}

	for logicalBit := totalBits; logicalBit < bitSize; logicalBit++ {
		if packedBit(mask, logicalBit) {
			return nil, &UnavailableBitError{LogicalBit: logicalBit, Fragment: -1, Reason: "composite location is shorter than the object"}
		}
	}

	regData := make(map[uint64][]byte, len(regOriginals))
	for regNum, original := range regOriginals {
		regData[regNum] = cloneByteSlice(original)
	}
	memSet := make(map[uint64]byte)
	memClear := make(map[uint64]byte)

	for key, vote := range votes {
		switch key.kind {
		case PhysicalRegisterWrite:
			data := regData[key.regNum]
			applyPhysicalBit(data, key.bit, vote.value)
		case PhysicalMemoryWrite:
			maskBit := byte(1) << key.bit
			if vote.value == 1 {
				memSet[key.address] |= maskBit
			} else {
				memClear[key.address] |= maskBit
			}
		}
	}

	regNums := make([]uint64, 0, len(regOriginals))
	for regNum := range regOriginals {
		regNums = append(regNums, regNum)
	}
	sort.Slice(regNums, func(i, j int) bool { return regNums[i] < regNums[j] })
	for _, regNum := range regNums {
		p.registers = append(p.registers, RegisterWrite{
			RegNum:   regNum,
			Original: cloneByteSlice(regOriginals[regNum]),
			Data:     regData[regNum],
		})
	}

	if len(memSet)+len(memClear) > 0 {
		if readMemory == nil {
			return nil, errors.New("memory location writes require a ReadMemoryFunc to prepare original bytes")
		}
		addressSet := make(map[uint64]struct{}, len(memSet)+len(memClear))
		for addr := range memSet {
			addressSet[addr] = struct{}{}
		}
		for addr := range memClear {
			addressSet[addr] = struct{}{}
		}
		addresses := make([]uint64, 0, len(addressSet))
		for addr := range addressSet {
			addresses = append(addresses, addr)
		}
		sort.Slice(addresses, func(i, j int) bool { return addresses[i] < addresses[j] })

		for i := 0; i < len(addresses); {
			start := addresses[i]
			end := start
			j := i + 1
			for j < len(addresses) && addresses[j] == end+1 {
				end = addresses[j]
				j++
			}
			length := int(end-start) + 1
			original := make([]byte, length)
			n, err := readMemory(original, start)
			if err != nil {
				return nil, fmt.Errorf("reading prepared memory at %#x: %w", start, err)
			}
			if n != length {
				return nil, fmt.Errorf("short read preparing memory at %#x: read %d bytes, wanted %d", start, n, length)
			}
			data := cloneByteSlice(original)
			for offset := 0; offset < length; offset++ {
				addr := start + uint64(offset)
				data[offset] &^= memClear[addr]
				data[offset] |= memSet[addr]
			}
			p.memories = append(p.memories, MemoryWrite{Address: start, Original: original, Data: data})
			i = j
		}
	}

	return p, nil
}

func resolveLogicalFragments(ctxt *context, bitSize uint64) ([]logicalFragment, uint64, error) {
	if len(ctxt.locs) == 0 {
		if len(ctxt.stack) == 0 {
			// A location program with no result is an empty/unavailable
			// location. ExecuteStackProgram preserves its historical
			// "empty OP stack" error; the write planner models it so selected
			// bits fail while an all-unselected write can still be empty.
			frag := logicalFragment{
				locationFragment: locationFragment{unavailable: true, explicit: false, offset: ctxt.opOffset},
				index:            0,
				start:            0,
				length:           bitSize,
			}
			return []logicalFragment{frag}, bitSize, nil
		}
		addr := uint64(ctxt.stack[len(ctxt.stack)-1])
		frag := logicalFragment{
			locationFragment: locationFragment{offset: ctxt.opOffset},
			index:            0,
			start:            0,
			length:           bitSize,
		}
		frag.piece = Piece{Kind: AddrPiece, Val: addr}
		return []logicalFragment{frag}, bitSize, nil
	}

	fragments := make([]logicalFragment, 0, len(ctxt.locs))
	var total uint64
	last := ctxt.locs[len(ctxt.locs)-1]
	if len(ctxt.locs) > 1 && !last.explicit {
		return nil, 0, ctxt.exprError(last.offset, errors.New("the final location of a composite expression must have an explicit piece length"))
	}
	if last.explicit && len(ctxt.stack) > 0 {
		offset := len(ctxt.prog) - ctxt.buf.Len()
		return nil, 0, ctxt.exprError(offset, errors.New("the final location of a composite expression must end with DW_OP_piece or DW_OP_bit_piece"))
	}

	for i, loc := range ctxt.locs {
		length := loc.bitLength
		if !loc.explicit {
			if len(ctxt.locs) != 1 {
				return nil, 0, ctxt.exprError(loc.offset, errors.New("the final location of a composite expression must have an explicit piece length"))
			}
			length = bitSize
		}
		if length > bitSize-total {
			return nil, 0, ctxt.exprError(loc.offset, fmt.Errorf("piece length %d bits exceeds object size %d bits", length, bitSize))
		}
		fragments = append(fragments, logicalFragment{
			locationFragment: loc,
			index:            i,
			start:            total,
			length:           length,
		})
		total += length
	}
	return fragments, total, nil
}

func addVote(votes map[physicalKey]bitVote, key physicalKey, value byte, source BitSource) error {
	target := PhysicalTarget{Kind: key.kind, RegNum: key.regNum, Address: key.address, Bit: key.bit}
	if existing, ok := votes[key]; ok {
		if existing.value != value {
			return &AliasConflictError{Target: target, First: existing.source, Second: source}
		}
		return nil
	}
	votes[key] = bitVote{value: value, source: source}
	return nil
}

func registerOriginal(regs *DwarfRegisters, regNum uint64, cache map[uint64][]byte) ([]byte, error) {
	if original, ok := cache[regNum]; ok {
		return original, nil
	}
	reg := regs.Reg(regNum)
	if reg == nil {
		return nil, &MissingRegisterError{RegNum: regNum}
	}
	var original []byte
	if reg.Bytes != nil {
		original = cloneByteSlice(reg.Bytes)
	} else {
		original = make([]byte, 8)
		binary.LittleEndian.PutUint64(original, reg.Uint64Val)
	}
	cache[regNum] = original
	return original, nil
}

func currentRegisterOriginal(regs *DwarfRegisters, regNum uint64) ([]byte, error) {
	reg := regs.Reg(regNum)
	if reg == nil {
		return nil, &MissingRegisterError{RegNum: regNum}
	}
	if reg.Bytes != nil {
		return cloneByteSlice(reg.Bytes), nil
	}
	original := make([]byte, 8)
	binary.LittleEndian.PutUint64(original, reg.Uint64Val)
	return original, nil
}

func pieceSourceBit(piece Piece, sourceBit uint64) (byte, error) {
	if piece.Bytes != nil {
		width := uint64(len(piece.Bytes)) * 8
		if sourceBit >= width {
			return 0, fmt.Errorf("constant source bit %d is outside %d-byte implicit value", sourceBit, len(piece.Bytes))
		}
		return (piece.Bytes[sourceBit/8] >> (sourceBit % 8)) & 1, nil
	}
	if sourceBit >= 64 {
		return 0, fmt.Errorf("stack_value source bit %d is outside 64-bit stack value", sourceBit)
	}
	return byte((piece.Val >> sourceBit) & 1), nil
}

func packedBit(b []byte, bit uint64) bool {
	return packedBitValue(b, bit) == 1
}

func packedBitValue(b []byte, bit uint64) byte {
	return (b[bit/8] >> (bit % 8)) & 1
}

func applyPhysicalBit(data []byte, bit uint64, value byte) {
	mask := byte(1) << (bit % 8)
	index := bit / 8
	if value == 1 {
		data[index] |= mask
	} else {
		data[index] &^= mask
	}
}

func addUint64(a, b uint64) (uint64, bool) {
	if a > math.MaxUint64-b {
		return 0, true
	}
	return a + b, false
}

func bytePieceBitLength(byteLength uint64) (uint64, error) {
	if byteLength > uint64(math.MaxInt) {
		return 0, fmt.Errorf("%w: %d bytes cannot be represented as int", ErrLengthOverflow, byteLength)
	}
	if byteLength > math.MaxUint64/8 {
		return 0, fmt.Errorf("%w: %d bytes", ErrLengthOverflow, byteLength)
	}
	return byteLength * 8, nil
}

func decodeULEB128Checked(r io.ByteReader) (uint64, int, error) {
	var value uint64
	var shift uint
	for n := 0; ; n++ {
		b, err := r.ReadByte()
		if err != nil {
			return value, n, io.ErrUnexpectedEOF
		}
		if shift == 63 {
			if b&0x7e != 0 {
				return value, n + 1, ErrLengthOverflow
			}
			value |= uint64(b&0x01) << 63
		} else if shift < 64 {
			value |= uint64(b&0x7f) << shift
		} else if b&0x7f != 0 {
			return value, n + 1, ErrLengthOverflow
		}
		if b&0x80 == 0 {
			return value, n + 1, nil
		}
		shift += 7
	}
}

func cloneByteSlice(b []byte) []byte {
	if b == nil {
		return nil
	}
	out := make([]byte, len(b))
	copy(out, b)
	return out
}

// RegisterWrites returns copies of complete native-width register records,
// sorted by DWARF register number.
func (p *LocationWritePlan) RegisterWrites() []RegisterWrite {
	if p == nil {
		return nil
	}
	out := make([]RegisterWrite, len(p.registers))
	for i, r := range p.registers {
		out[i] = RegisterWrite{RegNum: r.RegNum, Original: cloneByteSlice(r.Original), Data: cloneByteSlice(r.Data)}
	}
	return out
}

// MemoryWrites returns copies of contiguous memory records, sorted by address.
func (p *LocationWritePlan) MemoryWrites() []MemoryWrite {
	if p == nil {
		return nil
	}
	out := make([]MemoryWrite, len(p.memories))
	for i, m := range p.memories {
		out[i] = MemoryWrite{Address: m.Address, Original: cloneByteSlice(m.Original), Data: cloneByteSlice(m.Data)}
	}
	return out
}

// Writes returns copies of all records in commit order: registers ascending by
// number, followed by memory ranges ascending by address.
func (p *LocationWritePlan) Writes() []PhysicalWrite {
	if p == nil {
		return nil
	}
	out := make([]PhysicalWrite, 0, len(p.registers)+len(p.memories))
	for _, r := range p.registers {
		out = append(out, PhysicalWrite{
			Kind: PhysicalRegisterWrite, RegNum: r.RegNum,
			Original: cloneByteSlice(r.Original), Data: cloneByteSlice(r.Data),
		})
	}
	for _, m := range p.memories {
		out = append(out, PhysicalWrite{
			Kind: PhysicalMemoryWrite, Address: m.Address,
			Original: cloneByteSlice(m.Original), Data: cloneByteSlice(m.Data),
		})
	}
	return out
}

// Records is an alias for Writes.
func (p *LocationWritePlan) Records() []PhysicalWrite { return p.Writes() }

// PhysicalWrites is an alias for Writes.
func (p *LocationWritePlan) PhysicalWrites() []PhysicalWrite { return p.Writes() }

// Commit preflights every record against current values and then submits the
// records in fixed order.
//
// regs.ChangeFunc is used for register records. readMemory is used to recheck
// memory records, and writeMemory submits them. A nil callback is an error
// only when the plan has a record that needs it. The plan is consumed by the
// first call, including a failed preflight or callback attempt.
//
// Completed writes are not rolled back after a later failure. Commit does not
// provide atomicity against external debuggee or snapshot changes made after
// preflight.
func (p *LocationWritePlan) Commit(regs DwarfRegisters, readMemory ReadMemoryFunc, writeMemory func(addr uint64, data []byte) (int, error)) (*CommitResult, error) {
	if p == nil {
		err := errors.New("nil location write plan")
		return &CommitResult{Stage: CommitPreflight, FailedIndex: -1, Err: err}, err
	}
	result := &CommitResult{Stage: CommitPreflight, FailedIndex: -1}
	if !p.used.CompareAndSwap(false, true) {
		result.Err = ErrPlanAlreadyCommitted
		return result, result.Err
	}

	registerCount := len(p.registers)
	if registerCount > 0 && regs.ChangeFunc == nil {
		result.FailedIndex = 0
		result.RecordLength = len(p.registers[0].Data)
		result.Err = fmt.Errorf("%w: regs.ChangeFunc is required for register %d", ErrPreflight, p.registers[0].RegNum)
		return result, result.Err
	}
	if len(p.memories) > 0 && writeMemory == nil {
		idx := registerCount
		result.FailedIndex = idx
		result.RecordLength = len(p.memories[0].Data)
		result.Err = fmt.Errorf("%w: memory write callback is required for address %#x", ErrPreflight, p.memories[0].Address)
		return result, result.Err
	}
	if len(p.memories) > 0 && readMemory == nil {
		idx := registerCount
		result.FailedIndex = idx
		result.RecordLength = len(p.memories[0].Original)
		result.Err = fmt.Errorf("%w: memory read callback is required for address %#x", ErrPreflight, p.memories[0].Address)
		return result, result.Err
	}

	for i := range p.registers {
		record := &p.registers[i]
		current, err := currentRegisterOriginal(&regs, record.RegNum)
		if err != nil {
			result.FailedIndex = i
			result.RecordLength = len(record.Original)
			result.Err = fmt.Errorf("%w: register %d could not be read: %v", ErrPreflight, record.RegNum, err)
			return result, result.Err
		}
		if !bytesEqual(current, record.Original) {
			result.FailedIndex = i
			result.RecordLength = len(record.Original)
			result.Err = fmt.Errorf("%w: register %d changed after planning", ErrPreflight, record.RegNum)
			return result, result.Err
		}
	}

	for i := range p.memories {
		record := &p.memories[i]
		idx := registerCount + i
		current := make([]byte, len(record.Original))
		n, err := readMemory(current, record.Address)
		if err != nil {
			result.FailedIndex = idx
			result.RecordLength = len(record.Original)
			result.Err = fmt.Errorf("%w: reading memory %#x: %v", ErrPreflight, record.Address, err)
			return result, result.Err
		}
		if n != len(current) {
			result.FailedIndex = idx
			result.BytesWritten = n
			result.RecordLength = len(current)
			result.Err = fmt.Errorf("%w: short read at %#x: read %d, wanted %d", ErrPreflight, record.Address, n, len(current))
			return result, result.Err
		}
		if !bytesEqual(current, record.Original) {
			result.FailedIndex = idx
			result.RecordLength = len(record.Original)
			result.Err = fmt.Errorf("%w: memory at %#x changed after planning", ErrPreflight, record.Address)
			return result, result.Err
		}
	}

	result.Stage = CommitWriting
	for i := range p.registers {
		idx := i
		record := &p.registers[i]
		newReg := DwarfRegisterFromBytes(cloneByteSlice(record.Data))
		if err := regs.ChangeFunc(record.RegNum, newReg); err != nil {
			result.FailedIndex = idx
			result.RecordLength = len(record.Data)
			result.Err = fmt.Errorf("register %d write failed: %w", record.RegNum, err)
			return result, result.Err
		}
		result.CompletedRecords = append(result.CompletedRecords, idx)
	}

	for i := range p.memories {
		idx := registerCount + i
		record := &p.memories[i]
		payload := cloneByteSlice(record.Data)
		n, err := writeMemory(record.Address, payload)
		if err != nil {
			result.FailedIndex = idx
			result.BytesWritten = n
			result.RecordLength = len(record.Data)
			result.Err = fmt.Errorf("memory write at %#x failed after %d bytes: %w", record.Address, n, err)
			return result, result.Err
		}
		if n != len(record.Data) {
			result.FailedIndex = idx
			result.BytesWritten = n
			result.RecordLength = len(record.Data)
			result.Err = fmt.Errorf("%w at %#x: wrote %d of %d bytes", ErrShortWrite, record.Address, n, len(record.Data))
			return result, result.Err
		}
		result.CompletedRecords = append(result.CompletedRecords, idx)
	}

	result.FailedIndex = -1
	return result, nil
}

func bytesEqual(a, b []byte) bool {
	if len(a) != len(b) {
		return false
	}
	for i := range a {
		if a[i] != b[i] {
			return false
		}
	}
	return true
}
