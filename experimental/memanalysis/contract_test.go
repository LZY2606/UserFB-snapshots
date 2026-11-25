package memanalysis

import (
	"context"
	"testing"
	"time"

	"github.com/tetratelabs/wazero/task_support/taskcheck"
)

// TestObjectBoundary covers whole-interval writes landing inside a neighbor
// object, legal in-object writes, linear-memory traps and the rule that the
// bound object is taken from the manifest rather than re-picked by address.
func TestObjectBoundary(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		const (
			i32Const   = 0x41
			i32Store   = 0x36
			i32Store16 = 0x3b
		)
		miscCopy := []byte{0xfc, 0x0a, 0x00, 0x00}
		miscFill := []byte{0xfc, 0x0b, 0x00}
		b := newWasmBuilder()
		fn := b.addFunction(0, "write")
		// Legal write fully inside objA [0,8).
		b.emit(fn, i32Const, 0, i32Const, 0x2a)
		legalOff := b.markStore(fn, "objA")
		b.emit(fn, i32Store, 2, 0)
		// Write whose whole interval lands in neighbor objB [8,16), while the
		// manifest still binds it to objA. 187 encodes as 0xbb 0x01.
		b.emit(fn, i32Const, 8, i32Const, 0xbb, 0x01)
		neighborOff := b.markStore(fn, "objA")
		b.emit(fn, i32Store, 2, 0)
		// Write past the single-page linear memory: 65535 + 2 bytes.
		b.emit(fn, i32Const, 0xff, 0xff, 3, i32Const, 1)
		oobOff := b.markStore(fn, "objA")
		b.emit(fn, i32Store16, 1, 0)
		bin := b.build()

		plan := Plan{
			Objects: []Object{{ID: "objA", Base: 0, Size: 8}, {ID: "objB", Base: 8, Size: 16}},
			Calls:   []Call{{Export: "write"}},
			Targets: b.writeSites,
		}
		report, err := Analyze(context.Background(), bin, plan)
		if err != nil {
			c.Check(false, "Analyze failed: %v", err)
			return
		}
		c.Check(len(report.Writes) == 3, "want 3 write events, got %d", len(report.Writes))

		legal := findEvent(report.Writes, uint32(fn), legalOff)
		c.Check(legal != nil && legal.Kind == "legal", "legal write misclassified: %+v", legal)
		c.Check(legal != nil && legal.Committed, "legal write must be committed")

		overflow := findEvent(report.Writes, uint32(fn), neighborOff)
		c.Check(overflow != nil, "neighbor event missing")
		if overflow != nil {
			c.Check(overflow.Kind == "object-overflow", "whole-interval neighbor write must be object-overflow, got %q", overflow.Kind)
			c.Check(overflow.Committed, "object-overflow writes still execute")
			c.Check(overflow.FirstInvalid == 8, "first_invalid must be the interval start 8, got %d", overflow.FirstInvalid)
			c.Check(overflow.Side == "destination", "overflow side must be destination, got %q", overflow.Side)
			c.Check(len(overflow.Victims) == 1 && overflow.Victims[0] == "objB",
				"victims must list only objB, got %v", overflow.Victims)
		}

		oob := findEvent(report.Writes, uint32(fn), oobOff)
		c.Check(oob != nil, "oob event missing")
		if oob != nil {
			c.Check(oob.Kind == "linear-oob", "out-of-range store must be linear-oob, got %q", oob.Kind)
			c.Check(!oob.Committed, "trapping store must not be committed")
			c.Check(oob.FirstInvalid == 65536 && oob.Side == "destination",
				"oob first_invalid/side wrong: %d %q", oob.FirstInvalid, oob.Side)
		}
		c.Check(report.Writes[0].ID == 1 && report.Writes[2].ID == 3, "event ids must be dense from 1")

		// Counter-example: binding the neighbor write to the object it actually
		// lands in makes it legal, proving the object is never re-selected by
		// the observed address.
		rebound := plan
		rebound.Targets = append([]Target(nil), plan.Targets...)
		rebound.Targets[1] = Target{Function: uint32(fn), Offset: neighborOff, Object: "objB"}
		report2, err := Analyze(context.Background(), bin, rebound)
		c.Check(err == nil, "rebound Analyze failed: %v", err)
		ev := findEvent(report2.Writes, uint32(fn), neighborOff)
		c.Check(ev != nil && ev.Kind == "legal", "rebound write should be legal, got %+v", ev)

		// Call arguments have no origin even when they drive a store address.
		const localGet = 0x20
		pb := newWasmBuilder()
		pfn := pb.addFunction(1, "withparam")
		pb.emit(pfn, i32Const, 0, localGet, 0)
		paramOff := pb.markStore(pfn, "objA")
		pb.emit(pfn, i32Store, 2, 0)
		paramPlan := Plan{
			Objects: []Object{{ID: "objA", Base: 0, Size: 8}},
			Calls:   []Call{{Export: "withparam", Args: []uint32{0x77}}},
			Targets: pb.writeSites,
		}
		paramReport, err := Analyze(context.Background(), pb.build(), paramPlan)
		if err != nil {
			c.Check(false, "parameterized Analyze failed: %v", err)
		} else {
			paramEv := findEvent(paramReport.Writes, uint32(pfn), paramOff)
			c.Check(paramEv != nil && len(paramEv.Data.Sources) == 0 && len(paramEv.Data.Writes) == 0,
				"argument-derived store must carry no origin: %+v", paramEv)
			c.Check(paramReport.Objects[0].Bytes[0] == 0x77, "argument value must reach memory")
		}

		// Zero-length operations pass the linear boundary check and stay legal
		// without writing any byte; a source-side linear OOB reports side
		// "source" only when the destination interval is in bounds.
		zb := newWasmBuilder()
		zfn := zb.addFunction(0, "zlen")
		// fill objA[4..4): zero length at an in-bounds start.
		zb.emit(zfn, i32Const, 4, i32Const, 0, i32Const, 0)
		zeroFillOff := zb.markStore(zfn, "objA")
		zb.emit(zfn, miscFill...)
		// copy with an in-bounds destination but an out-of-bounds source.
		zb.emit(zfn, i32Const, 4, i32Const, 0xff, 0xff, 3, i32Const, 4)
		srcOobOff := zb.markStore(zfn, "objA")
		zb.emit(zfn, miscCopy...)
		zplan := Plan{
			Objects: []Object{{ID: "objA", Base: 0, Size: 8}},
			Calls:   []Call{{Export: "zlen"}},
			Targets: zb.writeSites,
		}
		zreport, err := Analyze(context.Background(), zb.build(), zplan)
		if err != nil {
			c.Check(false, "zero-length Analyze failed: %v", err)
		} else {
			zeroFill := findEvent(zreport.Writes, uint32(zfn), zeroFillOff)
			c.Check(zeroFill != nil && zeroFill.Kind == "legal" && zeroFill.Committed && zeroFill.Length == 0,
				"zero-length fill must be legal and committed: %+v", zeroFill)
			c.Check(zeroFill != nil && zeroFill.FirstInvalid == 0 && zeroFill.Side == "",
				"legal event carries no first_invalid or side")
			srcOob := findEvent(zreport.Writes, uint32(zfn), srcOobOff)
			c.Check(srcOob != nil && srcOob.Kind == "linear-oob" && !srcOob.Committed,
				"source OOB copy must be linear-oob: %+v", srcOob)
			c.Check(srcOob != nil && srcOob.Side == "source" && srcOob.FirstInvalid == 65536,
				"source OOB side/first_invalid wrong: %+v", srcOob)
		}

		// A fill that leaves the bound object is object-overflow but keeps
		// executing, and reports the bound object's end as first_invalid.
		fb := newWasmBuilder()
		ffn := fb.addFunction(0, "fillover")
		fb.emit(ffn, i32Const, 4, i32Const, 0x19, i32Const, 8)
		fillOverOff := fb.markStore(ffn, "objA")
		fb.emit(ffn, miscFill...)
		fplan := Plan{
			Objects: []Object{{ID: "objA", Base: 0, Size: 8}, {ID: "objB", Base: 8, Size: 8}},
			Calls:   []Call{{Export: "fillover"}},
			Targets: fb.writeSites,
		}
		freport, err := Analyze(context.Background(), fb.build(), fplan)
		if err != nil {
			c.Check(false, "fill-overflow Analyze failed: %v", err)
		} else {
			fev := findEvent(freport.Writes, uint32(ffn), fillOverOff)
			c.Check(fev != nil && fev.Kind == "object-overflow" && fev.Committed && fev.FirstInvalid == 8,
				"fill overflow classification wrong: %+v", fev)
			c.Check(fev != nil && len(fev.Victims) == 1 && fev.Victims[0] == "objB",
				"fill overflow victims wrong: %+v", fev)
		}
	})
}

