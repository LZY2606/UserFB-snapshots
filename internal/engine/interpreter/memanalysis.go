// This file implements the dynamic memory-corruption analysis instrumentation
// used by experimental/memanalysis. It is deliberately an internal, narrowly
// scoped facility rather than a general provenance API: trackers are only
// created by the memanalysis package and only carry the state its manifest
// requires. Normal interpreter execution leaves the tracker nil and follows
// the original code paths.

package interpreter

import (
	"errors"
	"sort"

	"github.com/tetratelabs/wazero/api"
	"github.com/tetratelabs/wazero/internal/wasm"
	"github.com/tetratelabs/wazero/internal/wasmruntime"
)

// errNotInterpreterEngine is returned when memanalysis is attached to a
// non-interpreter function.
var errNotInterpreterEngine = errors.New("memanalysis: function is not an interpreter call engine")

// valueTag is the provenance attached to every i32 value on the parallel
// operand stack. Both slices are always sorted and de-duplicated. A zero
// valueTag denotes a clean value with no input-source or write origin.
type valueTag struct {
	sources []string
	writes  []uint32
}

// unionTag returns the sorted, de-duplicated union of the two tags.
func unionTag(a, b valueTag) valueTag {
	if len(a.sources) == 0 && len(a.writes) == 0 {
		return b.clone()
	} else if len(b.sources) == 0 && len(b.writes) == 0 {
		return a.clone()
	}
	return valueTag{
		sources: mergeStrings(a.sources, b.sources),
		writes:  mergeUint32(a.writes, b.writes),
	}
}

func (t valueTag) clone() valueTag {
	if len(t.sources) == 0 && len(t.writes) == 0 {
		return valueTag{}
	}
	return valueTag{sources: append([]string(nil), t.sources...), writes: append([]uint32(nil), t.writes...)}
}

func mergeStrings(a, b []string) []string {
	if len(a) == 0 {
		return append([]string(nil), b...)
	} else if len(b) == 0 {
		return append([]string(nil), a...)
	}
	out := make([]string, 0, len(a)+len(b))
	i, j := 0, 0
	for i < len(a) && j < len(b) {
		switch {
		case a[i] < b[j]:
			out = append(out, a[i])
			i++
		case a[i] > b[j]:
			out = append(out, b[j])
			j++
		default:
			out = append(out, a[i])
			i++
			j++
		}
	}
	out = append(out, a[i:]...)
	out = append(out, b[j:]...)
	return out
}

func mergeUint32(a, b []uint32) []uint32 {
	if len(a) == 0 {
		return append([]uint32(nil), b...)
	} else if len(b) == 0 {
		return append([]uint32(nil), a...)
	}
	out := make([]uint32, 0, len(a)+len(b))
	i, j := 0, 0
	for i < len(a) && j < len(b) {
		switch {
		case a[i] < b[j]:
			out = append(out, a[i])
			i++
		case a[i] > b[j]:
			out = append(out, b[j])
			j++
		default:
			out = append(out, a[i])
			i++
			j++
		}
	}
	out = append(out, a[i:]...)
	out = append(out, b[j:]...)
	return out
}

// MAObject is the internal view of an experimental/memanalysis.Object.
type MAObject struct {
	ID   string
	Base uint32
	Size uint32
}

// MASource is the internal view of an experimental/memanalysis.Source.
type MASource struct {
	ID     string
	Offset uint32
	Length uint32
}

// MATarget binds a write instruction, identified by its local function index
// and the instruction's offset relative to the start of the code section, to
// the manifest object it was intended to write, together with the opcode name
// reported in events.
type MATarget struct {
	Object string
	Opcode string
}

// MAConfig configures a MemAnalysisTracker. All fields are validated by the
// memanalysis package before the tracker is constructed.
type MAConfig struct {
	Memory      *wasm.MemoryInstance
	GlobalCount int
	Objects     []MAObject
	Sources     []MASource
	Targets     map[MASiteKey]MATarget
}

// MASiteKey identifies a write operation through its function index and code
// section offset, both of which are available directly from a running frame.
type MASiteKey struct {
	FunctionIndex  uint32
	CodeSectionOff uint64
}

