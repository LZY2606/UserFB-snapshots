package op

import (
	"bytes"
	"encoding/binary"
	"errors"
	"fmt"
	"sort"
	"sync"
)

// This file implements a two-phase, bit-piece-aware location writer.
//
// The first phase (PlanLocationWrite) evaluates the location expression with
// the same opcode interpreter and terminator logic as ExecuteStackProgram,
// translates the masked logical assignment into concrete physical bit
// changes (register bits or memory bytes), resolves physical aliases and
// snapshots every byte that would be touched. No write callback is involved
// and the debugged program is never started.
//
// The second phase (LocationWritePlan.Commit) re-checks every snapshotted
// byte against the current registers/memory, then applies the prepared
// records in a fixed order. A plan can be committed at most once.

// maxLocationWriteBits bounds the size of a single planned write.
const maxLocationWriteBits = 1 << 20

// PhysicalTargetKind identifies what a LocationWriteRecord modifies.
type PhysicalTargetKind uint8

const (
	// PhysicalRegister is a full, native-width register write.
	PhysicalRegister PhysicalTargetKind = iota
	// PhysicalMemory is a contiguous memory byte range.
	PhysicalMemory
)

// LocationWriteRecord is one prepared physical modification.
//
// For Kind == PhysicalRegister, RegNum is the register number and Addr is
// zero; the record carries the complete register contents (native width),
// with every unselected bit preserved from the planning snapshot.
//
// For Kind == PhysicalMemory, Addr is the start address and RegNum is zero.
// Adjacent modified bytes are coalesced into one record, but a record never
// extends across an unmodified byte.
//
// Orig are the bytes read during planning, Data the bytes that will be
// written. Both slices are owned by the plan; the copies returned by
// LocationWritePlan.Records are independent and cannot affect the commit.
type LocationWriteRecord struct {
	Kind   PhysicalTargetKind
	RegNum uint64
	Addr   uint64
	Orig   []byte
	Data   []byte
}

// LocationWritePlan is a prepared, not yet applied set of physical
// modifications. Plans are safe to inspect concurrently but may only be
// committed once; after Commit (successful or not) the plan is spent.
type LocationWritePlan struct {
	records []LocationWriteRecord

	mu   sync.Mutex
	used bool
}

// Records returns a deep copy of the planned physical modifications, ordered
// by ascending register number followed by ascending memory address.
func (p *LocationWritePlan) Records() []LocationWriteRecord {
	out := make([]LocationWriteRecord, len(p.records))
	for i, r := range p.records {
		out[i] = cloneRecord(r)
	}
	return out
}

// LocationWriteResult reports how far a Commit got.
//
// Completed holds the records applied before the current one, Current (when
// non-nil) is the record that was in progress, CurrentWritten is the number
// of bytes from Current that the write callback actually accepted, and
// FirstError is the first error encountered. No rollback is attempted and
// atomicity against concurrent external writers is not promised.
type LocationWriteResult struct {
	Completed      []LocationWriteRecord
	Current        *LocationWriteRecord
	CurrentWritten int
	FirstError     error
}

