// Package memanalysis provides dynamic memory-corruption analysis for
// controlled, finite WebAssembly samples, driven by an out-of-band manifest
// supplied by the sample author.
//
// The analysis runs the sample on wazero's interpreter engine while tracking
// the origin of every i32 value and every linear-memory byte: which declared
// input sources and which earlier write instructions each value or byte
// derives from. Each executed write instruction is classified against the
// object the manifest says it should target and against the real linear
// memory boundary.
//
// This is deliberately manifest-constrained dynamic analysis, not a safety
// proof: the object boundaries come from the trusted Plan rather than from
// WebAssembly itself (Wasm modules carry no object boundaries), only the
// listed calls are executed, and the conclusions cannot establish the safety
// of arbitrary WebAssembly inputs.
package memanalysis

import (
	"context"
	"fmt"
	"sort"

	"github.com/tetratelabs/wazero/api"
	"github.com/tetratelabs/wazero/internal/engine/interpreter"
	"github.com/tetratelabs/wazero/internal/sys"
	"github.com/tetratelabs/wazero/internal/wasm"
	"github.com/tetratelabs/wazero/internal/wasm/binary"
)

// Object declares one static, half-open [Base, Base+Size) interval in the
// initial linear memory. Objects are non-empty, unique and non-overlapping;
// the analyzer never models allocation, freeing or object lifetime changes.
type Object struct {
	ID   string `json:"id"`
	Base uint32 `json:"base"`
	Size uint32 `json:"size"`
}

// Source declares input bytes written into linear memory during
// instantiation, before the first exported call. Each source is a non-empty,
// non-overlapping interval and gives its bytes an input origin.
type Source struct {
	ID     string `json:"id"`
	Offset uint32 `json:"offset"`
	Bytes  []byte `json:"bytes"`
}

// Target binds one write instruction to the object it was intended to write.
// Function is the local defined-function index and Offset is the instruction
// opcode's byte offset within that function's Body (offset zero is the first
// instruction, excluding the local declaration vector).
type Target struct {
	Function uint32 `json:"function"`
	Offset   uint32 `json:"offset"`
	Object   string `json:"object"`
}

// Call is one exported-function invocation, in execution order.
type Call struct {
	Export string   `json:"export"`
	Args   []uint32 `json:"args"`
}

// Plan is the sample author's trusted side-channel manifest.
type Plan struct {
	Objects []Object `json:"objects"`
	Sources []Source `json:"sources"`
	Targets []Target `json:"targets"`
	Calls   []Call   `json:"calls"`
}

// Origin reports the provenance of an address, length or written value. Both
// lists are sorted and de-duplicated; empty sets are serialized as [].
type Origin struct {
	Sources []string `json:"sources"`
	Writes  []uint32 `json:"writes"`
}

// Event is one executed write instruction, including legal, zero-length and
// finally-trapping attempts.
type Event struct {
	ID       uint32 `json:"id"`
	Call     uint32 `json:"call"`
	Function uint32 `json:"function"`
	Offset   uint32 `json:"offset"`
	Opcode   string `json:"opcode"`
	Target   string `json:"target"`
	Dst      uint64 `json:"dst"`
	Length   uint64 `json:"length"`
	// Src is present only for memory.copy.
	Src *uint64 `json:"src,omitempty"`

	Kind         string   `json:"kind"`
	Committed    bool     `json:"committed"`
	FirstInvalid uint64   `json:"first_invalid,omitempty"`
	Side         string   `json:"side,omitempty"`
	Victims      []string `json:"victims"`

	Address       Origin  `json:"address"`
	LengthOrigin  Origin  `json:"length_origin"`
	Data          Origin  `json:"data"`
	SourceAddress *Origin `json:"source_address,omitempty"`
}

// CallReport records the outcome of one planned exported call.
type CallReport struct {
	Call    uint32   `json:"call"`
	Export  string   `json:"export"`
	Status  string   `json:"status"`
	Results []uint32 `json:"results"`
	Trap    string   `json:"trap,omitempty"`
}

// ObjectSnapshot is the final content of one declared object.
type ObjectSnapshot struct {
	ID    string `json:"id"`
	Bytes []byte `json:"bytes"`
}

// Report is the json.Marshal-able result of Analyze.
type Report struct {
	Calls   []CallReport     `json:"calls"`
	Writes  []Event          `json:"writes"`
	Objects []ObjectSnapshot `json:"objects"`
}