// TrackedEvent is the internal representation of a single executed write
// instruction, consumed by the memanalysis package when building the report.
type TrackedEvent struct {
	ID               uint32
	CallIndex        uint32
	FunctionIndex    uint32
	BodyOffset       uint32
	Opcode           string
	TargetObject     string
	Dst              uint64
	Length           uint64
	Src              uint64
	HasSrc           bool
	Kind             string
	Committed        bool
	HasFirstInvalid  bool
	FirstInvalid     uint64
	Side             string
	AddressTag       valueTag
	LengthTag        valueTag
	DataTag          valueTag
	SourceAddressTag valueTag
	Victims          []string
}

// ValueTagView is the exported, read-only view of an internal valueTag.
type ValueTagView struct {
	Sources []string
	Writes  []uint32
}

func (t valueTag) view() ValueTagView {
	return ValueTagView{Sources: t.sources, Writes: t.writes}
}

// AddressOrigin returns the provenance of the write's effective address.
func (e *TrackedEvent) AddressOrigin() ValueTagView { return e.AddressTag.view() }

// LengthOriginView returns the provenance of the write's length operand.
func (e *TrackedEvent) LengthOriginView() ValueTagView { return e.LengthTag.view() }

// DataOrigin returns the provenance of the written value or copied bytes.
func (e *TrackedEvent) DataOrigin() ValueTagView { return e.DataTag.view() }

// SourceAddressOrigin returns the provenance of a memory.copy source address.
func (e *TrackedEvent) SourceAddressOrigin() ValueTagView { return e.SourceAddressTag.view() }

// MemAnalysisTracker carries the provenance state that persists across the
// exported calls of one Analyze run: per-byte memory origins, per-global
// origins, the manifest bindings and the emitted events.
type MemAnalysisTracker struct {
	mem     *wasm.MemoryInstance
	objects []MAObject
	targets map[MASiteKey]MATarget

	// memSources[i] holds the input-source IDs of linear-memory byte i; a nil
	// entry means the byte currently has no input origin.
	memSources [][]string
	// memWrites[i] holds the most recent successful write event touching byte
	// i, or zero when the byte was never written by an analyzed instruction.
	memWrites []uint32

	// globalTags parallels the module's global instances.
	globalTags []valueTag

	events    []*TrackedEvent
	nextEvent uint32
}

// NewMemAnalysisTracker creates a tracker for the given configuration,
// seeding the input-source byte origins after the module's active data
// segments have been applied.
func NewMemAnalysisTracker(cfg MAConfig) *MemAnalysisTracker {
	t := &MemAnalysisTracker{
		mem:        cfg.Memory,
		objects:    cfg.Objects,
		targets:    cfg.Targets,
		memSources: make([][]string, len(cfg.Memory.Buffer)),
		memWrites:  make([]uint32, len(cfg.Memory.Buffer)),
		globalTags: make([]valueTag, cfg.GlobalCount),
	}
	for _, s := range cfg.Sources {
		for a := s.Offset; a < s.Offset+s.Length; a++ {
			t.memSources[a] = []string{s.ID}
		}
	}
	return t
}

// Events returns the events emitted so far, in id order.
func (t *MemAnalysisTracker) Events() []*TrackedEvent { return t.events }

// ForceSourceOffsets makes the interpreter compiler retain every operation's
// code-section offset even when the module carries no DWARF section. The
// analysis needs those offsets to locate write instructions.
func ForceSourceOffsets(m *wasm.Module) {
	m.ForceSourceOffsets()
}

// AttachMemAnalysis binds a tracker to an exported function's call engine for
// the duration of one exported call. It is only callable with interpreter
// engines produced by wazero.NewRuntimeConfigInterpreter.
func AttachMemAnalysis(fn api.Function, tracker *MemAnalysisTracker, callIndex uint32) error {
	ce, ok := fn.(*callEngine)
	if !ok {
		return errNotInterpreterEngine
	}
	ce.ma = tracker
	ce.maCall = callIndex
	ce.maTags = ce.maTags[:0]
	return nil
}

// DetachMemAnalysis clears the per-call binding. Tagged temporaries are
// dropped so a trap cannot leak labels into the next call.
func DetachMemAnalysis(fn api.Function) {
	if ce, ok := fn.(*callEngine); ok {
		ce.ma = nil
		ce.maCall = 0
		ce.maTags = ce.maTags[:0]
	}
}