// PlanLocationWrite turns a masked logical assignment into a plan of
// consistent physical modifications. Nothing is written here and no
// debugged process is started.
//
// instructions is a DWARF location expression using the same opcode set as
// ExecuteStackProgram, including DW_OP_piece (length in bytes) and
// DW_OP_bit_piece (length in bits, second ULEB128 operand is the source bit
// offset). bitSize is the width of the logical object in bits; data and
// mask must each be ceil(bitSize/8) bytes long, laid out little-endian with
// the least significant bit first. A set mask bit selects the matching
// variable bit; mask bits above bitSize must be zero.
//
// Logical pieces are concatenated bit by bit without per-piece byte
// padding. Register, stack_value and implicit_value bits are numbered from
// the least significant bit; memory pieces count upward from the start
// address, low bit first inside each byte. Unavailable (empty) pieces and
// bits beyond the object's declared length cannot be selected. Constant
// pieces only accept requests equal to their value and produce no writes.
//
// When two selected logical bits land on the same physical bit, equal
// requested values collapse into one modification and conflicting values
// make planning fail, naming both logical sources; piece ordering never
// silently overwrites anything.
//
// Support range: little-endian targets, ptrSize 4 or 8, 1 <= bitSize <=
// 1<<20. Registers prefer Bytes (vector values wider than eight bytes are
// supported) and fall back to the eight-byte Uint64Val when Bytes is nil;
// missing registers go through the existing Reg lazy-loading mechanism.
// Data sources of pieces the request does not touch are never read.
func PlanLocationWrite(regs DwarfRegisters, instructions []byte, ptrSize int, bitSize uint64, data, mask []byte, readMemory ReadMemoryFunc) (*LocationWritePlan, error) {
	if ptrSize != 4 && ptrSize != 8 {
		return nil, fmt.Errorf("PlanLocationWrite: unsupported ptrSize %d (want 4 or 8)", ptrSize)
	}
	if bitSize == 0 || bitSize > maxLocationWriteBits {
		return nil, fmt.Errorf("PlanLocationWrite: bitSize %d out of range (1..%d)", bitSize, maxLocationWriteBits)
	}
	want := int((bitSize + 7) / 8)
	if len(data) != want || len(mask) != want {
		return nil, fmt.Errorf("PlanLocationWrite: data/mask must be %d bytes for %d bits, got %d/%d", want, bitSize, len(data), len(mask))
	}
	if bo := regs.ByteOrder; bo != nil && bo != binary.LittleEndian {
		return nil, fmt.Errorf("PlanLocationWrite: only little-endian targets are supported")
	}
	if rem := bitSize % 8; rem != 0 {
		if pad := mask[want-1] >> rem; pad != 0 {
			return nil, fmt.Errorf("PlanLocationWrite: mask has set bits beyond bitSize (%d)", bitSize)
		}
	}

	b := &planBuilder{
		regs:       regs,
		readMemory: readMemory,
		data:       data,
		mask:       mask,
		bitSize:    bitSize,
		regBits:    make(map[uint64]*regTarget),
		memBytes:   make(map[uint64]*memByte),
	}

	// Evaluate through the shared interpreter; only the piece interpretation
	// differs (ctxt.bitPieces), the opcode handlers and terminator flow are
	// exactly those used by ExecuteStackProgram.
	ctxt := &context{
		buf:            bytes.NewBuffer(instructions),
		prog:           instructions,
		stack:          make([]int64, 0, 3),
		DwarfRegisters: regs,
		ptrSize:        ptrSize,
		readMemory:     readMemory,
		bitPieces:      true,
	}
	if err := ctxt.run(); err != nil {
		return nil, err
	}

	pieces, err := b.normalizePieces(ctxt)
	if err != nil {
		return nil, err
	}
	if err := b.mapPieces(pieces); err != nil {
		return nil, err
	}
	records, err := b.buildRecords()
	if err != nil {
		return nil, err
	}
	return &LocationWritePlan{records: records}, nil
}

// planBuilder accumulates the physical bit assignments of one plan.
type planBuilder struct {
	regs       DwarfRegisters
	readMemory ReadMemoryFunc
	data       []byte
	mask       []byte
	bitSize    uint64

	regBits  map[uint64]*regTarget
	memBytes map[uint64]*memByte
}

type bitSource struct {
	logicalBit uint64
	pieceIndex int
}

type regTarget struct {
	orig []byte // planning-time full register contents
	bits map[uint64]bitAssign
}

type bitAssign struct {
	value  uint8
	source bitSource
}

type memByte struct {
	addr uint64
	orig uint8
	bits map[uint8]bitAssign // bit position 0..7 within the byte
}

// normalizePieces converts the interpreter output into pieces with explicit
// bit lengths and validates the combined length against the logical object.
func (b *planBuilder) normalizePieces(ctxt *context) ([]Piece, error) {
	if ctxt.pieces == nil {
		if len(ctxt.stack) == 0 {
			return nil, errors.New("empty OP stack")
		}
		// A plain address expression is one memory-sized location covering
		// the whole object.
		return []Piece{{Kind: AddrPiece, Val: uint64(ctxt.stack[len(ctxt.stack)-1]), BitSize: b.bitSize}}, nil
	}

	if len(ctxt.stack) != 0 {
		// Every piece terminator clears the operand stack, so values left
		// over after a composite expression mean a trailing fragment with
		// no terminator: the expression is malformed.
		return nil, errors.New("trailing location expression is missing a piece terminator")
	}

	pieces := ctxt.pieces
	var cursor uint64
	for i := range pieces {
		p := &pieces[i]
		if !p.bitSized {
			if len(pieces) != 1 {
				return nil, fmt.Errorf("piece %d is missing a piece terminator", i)
			}
			p.BitSize = b.bitSize
		}
		if p.BitSize > b.bitSize-cursor {
			return nil, fmt.Errorf("piece %d extends past the %d-bit object (combined length overflows)", i, b.bitSize)
		}
		cursor += p.BitSize
		if cursor > b.bitSize {
			return nil, fmt.Errorf("piece %d extends past the %d-bit object (combined length %d)", i, b.bitSize, cursor)
		}
	}
	if cursor < b.bitSize {
		// The tail of the object is unavailable; selecting any of its bits
		// is rejected while mapping pieces.
		pieces = append(pieces, Piece{Kind: ImmPiece, unavailable: true, BitSize: b.bitSize - cursor, bitSized: true})
	}
	return pieces, nil
}