// TestWriteOrigins covers sourced legal writes, narrow overwrites clearing old
// origins, per-byte differences under overlapping memory.copy, and addresses
// and lengths derived from input bytes.
func TestWriteOrigins(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		const (
			i32Const  = 0x41
			i32Load   = 0x28
			i32Load8U = 0x2d
			i32Store  = 0x36
			i32Store8 = 0x3a
			i32Add    = 0x6a
		)
		miscCopy := []byte{0xfc, 0x0a, 0x00, 0x00}
		miscFill := []byte{0xfc, 0x0b, 0x00}

		b := newWasmBuilder()

		// f0: sourced legal writes into dst [0,8), a narrow constant overwrite
		// and per-byte verification stores into work [16,24).
		f0 := b.addFunction(0, "legalAndNarrow")
		b.emit(f0, i32Const, 0, i32Const, 32, i32Load, 2, 0)
		sourcedOff := b.markStore(f0, "dst")
		b.emit(f0, i32Store, 2, 0)
		b.emit(f0, i32Const, 4, i32Const, 32, i32Load, 2, 0)
		prefillOff := b.markStore(f0, "dst")
		b.emit(f0, i32Store, 2, 0)
		// Constant single-byte overwrite of dst byte 5.
		b.emit(f0, i32Const, 5, i32Const, 0)
		narrowOff := b.markStore(f0, "dst")
		b.emit(f0, i32Store8, 0, 0)
		// Verify each byte's origin through narrow loads and stores.
		var verifyOffs [3]uint32
		for i, srcAddr := range []uint32{4, 5, 6} {
			b.emit(f0, i32Const)
			b.emitU(f0, uint32(16+i))
			b.emit(f0, i32Const, byte(srcAddr), i32Load8U, 0, 0)
			verifyOffs[i] = b.markStore(f0, "work")
			b.emit(f0, i32Store8, 0, 0)
		}

		// f1: address derived from an input byte (value 16).
		f1 := b.addFunction(0, "derivedAddress")
		b.emit(f1, i32Const, 48, i32Load8U, 0, 0, i32Const, 0x77)
		derivedAddrOff := b.markStore(f1, "work")
		b.emit(f1, i32Store, 2, 0)

		// f2: fill length derived from an input byte (value 2).
		f2 := b.addFunction(0, "derivedLength")
		b.emit(f2, i32Const, 16, i32Const, 0x55, i32Const, 50, i32Load8U, 0, 0)
		fillOff := b.markStore(f2, "work")
		b.emit(f2, miscFill...)

		// f3: overlapping memory.copy inside blob [56,72), then per-byte
		// verification of the shifted origins.
		f3 := b.addFunction(0, "overlap")
		// blob[60] := input byte from source x; blob[61] := source y.
		b.emit(f3, i32Const, 60, i32Const, 32, i32Load8U, 0, 0)
		setupX := b.markStore(f3, "blob")
		b.emit(f3, i32Store8, 0, 0)
		b.emit(f3, i32Const, 61, i32Const, 40, i32Load8U, 0, 0)
		setupY := b.markStore(f3, "blob")
		b.emit(f3, i32Store8, 0, 0)
		// copy [60,62) -> [61,63): overlapping one-byte shift.
		b.emit(f3, i32Const, 61, i32Const, 60, i32Const, 2)
		copyOff := b.markStore(f3, "blob")
		b.emit(f3, miscCopy...)
		var overlapVerify [2]uint32
		for i, srcAddr := range []uint32{61, 62} {
			b.emit(f3, i32Const)
			b.emitU(f3, uint32(20+i))
			b.emit(f3, i32Const, byte(srcAddr), i32Load8U, 0, 0)
			overlapVerify[i] = b.markStore(f3, "work")
			b.emit(f3, i32Store8, 0, 0)
		}

		bin := b.build()
		plan := Plan{
			Objects: []Object{
				{ID: "dst", Base: 0, Size: 8},
				{ID: "work", Base: 16, Size: 8},
				{ID: "blob", Base: 56, Size: 16},
			},
			Sources: []Source{
				{ID: "x", Offset: 32, Bytes: []byte{0x11, 0x22, 0x33, 0x44}},
				{ID: "y", Offset: 40, Bytes: []byte{0x09}},
				{ID: "addr", Offset: 48, Bytes: []byte{16}},
				{ID: "len", Offset: 50, Bytes: []byte{2}},
			},
			Calls: []Call{
				{Export: "legalAndNarrow"},
				{Export: "derivedAddress"},
				{Export: "derivedLength"},
				{Export: "overlap"},
			},
			Targets: b.writeSites,
		}
		report, err := Analyze(context.Background(), bin, plan)
		c.Check(err == nil, "Analyze failed: %v", err)

		// Legal sourced write carries the input source, not the address.
		ev := findEvent(report.Writes, uint32(f0), sourcedOff)
		c.Check(ev != nil, "sourced store event missing")
		if ev != nil {
			c.Check(ev.Kind == "legal" && len(ev.Data.Sources) == 1 && ev.Data.Sources[0] == "x",
				"sourced legal write data origin wrong: %+v", ev.Data)
			c.Check(len(ev.Address.Sources) == 0, "constant address must have no source origin")
		}
		prefill := findEvent(report.Writes, uint32(f0), prefillOff)
		c.Check(prefill != nil && prefill.Kind == "legal", "prefill write must be legal")

		// The narrow constant overwrite has no data origin itself.
		narrow := findEvent(report.Writes, uint32(f0), narrowOff)
		c.Check(narrow != nil && len(narrow.Data.Sources) == 0 && len(narrow.Data.Writes) == 0,
			"constant narrow write must carry no origin: %+v", narrow)

		// Narrow coverage: only the overwritten byte loses source x; its old
		// write history is replaced by the narrow event itself.
		keep := findEvent(report.Writes, uint32(f0), verifyOffs[0])
		cleared := findEvent(report.Writes, uint32(f0), verifyOffs[1])
		other := findEvent(report.Writes, uint32(f0), verifyOffs[2])
		c.Check(keep != nil && contains(keep.Data.Sources, "x"), "byte 4 should retain x: %+v", keep)
		c.Check(cleared != nil && len(cleared.Data.Sources) == 0 &&
			len(cleared.Data.Writes) == 1 && cleared.Data.Writes[0] == narrow.ID,
			"byte 5 source must be cleared and rewritten only by the narrow event %d: %+v", narrow.ID, cleared)
		c.Check(other != nil && contains(other.Data.Sources, "x"), "byte 6 should retain x: %+v", other)

		// Data-derived address and length.
		derived := findEvent(report.Writes, uint32(f1), derivedAddrOff)
		c.Check(derived != nil && contains(derived.Address.Sources, "addr"),
			"derived address origin missing: %+v", derived)
		fill := findEvent(report.Writes, uint32(f2), fillOff)
		c.Check(fill != nil && fill.Opcode == "memory.fill", "fill event opcode wrong: %+v", fill)
		if fill != nil {
			c.Check(contains(fill.LengthOrigin.Sources, "len"), "derived length origin missing: %+v", fill.LengthOrigin)
			c.Check(len(fill.Data.Sources) == 0, "constant fill value has no source origin")
			c.Check(fill.Length == 2 && fill.Dst == 16, "fill operands wrong: dst=%d len=%d", fill.Dst, fill.Length)
		}

		// Overlapping copy: snapshot semantics preserve distinct per-byte origins.
		cx := findEvent(report.Writes, uint32(f3), setupX)
		cy := findEvent(report.Writes, uint32(f3), setupY)
		cp := findEvent(report.Writes, uint32(f3), copyOff)
		c.Check(cx != nil && cy != nil && cp != nil, "overlap setup/copy events missing")
		if cp != nil {
			c.Check(cp.Opcode == "memory.copy" && cp.Src != nil && *cp.Src == 60 && cp.Dst == 61 && cp.Length == 2,
				"copy operands wrong: %+v", cp)
		}
		shiftedX := findEvent(report.Writes, uint32(f3), overlapVerify[0])
		shiftedY := findEvent(report.Writes, uint32(f3), overlapVerify[1])
		c.Check(shiftedX != nil && contains(shiftedX.Data.Sources, "x") && !contains(shiftedX.Data.Sources, "y"),
			"byte 61 must carry x alone after overlap: %+v", shiftedX)
		c.Check(shiftedY != nil && contains(shiftedY.Data.Sources, "y") && !contains(shiftedY.Data.Sources, "x"),
			"byte 62 must carry y alone after overlap: %+v", shiftedY)

		// Counter-example: removing the narrow overwrite restores x on byte 5.
		// Build a module identical to f0 but without the store8 overwrite.
		b2 := newWasmBuilder()
		g := b2.addFunction(0, "legalAndNarrow")
		b2.emit(g, i32Const, 0, i32Const, 32, i32Load, 2, 0)
		b2.markStore(g, "dst")
		b2.emit(g, i32Store, 2, 0)
		b2.emit(g, i32Const, 4, i32Const, 32, i32Load, 2, 0)
		b2.markStore(g, "dst")
		b2.emit(g, i32Store, 2, 0)
		var offs [3]uint32
		for i, srcAddr := range []uint32{4, 5, 6} {
			b2.emit(g, i32Const)
			b2.emitU(g, uint32(16+i))
			b2.emit(g, i32Const, byte(srcAddr), i32Load8U, 0, 0)
			offs[i] = b2.markStore(g, "work")
			b2.emit(g, i32Store8, 0, 0)
		}
		plan2 := Plan{
			Objects: plan.Objects, Sources: plan.Sources,
			Calls: []Call{{Export: "legalAndNarrow"}}, Targets: b2.writeSites,
		}
		report2, err := Analyze(context.Background(), b2.build(), plan2)
		c.Check(err == nil, "counter Analyze failed: %v", err)
		mid := findEvent(report2.Writes, uint32(g), offs[1])
		c.Check(mid != nil && contains(mid.Data.Sources, "x"),
			"without the narrow overwrite byte 5 must retain x: %+v", mid)

		// select keeps only the chosen value's origin.
		const (
			i32Select = 0x1b
			i32Drop   = 0x1a
			memGrow   = 0x40
		)
		sb := newWasmBuilder()
		sfn := sb.addFunction(0, "sel")
		// Stack: addr, sourced v1 (load x[0]), clean v2, condition 1 -> v1.
		sb.emit(sfn, i32Const, 0, i32Const, 32, i32Load8U, 0, 0, i32Const, 0x22, i32Const, 1, i32Select)
		selOff := sb.markStore(sfn, "dst")
		sb.emit(sfn, i32Store, 2, 0)
		sPlan := Plan{
			Objects: plan.Objects, Sources: plan.Sources,
			Calls: []Call{{Export: "sel"}}, Targets: sb.writeSites,
		}
		sReport, err := Analyze(context.Background(), sb.build(), sPlan)
		c.Check(err == nil, "select Analyze failed: %v", err)
		selEv := findEvent(sReport.Writes, uint32(sfn), selOff)
		c.Check(selEv != nil && contains(selEv.Data.Sources, "x") && !contains(selEv.Data.Sources, "y"),
			"select must keep only the chosen value's origin: %+v", selEv)

		// Counter: condition 0 selects the clean alternative, dropping x.
		sb0 := newWasmBuilder()
		sfn0 := sb0.addFunction(0, "sel")
		sb0.emit(sfn0, i32Const, 0, i32Const, 32, i32Load8U, 0, 0, i32Const, 0x22, i32Const, 0, i32Select)
		selOff0 := sb0.markStore(sfn0, "dst")
		sb0.emit(sfn0, i32Store, 2, 0)
		s0Report, err := Analyze(context.Background(), sb0.build(), Plan{
			Objects: plan.Objects, Sources: plan.Sources,
			Calls: []Call{{Export: "sel"}}, Targets: sb0.writeSites,
		})
		c.Check(err == nil, "select-zero Analyze failed: %v", err)
		sel0 := findEvent(s0Report.Writes, uint32(sfn0), selOff0)
		c.Check(sel0 != nil && len(sel0.Data.Sources) == 0,
			"unselected sourced operand must not contribute origin: %+v", sel0)

		// memory.grow extends origin tracking without panic; a write into the
		// grown page passes the linear check but remains outside every object.
		gb := newWasmBuilder()
		gfn := gb.addFunction(0, "grow")
		gb.emit(gfn, i32Const, 1, memGrow, 0, i32Drop)
		// 65536 = 0x10000 encodes as 0x80 0x80 0x04.
		gb.emit(gfn, i32Const, 0x80, 0x80, 0x04, i32Const, 0x33)
		growStoreOff := gb.markStore(gfn, "dst")
		gb.emit(gfn, i32Store, 2, 0)
		gReport, err := Analyze(context.Background(), gb.build(), Plan{
			Objects: plan.Objects, Sources: plan.Sources,
			Calls: []Call{{Export: "grow"}}, Targets: gb.writeSites,
		})
		c.Check(err == nil, "grow Analyze failed: %v", err)
		gEv := findEvent(gReport.Writes, uint32(gfn), growStoreOff)
		c.Check(gEv != nil && gEv.Kind == "object-overflow" && gEv.Committed && gEv.FirstInvalid == 65536,
			"write into grown page must stay linearly legal but outside objects: %+v", gEv)
	})
}