// Analyze executes plan.Calls against one fresh interpreter runtime and module
// instance, collecting write events and byte provenance, then closes the
// runtime. Neither binary nor plan nor their slices are modified, and an
// invalid plan returns an error before any sample function executes.
func Analyze(ctx context.Context, binaryInput []byte, plan Plan) (Report, error) {
	prepared, module, err := prepare(binaryInput, plan)
	if err != nil {
		return Report{}, err
	}

	// Build the interpreter runtime directly so the exact module decoded and
	// validated above (including the forced per-operation source offsets) is
	// the one that gets compiled and instantiated.
	engine := interpreter.NewEngine(ctx, api.CoreFeaturesV2, nil)
	store := wasm.NewStore(api.CoreFeaturesV2, engine)
	typeIDs, err := store.GetFunctionTypeIDs(module.TypeSection)
	if err != nil {
		return Report{}, fmt.Errorf("resolve function types: %w", err)
	}
	if err := engine.CompileModule(ctx, module, nil, true); err != nil {
		return Report{}, fmt.Errorf("compile module: %w", err)
	}
	inst, err := store.Instantiate(ctx, module, "", sys.DefaultContext(nil), typeIDs)
	if err != nil {
		return Report{}, fmt.Errorf("instantiate module: %w", err)
	}
	defer func() {
		_ = store.CloseWithExitCode(ctx, 0)
		_ = engine.Close()
	}()

	tracker := interpreter.NewMemAnalysisTracker(interpreter.MAConfig{
		Memory:      inst.MemoryInstance,
		GlobalCount: len(inst.Globals),
		Objects:     prepared.objects,
		Sources:     prepared.sources,
		Targets:     prepared.targets,
	})
	// Sources are the input bytes placed in memory between instantiation and
	// the first call. The module itself carries no data for them, so the
	// manifest drives the seeding; ranges were validated as in-bounds and
	// non-overlapping before execution.
	for _, s := range plan.Sources {
		copy(inst.MemoryInstance.Buffer[s.Offset:], s.Bytes)
	}

	report := Report{Calls: []CallReport{}, Writes: []Event{}, Objects: []ObjectSnapshot{}}
	for i, call := range plan.Calls {
		if err := ctx.Err(); err != nil {
			return Report{}, err
		}
		fn := inst.ExportedFunction(call.Export)
		if fn == nil {
			return Report{}, fmt.Errorf("call %d: unknown export %q", i+1, call.Export)
		}
		if err := interpreter.AttachMemAnalysis(fn, tracker, uint32(i+1)); err != nil {
			return Report{}, err
		}
		params := make([]uint64, len(call.Args))
		for j, a := range call.Args {
			params[j] = uint64(a)
		}
		results, callErr := fn.Call(ctx, params...)
		interpreter.DetachMemAnalysis(fn)
		// Cancellation is an external interruption rather than a guest trap:
		// stop the sequence and surface it, leaving the runtime to close.
		if ctxErr := ctx.Err(); ctxErr != nil {
			return Report{}, ctxErr
		}

		entry := CallReport{Call: uint32(i + 1), Export: call.Export, Results: []uint32{}}
		if callErr != nil {
			entry.Status = "trap"
			entry.Trap = callErr.Error()
		} else {
			entry.Status = "ok"
			for _, r := range results {
				entry.Results = append(entry.Results, uint32(r))
			}
		}
		report.Calls = append(report.Calls, entry)
	}

	for _, ev := range tracker.Events() {
		report.Writes = append(report.Writes, toEvent(ev))
	}
	for _, obj := range plan.Objects {
		report.Objects = append(report.Objects, ObjectSnapshot{
			ID:    obj.ID,
			Bytes: append([]byte(nil), inst.MemoryInstance.Buffer[obj.Base:obj.Base+obj.Size]...),
		})
	}
	sort.Slice(report.Objects, func(i, j int) bool { return report.Objects[i].ID < report.Objects[j].ID })
	return report, nil
}

func toOrigin(v interpreter.ValueTagView) Origin {
	sources, writes := v.Sources, v.Writes
	if sources == nil {
		sources = []string{}
	}
	if writes == nil {
		writes = []uint32{}
	}
	return Origin{Sources: sources, Writes: writes}
}

func toEvent(ev *interpreter.TrackedEvent) Event {
	out := Event{
		ID:           ev.ID,
		Call:         ev.CallIndex,
		Function:     ev.FunctionIndex,
		Offset:       ev.BodyOffset,
		Opcode:       ev.Opcode,
		Target:       ev.TargetObject,
		Dst:          ev.Dst,
		Length:       ev.Length,
		Kind:         ev.Kind,
		Committed:    ev.Committed,
		Victims:      append([]string(nil), ev.Victims...),
		Address:      toOrigin(ev.AddressOrigin()),
		LengthOrigin: toOrigin(ev.LengthOriginView()),
		Data:         toOrigin(ev.DataOrigin()),
	}
	if len(out.Victims) == 0 {
		out.Victims = []string{}
	}
	if ev.HasFirstInvalid {
		out.FirstInvalid = ev.FirstInvalid
		out.Side = ev.Side
	}
	if ev.HasSrc {
		src := ev.Src
		out.Src = &src
		sa := toOrigin(ev.SourceAddressOrigin())
		out.SourceAddress = &sa
	}
	return out
}