// mapPieces walks every selected logical bit and records the physical bit it
// lands on, enforcing alias consistency and source-width limits.
func (b *planBuilder) mapPieces(pieces []Piece) error {
	var lo uint64
	for pi, p := range pieces {
		hi := lo + p.BitSize
		if p.unavailable {
			for l := lo; l < hi; l++ {
				if bitAt(b.mask, l) != 0 {
					return fmt.Errorf("logical bit %d (piece %d) is unavailable", l, pi)
				}
			}
			lo = hi
			continue
		}

		for j := uint64(0); j < p.BitSize; j++ {
			l := lo + j
			if bitAt(b.mask, l) == 0 {
				continue // unselected bits impose no constraint
			}
			v := bitAt(b.data, l)
			src := bitSource{logicalBit: l, pieceIndex: pi}
			switch p.Kind {
			case RegPiece:
				bitPos, overflow := addChecked(p.BitOffset, j)
				if overflow {
					return fmt.Errorf("piece %d bit offset overflows", pi)
				}
				if err := b.assignRegBit(p.Val, bitPos, v, src); err != nil {
					return err
				}
			case AddrPiece:
				bitPos, overflow := addChecked(p.BitOffset, j)
				if overflow {
					return fmt.Errorf("piece %d bit offset overflows", pi)
				}
				if err := b.assignMemBit(p.Val, bitPos, v, src); err != nil {
					return err
				}
			case ImmPiece:
				bitPos, overflow := addChecked(p.BitOffset, j)
				if overflow {
					return fmt.Errorf("piece %d bit offset overflows", pi)
				}
				if err := b.checkConstBit(p, bitPos, v, src); err != nil {
					return err
				}
			default:
				return fmt.Errorf("piece %d has unknown kind %d", pi, p.Kind)
			}
		}
		lo = hi
	}
	return nil
}

func (b *planBuilder) assignRegBit(regNum, bitPos uint64, value uint8, src bitSource) error {
	t := b.regBits[regNum]
	if t == nil {
		raw, err := registerSnapshot(&b.regs, regNum)
		if err != nil {
			return err
		}
		t = &regTarget{orig: raw, bits: make(map[uint64]bitAssign)}
		b.regBits[regNum] = t
	}
	if bitPos >= uint64(len(t.orig))*8 {
		return fmt.Errorf("logical bit %d (piece %d) is beyond register %d width (%d bytes)", src.logicalBit, src.pieceIndex, regNum, len(t.orig))
	}
	if prev, ok := t.bits[bitPos]; ok && prev.value != value {
		return aliasConflict("register", fmt.Sprintf("register %d bit %d", regNum, bitPos), prev.source, src)
	}
	t.bits[bitPos] = bitAssign{value: value, source: src}
	return nil
}

func (b *planBuilder) assignMemBit(base, bitPos uint64, value uint8, src bitSource) error {
	byteOff, bitInByte := bitPos/8, bitPos%8
	addr, overflow := addChecked(base, byteOff)
	if overflow {
		return fmt.Errorf("logical bit %d (piece %d) overflows the address space (base %#x + %d)", src.logicalBit, src.pieceIndex, base, byteOff)
	}
	m := b.memBytes[addr]
	if m == nil {
		orig, err := b.readMemByte(addr)
		if err != nil {
			return err
		}
		m = &memByte{addr: addr, orig: orig, bits: make(map[uint8]bitAssign)}
		b.memBytes[addr] = m
	}
	if prev, ok := m.bits[uint8(bitInByte)]; ok && prev.value != value {
		return aliasConflict("memory", fmt.Sprintf("address %#x bit %d", addr, bitInByte), prev.source, src)
	}
	m.bits[uint8(bitInByte)] = bitAssign{value: value, source: src}
	return nil
}

func (b *planBuilder) checkConstBit(p Piece, bitPos uint64, value uint8, src bitSource) error {
	origBit, err := constBit(p, bitPos)
	if err != nil {
		return fmt.Errorf("logical bit %d (piece %d): %w", src.logicalBit, src.pieceIndex, err)
	}
	if value != origBit {
		return fmt.Errorf("logical bit %d (piece %d) requests %d but the constant source holds %d", src.logicalBit, src.pieceIndex, value, origBit)
	}
	return nil
}