// TestCrossCallEvidence drives the public sample and checks cross-call state,
// event identity, trap continuation, victim evidence and report independence.
func TestCrossCallEvidence(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		bin, plan, err := loadCrosscallSample()
		c.Check(err == nil, "load sample: %v", err)
		report, err := Analyze(context.Background(), bin, plan)
		c.Check(err == nil, "Analyze sample: %v", err)

		c.Check(len(report.Calls) == 8, "want 8 call reports, got %d", len(report.Calls))
		c.Check(report.Calls[5].Status == "trap" && len(report.Calls[5].Results) == 0,
			"sixth call must trap with empty results: %+v", report.Calls[5])
		c.Check(report.Calls[6].Status == "ok" && report.Calls[7].Status == "ok",
			"calls after a trap must continue: %+v", report.Calls)

		// Every event location matches a manifest target and ids are dense.
		for i, ev := range report.Writes {
			c.Check(ev.ID == uint32(i+1), "event ids must be dense from 1 across calls")
			c.Check(ev.Call >= 1 && ev.Call <= 8, "event call index out of range")
			matched := false
			for _, tg := range plan.Targets {
				if tg.Function == ev.Function && tg.Offset == ev.Offset && tg.Object == ev.Target {
					matched = true
				}
			}
			c.Check(matched, "event %d has no matching manifest target", ev.ID)
		}

		// The helper copy that spills across the buffer/state boundary.
		var spill *Event
		for i := range report.Writes {
			if report.Writes[i].Opcode == "memory.copy" && report.Writes[i].Kind == "object-overflow" {
				spill = &report.Writes[i]
			}
		}
		c.Check(spill != nil, "expected an object-overflow memory.copy event")
		if spill != nil {
			c.Check(spill.Target == "buffer" && len(spill.Victims) == 1 && spill.Victims[0] == "state",
				"spill must victimize state: %+v", spill.Victims)
			c.Check(len(spill.Address.Writes) > 0, "copy address must reference earlier cross-call writes: %+v", spill.Address)
		}

		// The trapping store is recorded but not committed, and the earlier
		// write in the same call is preserved.
		var trapEv *Event
		for i := range report.Writes {
			if report.Writes[i].Kind == "linear-oob" {
				trapEv = &report.Writes[i]
			}
		}
		c.Check(trapEv != nil && !trapEv.Committed, "trapping write event missing or marked committed")
		c.Check(len(report.Objects) == 3, "final snapshot must cover all objects")
		var buffer *ObjectSnapshot
		for i := range report.Objects {
			if report.Objects[i].ID == "buffer" {
				buffer = &report.Objects[i]
			}
		}
		c.Check(buffer != nil && len(buffer.Bytes) == 8, "buffer snapshot wrong: %+v", buffer)

		// Counter-example: two independent Analyze runs share no event state.
		report2, err := Analyze(context.Background(), bin, plan)
		c.Check(err == nil, "second Analyze failed: %v", err)
		c.Check(len(report2.Writes) == len(report.Writes), "independent runs should produce the same event count")
		c.Check(report2.Writes[0].ID == 1 && report2.Writes[0].Call == 1,
			"fresh run must restart event numbering: %+v", report2.Writes[0])

		// A corrupted manifest (unknown target object) is rejected outright.
		bad := plan
		bad.Targets = append([]Target(nil), plan.Targets...)
		bad.Targets[0].Object = "missing"
		_, err = Analyze(context.Background(), bin, bad)
		c.Check(err != nil, "unknown target object must be rejected")

		// Global origins persist across exported calls while local/temporary
		// state is reset between them.
		gBin := buildGlobalPersistenceModule()
		gPlan := Plan{
			Objects: []Object{{ID: "objA", Base: 0, Size: 8}},
			Sources: []Source{{ID: "g-in", Offset: 32, Bytes: []byte{0x5a}}},
			Calls:   []Call{{Export: "setg"}, {Export: "getg"}},
			Targets: []Target{{Function: 1, Offset: globalGetgStoreOffset, Object: "objA"}},
		}
		gReport, err := Analyze(context.Background(), gBin, gPlan)
		if err != nil {
			c.Check(false, "global persistence Analyze failed: %v", err)
		} else {
			c.Check(len(gReport.Writes) == 1, "only getg contains a write, got %d events", len(gReport.Writes))
			gEv := findEvent(gReport.Writes, 1, globalGetgStoreOffset)
			c.Check(gEv != nil && gEv.Call == 2 && contains(gEv.Data.Sources, "g-in"),
				"global must carry the input origin across calls: %+v", gEv)
			c.Check(gReport.Objects[0].Bytes[0] == 0x5a, "sourced global value must reach memory")
		}
	})
}