// grow extends the per-byte origin arrays after a successful memory.grow. The
// new bytes carry no origin.
func (t *MemAnalysisTracker) grow() {
	n := len(t.mem.Buffer)
	if n > len(t.memSources) {
		t.memSources = append(t.memSources, make([][]string, n-len(t.memSources))...)
		t.memWrites = append(t.memWrites, make([]uint32, n-len(t.memWrites))...)
	}
}

// byteTag returns the full provenance of a single linear-memory byte.
func (t *MemAnalysisTracker) byteTag(addr uint64) valueTag {
	var writes []uint32
	if w := t.memWrites[addr]; w != 0 {
		writes = []uint32{w}
	}
	return valueTag{sources: append([]string(nil), t.memSources[addr]...), writes: writes}
}

// loadTag unions the origins of the width bytes starting at addr, mirroring
// the bytes an i32 load actually reads.
func (t *MemAnalysisTracker) loadTag(addr uint64, width uint32) valueTag {
	var out valueTag
	for i := uint64(0); i < uint64(width); i++ {
		out = unionTag(out, t.byteTag(addr+i))
	}
	return out
}

// --- parallel operand-stack helpers, all nil-tracker safe ---

func (ce *callEngine) maPush(t valueTag) {
	if ce.ma == nil {
		return
	}
	ce.maTags = append(ce.maTags, t)
}

func (ce *callEngine) maPop() valueTag {
	n := len(ce.maTags)
	t := ce.maTags[n-1]
	ce.maTags = ce.maTags[:n-1]
	return t
}

func (ce *callEngine) maBinary() {
	if ce.ma == nil {
		return
	}
	v2 := ce.maPop()
	v1 := ce.maPop()
	ce.maPush(unionTag(v1, v2))
}

func (ce *callEngine) maDrop(raw uint64) {
	if ce.ma == nil {
		return
	}
	r := inclusiveRangeFromU64(raw)
	if r.Start == -1 {
		return
	} else if r.Start == 0 {
		ce.maTags = ce.maTags[:int32(len(ce.maTags))-1-r.End]
	} else {
		newStack := ce.maTags[:int32(len(ce.maTags))-1-r.End]
		newStack = append(newStack, ce.maTags[int32(len(ce.maTags))-r.Start:]...)
		ce.maTags = newStack
	}
}

func (ce *callEngine) maPick(depth uint64) {
	if ce.ma == nil {
		return
	}
	index := len(ce.maTags) - 1 - int(depth)
	ce.maPush(ce.maTags[index].clone())
}

func (ce *callEngine) maSet(depth uint64) {
	if ce.ma == nil {
		return
	}
	index := len(ce.maTags) - 1 - int(depth)
	ce.maTags[index] = ce.maPop()
}

// maLoad mirrors an i32 load of the given width, consuming the address origin
// and producing the origins of the bytes actually read. It returns the
// resolved effective address so the numeric path performs only one stack pop.
func (ce *callEngine) maLoad(op *unionOperation, width uint32) uint32 {
	if ce.ma == nil {
		return ce.popMemoryOffset(op)
	}
	_ = ce.maPop() // address origin; loads only propagate the bytes read
	offset := ce.popMemoryOffset(op)
	ce.maPush(ce.ma.loadTag(uint64(offset), width))
	return offset
}

// maLoad8 mirrors a width-1 load.
func (ce *callEngine) maLoad8(op *unionOperation) uint32 { return ce.maLoad(op, 1) }

// maLoad16 mirrors a width-2 load.
func (ce *callEngine) maLoad16(op *unionOperation) uint32 { return ce.maLoad(op, 2) }

// maBodyOffset computes the Body offset of the instruction at the current
// frame position, excluding the local declaration vector.
func maBodyOffset(frame *callFrame) uint32 {
	parent := frame.f.parent
	return uint32(parent.offsetsInWasmBinary[frame.pc] -
		parent.source.CodeSection[parent.index].BodyOffsetInCodeSection)
}

// maBeginEvent allocates the next event id.
func (t *MemAnalysisTracker) maBeginEvent(callIndex uint32) *TrackedEvent {
	t.nextEvent++
	return &TrackedEvent{ID: t.nextEvent, CallIndex: callIndex, Victims: []string{}}
}

func (t *MemAnalysisTracker) object(id string) MAObject {
	for _, o := range t.objects {
		if o.ID == id {
			return o
		}
	}
	return MAObject{}
}