// buildRecords snapshots all affected memory, applies the assignments to
// plan-owned copies and produces the fixed-order record list.
func (b *planBuilder) buildRecords() ([]LocationWriteRecord, error) {
	records := make([]LocationWriteRecord, 0, len(b.regBits))

	regNums := make([]uint64, 0, len(b.regBits))
	for n := range b.regBits {
		regNums = append(regNums, n)
	}
	sort.Slice(regNums, func(i, j int) bool { return regNums[i] < regNums[j] })
	for _, n := range regNums {
		t := b.regBits[n]
		data := append([]byte(nil), t.orig...)
		for bitPos, a := range t.bits {
			if a.value != 0 {
				data[bitPos/8] |= 1 << (bitPos % 8)
			} else {
				data[bitPos/8] &^= 1 << (bitPos % 8)
			}
		}
		records = append(records, LocationWriteRecord{Kind: PhysicalRegister, RegNum: n, Orig: t.orig, Data: data})
	}

	addrs := make([]uint64, 0, len(b.memBytes))
	for a := range b.memBytes {
		addrs = append(addrs, a)
	}
	sort.Slice(addrs, func(i, j int) bool { return addrs[i] < addrs[j] })

	// Coalesce runs of adjacent modified bytes; a gap closes the record so
	// ranges never extend over bytes nobody selected.
	for i := 0; i < len(addrs); {
		j := i + 1
		for j < len(addrs) && addrs[j] == addrs[j-1]+1 {
			j++
		}
		size := int(addrs[j-1] - addrs[i] + 1)
		orig := make([]byte, size)
		data := make([]byte, size)
		for k := i; k < j; k++ {
			m := b.memBytes[addrs[k]]
			idx := int(addrs[k] - addrs[i])
			orig[idx] = m.orig
			b := m.orig
			for bitPos, a := range m.bits {
				if a.value != 0 {
					b |= 1 << bitPos
				} else {
					b &^= 1 << bitPos
				}
			}
			data[idx] = b
		}
		records = append(records, LocationWriteRecord{Kind: PhysicalMemory, Addr: addrs[i], Orig: orig, Data: data})
		i = j
	}
	return records, nil
}

func (b *planBuilder) readMemByte(addr uint64) (byte, error) {
	if b.readMemory == nil {
		return 0, ErrMemoryReadUnavailable
	}
	buf := make([]byte, 1)
	n, err := b.readMemory(buf, addr)
	if err != nil {
		return 0, fmt.Errorf("read memory at %#x: %w", addr, err)
	}
	if n != 1 {
		return 0, fmt.Errorf("short read at %#x: got %d byte(s), want 1", addr, n)
	}
	return buf[0], nil
}