// TestAnalysisLifecycle covers invalid manifests, unsupported dead code,
// trap-then-reenter behavior, context cancellation and input immutability.
func TestAnalysisLifecycle(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		const (
			i32Const  = 0x41
			i32Store  = 0x36
			i32Store8 = 0x3a
			unreach   = 0x00
		)
		// Module whose first function writes then traps; the second re-enters
		// with a clean constant write.
		b := newWasmBuilder()
		trapFn := b.addFunction(0, "traps")
		b.emit(trapFn, i32Const, 0, i32Const, 9)
		committedOff := b.markStore(trapFn, "objA")
		b.emit(trapFn, i32Store, 2, 0, unreach)
		nextFn := b.addFunction(0, "after")
		b.emit(nextFn, i32Const, 1, i32Const, 0)
		reenterOff := b.markStore(nextFn, "objA")
		b.emit(nextFn, i32Store8, 0, 0)
		bin := b.build()
		plan := Plan{
			Objects: []Object{{ID: "objA", Base: 0, Size: 8}},
			Calls:   []Call{{Export: "traps"}, {Export: "after"}},
			Targets: b.writeSites,
		}
		report, err := Analyze(context.Background(), bin, plan)
		c.Check(err == nil, "Analyze with trapping call failed: %v", err)
		c.Check(report.Calls[0].Status == "trap" && report.Calls[1].Status == "ok",
			"trap must be recorded and execution must continue: %+v", report.Calls)
		first := findEvent(report.Writes, uint32(trapFn), committedOff)
		reenter := findEvent(report.Writes, uint32(nextFn), reenterOff)
		c.Check(first != nil && first.Committed, "write before trap must be committed: %+v", first)
		c.Check(reenter != nil && reenter.Call == 2, "reentry event must belong to the second call: %+v", reenter)
		c.Check(reenter != nil && len(reenter.Data.Sources) == 0,
			"temporary labels must not leak across calls: %+v", reenter)
		var snap *ObjectSnapshot
		for i := range report.Objects {
			if report.Objects[i].ID == "objA" {
				snap = &report.Objects[i]
			}
		}
		c.Check(snap != nil && snap.Bytes[0] == 9 && snap.Bytes[1] == 0,
			"pre-trap byte must survive and the clean reentry write must land: %v", snap)

		// Invalid manifests are rejected before the first function executes.
		overlap := plan
		overlap.Objects = []Object{{ID: "a", Base: 0, Size: 4}, {ID: "b", Base: 2, Size: 4}}
		_, err = Analyze(context.Background(), bin, overlap)
		c.Check(err != nil, "overlapping objects must be rejected")

		dup := plan
		dup.Targets = append([]Target(nil), plan.Targets...)
		dup.Targets = append(dup.Targets, dup.Targets[0])
		_, err = Analyze(context.Background(), bin, dup)
		c.Check(err != nil, "duplicate targets must be rejected")

		missing := plan
		missing.Targets = append([]Target(nil), plan.Targets...)
		missing.Targets[0].Offset = 1 // points at an immediate byte, not an opcode
		_, err = Analyze(context.Background(), bin, missing)
		c.Check(err != nil, "targets pointing at immediate bytes must be rejected")

		badCall := plan
		badCall.Calls = []Call{{Export: "ghost"}}
		_, err = Analyze(context.Background(), bin, badCall)
		c.Check(err != nil, "unknown exports must be rejected")

		// Dead code is still decoded: i64.const after unreachable is rejected
		// even though ordinary validation considers it unreachable.
		deadBin := buildUnsupportedDeadCodeModule()
		deadPlan := Plan{
			Objects: []Object{{ID: "objA", Base: 0, Size: 8}},
			Calls:   []Call{{Export: "dead"}},
			Targets: []Target{{Function: 0, Offset: 0, Object: "objA"}},
		}
		_, err = Analyze(context.Background(), deadBin, deadPlan)
		c.Check(err != nil, "unsupported instruction in dead code must be rejected")

		// Context cancellation interrupts an infinite loop.
		loopBin := buildLoopModule()
		loopPlan := Plan{
			Objects: []Object{{ID: "objA", Base: 0, Size: 8}},
			Calls:   []Call{{Export: "spin"}},
		}
		ctx, cancel := context.WithCancel(context.Background())
		go func() {
			time.Sleep(50 * time.Millisecond)
			cancel()
		}()
		_, err = Analyze(ctx, loopBin, loopPlan)
		c.Check(err != nil, "canceled context must interrupt execution")

		// Inputs are never modified.
		binCopy := append([]byte(nil), bin...)
		planCopy := clonePlan(plan)
		_, err = Analyze(context.Background(), bin, plan)
		c.Check(err == nil, "re-run for immutability check failed: %v", err)
		c.Check(string(bin) == string(binCopy), "binary input must not be modified")
		c.Check(plansEqual(plan, planCopy), "plan input must not be modified")

		// Counter-example: empty collections marshal as [], never null.
		mb := newWasmBuilder()
		mfn := mb.addFunction(0, "one")
		mb.emit(mfn, i32Const, 0, i32Const, 0)
		onlyOff := mb.markStore(mfn, "objA")
		mb.emit(mfn, i32Store, 2, 0)
		legalOnly := Plan{
			Objects: []Object{{ID: "objA", Base: 0, Size: 8}},
			Calls:   []Call{{Export: "one"}},
			Targets: []Target{{Function: uint32(mfn), Offset: onlyOff, Object: "objA"}},
		}
		emptyReport, err := Analyze(context.Background(), mb.build(), legalOnly)
		c.Check(err == nil, "minimal plan failed: %v", err)
		c.Check(len(emptyReport.Writes) == 1 && len(emptyReport.Writes[0].Victims) == 0,
			"legal event should have no victims")
		c.Check(jsonArrayNotEmpty(emptyReport), "report slices must serialize as empty arrays")
	})
}