// firstOutOfBounds returns the lowest address outside [0, limit) in the
// interval starting at start.
func firstOutOfBounds(start, limit uint64) uint64 {
	if start >= limit {
		return start
	}
	return limit
}

// classifyObject checks the bound-object containment of a non-empty interval,
// assuming the linear-memory checks already passed.
func (t *MemAnalysisTracker) classifyObject(bound MAObject, dst, length uint64) (kind string, firstInvalid uint64, hasFirst bool) {
	if length == 0 {
		return "legal", 0, false
	}
	base, end := uint64(bound.Base), uint64(bound.Base)+uint64(bound.Size)
	if dst < base || dst >= end {
		// The interval start is already outside the allowed half-open range.
		return "object-overflow", dst, true
	}
	if dst+length > end {
		// The prefix lies inside the object; the first offending byte is its end.
		return "object-overflow", end, true
	}
	return "legal", 0, false
}

// victims returns the sorted IDs of declared objects other than boundID
// actually overlapped by the committed interval [dst, dst+length).
func (t *MemAnalysisTracker) victims(boundID string, dst, length uint64) []string {
	if length == 0 {
		return []string{}
	}
	var out []string
	for _, o := range t.objects {
		if o.ID == boundID {
			continue
		}
		ob, oe := uint64(o.Base), uint64(o.Base)+uint64(o.Size)
		if dst < oe && dst+length > ob {
			out = append(out, o.ID)
		}
	}
	sort.Strings(out)
	return out
}

// maStore implements the fully instrumented fixed-width store path.
func (ce *callEngine) maStore(memoryInst *wasm.MemoryInstance, op *unionOperation, width uint32, frame *callFrame) {
	t := ce.ma
	val := ce.popValue()
	valTag := ce.maPop()
	addr := ce.popValue()
	addrTag := ce.maPop()
	dst := op.U2 + addr // wide sum; never truncated before the boundary check

	key := MASiteKey{FunctionIndex: frame.f.parent.index, CodeSectionOff: frame.f.parent.offsetsInWasmBinary[frame.pc]}
	target := t.targets[key]

	ev := t.maBeginEvent(ce.maCall)
	ev.FunctionIndex = frame.f.parent.index
	ev.BodyOffset = maBodyOffset(frame)
	ev.Opcode = target.Opcode
	ev.TargetObject = target.Object
	ev.Dst = dst
	ev.Length = uint64(width)
	ev.AddressTag = addrTag
	ev.LengthTag = valueTag{} // fixed-width stores have no length origin
	ev.DataTag = valTag

	memLen := uint64(len(memoryInst.Buffer))
	if dst > memLen || dst+uint64(width) > memLen {
		ev.Kind = "linear-oob"
		ev.Side = "destination"
		ev.FirstInvalid = firstOutOfBounds(dst, memLen)
		ev.HasFirstInvalid = true
		ev.Committed = false
		t.events = append(t.events, ev)
		panic(wasmruntime.ErrRuntimeOutOfBoundsMemoryAccess)
	}
	ev.Kind, ev.FirstInvalid, ev.HasFirstInvalid = t.classifyObject(t.object(target.Object), dst, uint64(width))
	if ev.HasFirstInvalid {
		ev.Side = "destination"
	}
	ev.Committed = true
	ev.Victims = t.victims(target.Object, dst, uint64(width))

	for i := uint64(0); i < uint64(width); i++ {
		memoryInst.Buffer[dst+i] = byte(val >> (8 * i))
		t.memSources[dst+i] = append([]string(nil), valTag.sources...)
		t.memWrites[dst+i] = ev.ID
	}
	t.events = append(t.events, ev)
}