type prepared struct {
	objects []interpreter.MAObject
	sources []interpreter.MASource
	targets map[interpreter.MASiteKey]interpreter.MATarget
}

func prepare(binaryInput []byte, plan Plan) (*prepared, *wasm.Module, error) {
	module, err := binary.DecodeModule(binaryInput, api.CoreFeaturesV2,
		wasm.MemoryLimitPages, false, false, false)
	if err != nil {
		return nil, nil, fmt.Errorf("decode module: %w", err)
	}
	if err := module.Validate(api.CoreFeaturesV2); err != nil {
		return nil, nil, fmt.Errorf("validate module: %w", err)
	}
	if err := validateModuleShape(module); err != nil {
		return nil, nil, err
	}
	if err := validatePlan(module, plan); err != nil {
		return nil, nil, err
	}

	sites, err := scanBodies(module)
	if err != nil {
		return nil, nil, err
	}
	if err := validateTargets(module, plan, sites); err != nil {
		return nil, nil, err
	}

	// Retain source offsets for every compiled operation so the running frame
	// can locate each write instruction in the original Body.
	interpreter.ForceSourceOffsets(module)
	module.BuildMemoryDefinitions()

	p := &prepared{targets: map[interpreter.MASiteKey]interpreter.MATarget{}}
	for _, o := range plan.Objects {
		p.objects = append(p.objects, interpreter.MAObject{ID: o.ID, Base: o.Base, Size: o.Size})
	}
	for _, s := range plan.Sources {
		p.sources = append(p.sources, interpreter.MASource{ID: s.ID, Offset: s.Offset, Length: uint32(len(s.Bytes))})
	}
	for i, t := range plan.Targets {
		code := module.CodeSection[t.Function]
		key := interpreter.MASiteKey{
			FunctionIndex:  t.Function,
			CodeSectionOff: code.BodyOffsetInCodeSection + uint64(t.Offset),
		}
		p.targets[key] = interpreter.MATarget{Object: t.Object, Opcode: sitesOpcode(sites, t, i)}
	}
	return p, module, nil
}

func sitesOpcode(sites []writeSite, t Target, i int) string {
	for _, s := range sites {
		if s.function == t.Function && s.bodyOff == t.Offset {
			return s.opcode
		}
	}
	panic(fmt.Sprintf("target %d not matched to a scanned site", i))
}

// validateModuleShape enforces the analyzer's restricted module surface before
// any call can execute. Dead code is covered by scanBodies separately.
func validateModuleShape(module *wasm.Module) error {
	if len(module.ImportSection) != 0 {
		return fmt.Errorf("memanalysis: imported modules are not supported")
	}
	if module.StartSection != nil {
		return fmt.Errorf("memanalysis: start functions are not supported")
	}
	if len(module.TableSection) != 0 {
		return fmt.Errorf("memanalysis: tables are not supported")
	}
	if module.MemorySection == nil {
		return fmt.Errorf("memanalysis: module must declare exactly one memory")
	}
	mem := module.MemorySection
	if mem.IsShared {
		return fmt.Errorf("memanalysis: shared memory is not supported")
	}
	if mem.Min < 1 {
		return fmt.Errorf("memanalysis: memory min must be at least one page")
	}
	if !mem.IsMaxEncoded || mem.Max > 2 {
		return fmt.Errorf("memanalysis: memory must encode an explicit max of at most two pages")
	}
	for _, ft := range module.TypeSection {
		for _, p := range ft.Params {
			if p != wasm.ValueTypeI32 {
				return fmt.Errorf("memanalysis: only i32 function parameters are supported")
			}
		}
		for _, r := range ft.Results {
			if r != wasm.ValueTypeI32 {
				return fmt.Errorf("memanalysis: only i32 function results are supported")
			}
		}
		if len(ft.Results) > 1 {
			return fmt.Errorf("memanalysis: functions may have at most one result")
		}
	}
	for _, g := range module.GlobalSection {
		if g.Type.ValType != wasm.ValueTypeI32 {
			return fmt.Errorf("memanalysis: only i32 globals are supported")
		}
	}
	for _, code := range module.CodeSection {
		for _, lt := range code.LocalTypes {
			if lt != wasm.ValueTypeI32 {
				return fmt.Errorf("memanalysis: only i32 locals are supported")
			}
		}
	}
	for _, d := range module.DataSection {
		if d.Passive {
			return fmt.Errorf("memanalysis: only active data segments are supported")
		}
	}
	return nil
}