// Commit verifies the planning snapshot against the current registers and
// memory and, if nothing changed, applies every record in plan order.
//
// Register writes go through regs.ChangeFunc; memory writes through
// writeMemory with the exact addresses captured at planning time (the
// location expression is not evaluated again). Preflight failures reject
// the commit before the first write. A callback error or short write stops
// the commit and is reported through the returned LocationWriteResult; the
// plan cannot be committed again either way.
func (p *LocationWritePlan) Commit(regs DwarfRegisters, readMemory ReadMemoryFunc, writeMemory func(addr uint64, data []byte) (int, error)) (*LocationWriteResult, error) {
	p.mu.Lock()
	defer p.mu.Unlock()
	if p.used {
		return nil, errors.New("LocationWritePlan already committed")
	}
	p.used = true

	hasReg, hasMem := false, false
	for _, r := range p.records {
		switch r.Kind {
		case PhysicalRegister:
			hasReg = true
		case PhysicalMemory:
			hasMem = true
		}
	}
	if hasReg && regs.ChangeFunc == nil {
		return nil, errors.New("commit requires regs.ChangeFunc for register records")
	}
	if hasMem && writeMemory == nil {
		return nil, errors.New("commit requires a memory write callback for memory records")
	}

	// Preflight: every snapshotted byte must still match, and every needed
	// callback must be present, before any write happens.
	for _, r := range p.records {
		switch r.Kind {
		case PhysicalRegister:
			cur, err := registerSnapshot(&regs, r.RegNum)
			if err != nil {
				return nil, fmt.Errorf("preflight failed: %w", err)
			}
			if len(cur) != len(r.Orig) || !bytes.Equal(cur, r.Orig) {
				return nil, fmt.Errorf("preflight failed: register %d changed since planning", r.RegNum)
			}
		case PhysicalMemory:
			if readMemory == nil {
				return nil, ErrMemoryReadUnavailable
			}
			cur := make([]byte, len(r.Orig))
			n, err := readMemory(cur, r.Addr)
			if err != nil {
				return nil, fmt.Errorf("preflight failed: read memory at %#x: %w", r.Addr, err)
			}
			if n != len(r.Orig) {
				return nil, fmt.Errorf("preflight failed: short read at %#x: got %d of %d", r.Addr, n, len(r.Orig))
			}
			if !bytes.Equal(cur, r.Orig) {
				return nil, fmt.Errorf("preflight failed: memory at %#x changed since planning", r.Addr)
			}
		}
	}

	res := &LocationWriteResult{}
	for i := range p.records {
		r := p.records[i]
		switch r.Kind {
		case PhysicalRegister:
			// Registers have no partial-write semantics: either the change
			// callback accepts the full native-width value or it fails.
			if err := regs.ChangeFunc(r.RegNum, DwarfRegisterFromBytes(append([]byte(nil), r.Data...))); err != nil {
				res.Current = ptrRecord(cloneRecord(r))
				res.CurrentWritten = 0
				res.FirstError = fmt.Errorf("write register %d: %w", r.RegNum, err)
				return res, res.FirstError
			}
		case PhysicalMemory:
			n, err := writeMemory(r.Addr, append([]byte(nil), r.Data...))
			if err != nil {
				res.Current = ptrRecord(cloneRecord(r))
				res.CurrentWritten = n
				res.FirstError = fmt.Errorf("write memory at %#x: %w", r.Addr, err)
				return res, res.FirstError
			}
			if n != len(r.Data) {
				res.Current = ptrRecord(cloneRecord(r))
				res.CurrentWritten = n
				res.FirstError = fmt.Errorf("short write at %#x: wrote %d of %d bytes", r.Addr, n, len(r.Data))
				return res, res.FirstError
			}
		}
		res.Completed = append(res.Completed, cloneRecord(r))
	}
	return res, nil
}

func ptrRecord(r LocationWriteRecord) *LocationWriteRecord { return &r }

func cloneRecord(r LocationWriteRecord) LocationWriteRecord {
	out := r
	if r.Orig != nil {
		out.Orig = append([]byte(nil), r.Orig...)
	}
	if r.Data != nil {
		out.Data = append([]byte(nil), r.Data...)
	}
	return out
}

// registerSnapshot returns a plan-owned copy of a register's full native
// width. Bytes is preferred (it may hold vector values wider than eight
// bytes); when Bytes is nil the eight-byte little-endian Uint64Val is used.
// Missing registers go through Reg's lazy-loading mechanism. The caller's
// DwarfRegister objects are never modified.
func registerSnapshot(regs *DwarfRegisters, regNum uint64) ([]byte, error) {
	reg := regs.Reg(regNum)
	if reg == nil {
		return nil, fmt.Errorf("register %d is not available", regNum)
	}
	if reg.Bytes != nil {
		return append([]byte(nil), reg.Bytes...), nil
	}
	raw := make([]byte, 8)
	binary.LittleEndian.PutUint64(raw, reg.Uint64Val)
	return raw, nil
}

// bitAt returns bit i (LSB first) of the little-endian buffer b.
func bitAt(b []byte, i uint64) uint8 {
	return (b[i/8] >> (i % 8)) & 1
}

func addChecked(a, b uint64) (uint64, bool) {
	s := a + b
	if s < a {
		return 0, true
	}
	return s, false
}

// constBit extracts bit bitPos from a constant piece's source value and
// rejects positions beyond the source width.
func constBit(p Piece, bitPos uint64) (uint8, error) {
	var src []byte
	if p.Bytes != nil {
		src = p.Bytes
	} else {
		src = make([]byte, 8)
		binary.LittleEndian.PutUint64(src, p.Val)
	}
	if bitPos >= uint64(len(src))*8 {
		return 0, fmt.Errorf("bit %d is beyond the constant source width (%d bits)", bitPos, len(src)*8)
	}
	return bitAt(src, bitPos), nil
}

func aliasConflict(physType, loc string, prev, cur bitSource) error {
	return fmt.Errorf("write plan conflict: logical bit %d (piece %d) and logical bit %d (piece %d) alias the same %s location (%s) but request different values",
		prev.logicalBit, prev.pieceIndex, cur.logicalBit, cur.pieceIndex, physType, loc)
}