// maMemoryFill implements the instrumented memory.fill path.
func (ce *callEngine) maMemoryFill(memoryInst *wasm.MemoryInstance, frame *callFrame) {
	t := ce.ma
	n := ce.popValue()
	lengthTag := ce.maPop()
	value := byte(ce.popValue())
	valTag := ce.maPop()
	offset := ce.popValue()
	addrTag := ce.maPop()

	key := MASiteKey{FunctionIndex: frame.f.parent.index, CodeSectionOff: frame.f.parent.offsetsInWasmBinary[frame.pc]}
	target := t.targets[key]

	ev := t.maBeginEvent(ce.maCall)
	ev.FunctionIndex = frame.f.parent.index
	ev.BodyOffset = maBodyOffset(frame)
	ev.Opcode = target.Opcode
	ev.TargetObject = target.Object
	ev.Dst = offset
	ev.Length = n
	ev.AddressTag = addrTag
	ev.LengthTag = lengthTag
	ev.DataTag = valTag

	memLen := uint64(len(memoryInst.Buffer))
	if offset > memLen || offset+n > memLen {
		ev.Kind = "linear-oob"
		ev.Side = "destination"
		ev.FirstInvalid = firstOutOfBounds(offset, memLen)
		ev.HasFirstInvalid = true
		ev.Committed = false
		t.events = append(t.events, ev)
		panic(wasmruntime.ErrRuntimeOutOfBoundsMemoryAccess)
	}
	ev.Kind, ev.FirstInvalid, ev.HasFirstInvalid = t.classifyObject(t.object(target.Object), offset, n)
	if ev.HasFirstInvalid {
		ev.Side = "destination"
	}
	ev.Committed = true
	ev.Victims = t.victims(target.Object, offset, n)
	for i := uint64(0); i < n; i++ {
		memoryInst.Buffer[offset+i] = value
		t.memSources[offset+i] = append([]string(nil), valTag.sources...)
		t.memWrites[offset+i] = ev.ID
	}
	t.events = append(t.events, ev)
}

// maMemoryCopy implements the instrumented memory.copy path. Per-byte source
// origins are snapshotted before any destination byte is touched so that
// overlapping copies preserve memmove semantics.
func (ce *callEngine) maMemoryCopy(memoryInst *wasm.MemoryInstance, frame *callFrame) {
	t := ce.ma
	n := ce.popValue()
	lengthTag := ce.maPop()
	src := ce.popValue()
	srcAddrTag := ce.maPop()
	dst := ce.popValue()
	dstAddrTag := ce.maPop()

	key := MASiteKey{FunctionIndex: frame.f.parent.index, CodeSectionOff: frame.f.parent.offsetsInWasmBinary[frame.pc]}
	target := t.targets[key]

	ev := t.maBeginEvent(ce.maCall)
	ev.FunctionIndex = frame.f.parent.index
	ev.BodyOffset = maBodyOffset(frame)
	ev.Opcode = target.Opcode
	ev.TargetObject = target.Object
	ev.Dst = dst
	ev.Length = n
	ev.Src = src
	ev.HasSrc = true
	ev.AddressTag = dstAddrTag
	ev.LengthTag = lengthTag
	ev.SourceAddressTag = srcAddrTag

	memLen := uint64(len(memoryInst.Buffer))
	dstBad := dst > memLen || dst+n > memLen
	srcBad := src > memLen || src+n > memLen
	if dstBad || srcBad {
		ev.Kind = "linear-oob"
		ev.Committed = false
		if dstBad {
			// When both intervals are out of bounds the destination wins.
			ev.Side = "destination"
			ev.FirstInvalid = firstOutOfBounds(dst, memLen)
		} else {
			ev.Side = "source"
			ev.FirstInvalid = firstOutOfBounds(src, memLen)
		}
		ev.HasFirstInvalid = true
		// Data origins are only readable when the whole source interval is in bounds.
		if !srcBad {
			ev.DataTag = t.loadTag(src, uint32(n))
		}
		t.events = append(t.events, ev)
		panic(wasmruntime.ErrRuntimeOutOfBoundsMemoryAccess)
	}
	ev.DataTag = t.loadTag(src, uint32(n))

	ev.Kind, ev.FirstInvalid, ev.HasFirstInvalid = t.classifyObject(t.object(target.Object), dst, n)
	if ev.HasFirstInvalid {
		ev.Side = "destination"
	}
	ev.Committed = true
	ev.Victims = t.victims(target.Object, dst, n)

	if n > 0 {
		// Snapshot the source bytes and their origins before touching the
		// destination, then perform the memmove and replicate origins per byte.
		snapBytes := append([]byte(nil), memoryInst.Buffer[src:src+n]...)
		snapTags := make([]valueTag, n)
		for i := uint64(0); i < n; i++ {
			snapTags[i] = t.byteTag(src + i)
		}
		copy(memoryInst.Buffer[dst:dst+n], snapBytes)
		for i := uint64(0); i < n; i++ {
			t.memSources[dst+i] = append([]string(nil), snapTags[i].sources...)
			t.memWrites[dst+i] = ev.ID
		}
	}
	t.events = append(t.events, ev)
}