func validatePlan(module *wasm.Module, plan Plan) error {
	objectIDs := map[string]bool{}
	memMinBytes := uint64(module.MemorySection.Min) << 16
	type interval struct {
		name       string
		start, end uint64
	}
	var objIntervals []interval
	for i, o := range plan.Objects {
		if o.ID == "" {
			return fmt.Errorf("objects[%d]: id is empty", i)
		}
		if objectIDs[o.ID] {
			return fmt.Errorf("objects[%d]: duplicate object id %q", i, o.ID)
		}
		objectIDs[o.ID] = true
		if o.Size == 0 {
			return fmt.Errorf("objects[%d] %q: objects must be non-empty", i, o.ID)
		}
		start, end := uint64(o.Base), uint64(o.Base)+uint64(o.Size)
		if end > memMinBytes {
			return fmt.Errorf("objects[%d] %q: interval exceeds initial memory", i, o.ID)
		}
		objIntervals = append(objIntervals, interval{o.ID, start, end})
	}
	// Exhaustive pairwise overlap check (objects are not required to be sorted).
	for i := range objIntervals {
		for j := i + 1; j < len(objIntervals); j++ {
			a, b := objIntervals[i], objIntervals[j]
			if a.start < b.end && b.start < a.end {
				return fmt.Errorf("objects %q and %q overlap", a.name, b.name)
			}
		}
	}

	sourceIDs := map[string]bool{}
	var srcIntervals []interval
	for i, s := range plan.Sources {
		if s.ID == "" {
			return fmt.Errorf("sources[%d]: id is empty", i)
		}
		if sourceIDs[s.ID] {
			return fmt.Errorf("sources[%d]: duplicate source id %q", i, s.ID)
		}
		sourceIDs[s.ID] = true
		if len(s.Bytes) == 0 {
			return fmt.Errorf("sources[%d] %q: sources must be non-empty", i, s.ID)
		}
		start, end := uint64(s.Offset), uint64(s.Offset)+uint64(len(s.Bytes))
		if end > memMinBytes {
			return fmt.Errorf("sources[%d] %q: interval exceeds linear memory", i, s.ID)
		}
		srcIntervals = append(srcIntervals, interval{s.ID, start, end})
	}
	for i := range srcIntervals {
		for j := i + 1; j < len(srcIntervals); j++ {
			a, b := srcIntervals[i], srcIntervals[j]
			if a.start < b.end && b.start < a.end {
				return fmt.Errorf("sources %q and %q overlap", a.name, b.name)
			}
		}
	}

	for i, c := range plan.Calls {
		exp, ok := module.Exports[c.Export]
		if !ok {
			return fmt.Errorf("calls[%d]: unknown export %q", i, c.Export)
		}
		if exp.Type != wasm.ExternTypeFunc {
			return fmt.Errorf("calls[%d]: export %q is not a function", i, c.Export)
		}
		ft := module.TypeSection[module.FunctionSection[exp.Index]]
		if len(ft.Params) != len(c.Args) {
			return fmt.Errorf("calls[%d] %q: expected %d args, got %d", i, c.Export, len(ft.Params), len(c.Args))
		}
	}
	return nil
}

func validateTargets(module *wasm.Module, plan Plan, sites []writeSite) error {
	if len(plan.Targets) != len(sites) {
		return fmt.Errorf("targets: manifest lists %d write sites but module contains %d write instructions",
			len(plan.Targets), len(sites))
	}
	seen := map[writeSite]bool{}
	for i, t := range plan.Targets {
		if int(t.Function) >= len(module.CodeSection) {
			return fmt.Errorf("targets[%d]: unknown function index %d", i, t.Function)
		}
		if !objectExists(plan.Objects, t.Object) {
			return fmt.Errorf("targets[%d]: unknown object %q", i, t.Object)
		}
		matched := false
		for _, s := range sites {
			if s.function == t.Function && s.bodyOff == t.Offset {
				matched = true
				if seen[s] {
					return fmt.Errorf("targets[%d]: duplicate target function=%d offset=%d", i, t.Function, t.Offset)
				}
				seen[s] = true
			}
		}
		if !matched {
			return fmt.Errorf("targets[%d]: function=%d offset=%d is not the start of a write instruction",
				i, t.Function, t.Offset)
		}
	}
	if len(seen) != len(sites) {
		return fmt.Errorf("targets: manifest does not cover every write instruction")
	}
	return nil
}

func objectExists(objects []Object, id string) bool {
	for _, o := range objects {
		if o.ID == id {
			return true
		}
	}
	return false
}
