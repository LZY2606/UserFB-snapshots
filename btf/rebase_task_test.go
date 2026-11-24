package btf_test

import (
	"bytes"
	"encoding/binary"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/cilium/ebpf/btf"
	"github.com/cilium/ebpf/task_support/fixture"
	"github.com/cilium/ebpf/task_support/taskcheck"
)

var analysisOrders = []struct {
	name string
	bo   binary.ByteOrder
}{
	{"LE", binary.LittleEndian},
	{"BE", binary.BigEndian},
}

// loadAnalysisSpecs parses the synthetic fixture through the production
// parsers. Returned byte slices are copies so input immutability can be
// asserted later.
func loadAnalysisSpecs(t *testing.T, order binary.ByteOrder) (oldBase, split, newBase *btf.Spec, blobs [4][]byte) {
	t.Helper()

	oldBaseBlob, splitBlob, newBaseBlob, memory := fixture.Analysis(order)
	blobs = [4][]byte{
		append([]byte(nil), oldBaseBlob...),
		append([]byte(nil), splitBlob...),
		append([]byte(nil), newBaseBlob...),
		append([]byte(nil), memory...),
	}

	var err error
	oldBase, err = btf.LoadSpecFromReader(bytes.NewReader(oldBaseBlob))
	if err != nil {
		t.Fatalf("load old base: %v", err)
	}

	split, err = btf.LoadSplitSpecFromReader(bytes.NewReader(splitBlob), oldBase)
	if err != nil {
		t.Fatalf("load split: %v", err)
	}

	newBase, err = btf.LoadSpecFromReader(bytes.NewReader(newBaseBlob))
	if err != nil {
		t.Fatalf("load new base: %v", err)
	}

	return oldBase, split, newBase, blobs
}

func countAll(t *testing.T, spec *btf.Spec) int {
	t.Helper()
	n := 0
	for typ, err := range spec.All() {
		if err != nil {
			t.Fatalf("All: %v", err)
		}
		_ = typ
		n++
	}
	return n
}

func findRow(rows []btf.LayoutRow, endianness, phase, member string) btf.LayoutRow {
	for _, row := range rows {
		if row.Endianness == endianness && row.Phase == phase && row.Member == member {
			return row
		}
	}
	return btf.LayoutRow{}
}

// TestRebaseIdentity verifies that Rebase keeps local object identity, order,
// names, offsets, cycles, tags and the All/TypeByID/TypeID correspondence,
// while base edges and ID numbering move onto the replacement base. Hot types
// (inflated before Rebase) and cold types (inflated after) are interleaved.
func TestRebaseIdentity(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		for _, tc := range analysisOrders {
			oldBase, split, newBase, blobs := loadAnalysisSpecs(t, tc.bo)
			oldBaseBlob, splitBlob, newBaseBlob, memory := blobs[0], blobs[1], blobs[2], blobs[3]

			// Hot query before Rebase: keep the leaf object, as an upper
			// layer would. The pointer and decl tag stay cold for now.
			var leaf *btf.Struct
			if err := split.TypeByName("leaf", &leaf); err != nil {
				t.Fatalf("leaf lookup: %v", err)
			}

			oldInt, err := oldBase.TypeByID(1)
			if err != nil {
				t.Fatalf("old base int: %v", err)
			}

			c.Check(leaf.Size == 16, "[%s] leaf size before rebase = %d", tc.name, leaf.Size)
			c.Check(leaf.Members[0].Name == "member" && leaf.Members[0].Offset == 0, "[%s] member name/offset from parsed graph", tc.name)
			c.Check(leaf.Members[1].Name == "value" && leaf.Members[1].Offset == 64, "[%s] value name/offset from parsed graph", tc.name)
			c.Check(leaf.Members[1].Type == oldInt, "[%s] value edge reaches old base object before rebase", tc.name)

			mapping, err := split.Rebase(newBase, map[btf.TypeID]btf.TypeID{1: 2})
			if err != nil {
				t.Fatalf("[%s] rebase: %v", tc.name, err)
			}

			// The returned table covers every old record, including the
			// decl tag hidden by All.
			c.Check(len(mapping) == 5, "[%s] mapping covers void + base + 3 local records, got %d", tc.name, len(mapping))
			c.Check(mapping[0] == 0, "[%s] void stays zero", tc.name)
			c.Check(mapping[1] == 2, "[%s] base id 1 moves to id 2", tc.name)
			c.Check(mapping[2] == 3 && mapping[3] == 4 && mapping[4] == 5, "[%s] local records keep order after new base", tc.name)

			// Same leaf object, same layout, same names.
			var leafAgain *btf.Struct
			if err := split.TypeByName("leaf", &leafAgain); err != nil {
				t.Fatalf("[%s] leaf lookup after rebase: %v", tc.name, err)
			}
			c.Check(leafAgain == leaf, "[%s] leaf object identity is preserved across rebase", tc.name)
			c.Check(leaf.Members[0].Name == "member" && leaf.Members[1].Name == "value", "[%s] member names survive different base string length", tc.name)
			c.Check(leaf.Members[0].Offset == 0 && leaf.Members[1].Offset == 64, "[%s] member offsets unchanged", tc.name)
			c.Check(leaf.Size == 16, "[%s] struct size unchanged", tc.name)

			// Self reference stays a cycle on the same object.
			ptr, ok := leaf.Members[0].Type.(*btf.Pointer)
			c.Check(ok, "[%s] first member is still a pointer", tc.name)
			c.Check(ptr.Target == leaf, "[%s] self reference still points at retained leaf object", tc.name)

			// Base edge now reaches the new base object, not the old one.
			newInt, err := newBase.TypeByID(2)
			if err != nil {
				t.Fatalf("[%s] new base id 2: %v", tc.name, err)
			}
			c.Check(leaf.Members[1].Type == newInt, "[%s] value edge rewired to new base object", tc.name)
			c.Check(leaf.Members[1].Type != oldInt, "[%s] old base graph is not carried along", tc.name)

			// TypeID and TypeByID correspond and use the new numbering.
			leafID, err := split.TypeID(leaf)
			if err != nil {
				t.Fatalf("[%s] TypeID leaf: %v", tc.name, err)
			}
			c.Check(leafID == 4, "[%s] leaf id follows new base, got %d", tc.name, leafID)
			byID, err := split.TypeByID(4)
			c.Check(err == nil && byID == leaf, "[%s] TypeByID(4) returns the retained leaf", tc.name)

			// Cold queries after Rebase: pointer and the hidden decl tag were
			// never inflated before. Their edges must resolve through the new
			// base/local numbering as well.
			coldPtr, err := split.TypeByID(3)
			c.Check(err == nil, "[%s] cold pointer inflates after rebase", tc.name)
			p, ok := coldPtr.(*btf.Pointer)
			c.Check(ok, "[%s] id 3 is a pointer", tc.name)
			if ok {
				c.Check(p.Target == leaf, "[%s] cold pointer target is retained leaf", tc.name)
			}

			coldTag, err := split.TypeByID(5)
			c.Check(err == nil, "[%s] hidden decl tag still has an id after rebase", tc.name)
			c.Check(coldTag != nil, "[%s] decl tag record is retained in id space", tc.name)
			c.Check(leaf.Members[0].Tags[0] == "mark", "[%s] declaration tag still attached to member", tc.name)

			// All enumerates module types only and hides the decl tag.
			c.Check(countAll(t, split) == 2, "[%s] All lists only the two visible local records", tc.name)
			_, err = split.AnyTypeByName("int")
			c.Check(errors.Is(err, btf.ErrNotFound), "[%s] name lookup does not leak base types: %v", tc.name, err)
			_, err = split.AnyTypeByName("leaf")
			c.Check(err == nil, "[%s] local name lookup still works", tc.name)

			// TypeByID below the local range resolves in the new base.
			baseViaSplit, err := split.TypeByID(2)
			c.Check(err == nil && baseViaSplit == newInt, "[%s] base ids resolve against new base", tc.name)

			// Inputs and both base specs are untouched.
			c.Check(bytes.Equal(blobs[0], oldBaseBlob) && bytes.Equal(blobs[1], splitBlob), "[%s] input BTF bytes unchanged", tc.name)
			c.Check(bytes.Equal(blobs[2], newBaseBlob) && bytes.Equal(blobs[3], memory), "[%s] new base and memory bytes unchanged", tc.name)
			if oldIntStill, err := oldBase.TypeByID(1); err == nil {
				c.Check(true, "[%s] old base still usable", tc.name)
				c.Check(oldIntStill == oldInt, "[%s] old base spec not modified", tc.name)
			} else {
				c.Check(false, "[%s] old base still usable: %v", tc.name, err)
			}
			if newIntStill, err := newBase.TypeByID(2); err == nil {
				c.Check(true, "[%s] new base still usable", tc.name)
				c.Check(newIntStill == newInt, "[%s] new base spec not modified", tc.name)
			} else {
				c.Check(false, "[%s] new base still usable: %v", tc.name, err)
			}
		}
	})
}

// TestRebaseLookup drives the shared analysis entry point, checks the parsed
// layout and fragment interpretation for both byte orders and both phases, and
// emits the TSV and Markdown deliverables.
func TestRebaseLookup(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		reports := make([]*btf.LayoutReport, 0, len(analysisOrders))

		for _, tc := range analysisOrders {
			oldBaseBlob, splitBlob, newBaseBlob, memory := fixture.Analysis(tc.bo)

			report, err := btf.RunRebaseLayoutAnalysis(
				oldBaseBlob, splitBlob, newBaseBlob, memory,
				tc.bo, tc.name, map[btf.TypeID]btf.TypeID{1: 2},
			)
			if err != nil {
				t.Fatalf("[%s] analysis: %v", tc.name, err)
			}
			reports = append(reports, report)

			c.Check(report.Leaf != nil, "[%s] report retains leaf object", tc.name)
			c.Check(len(report.Rows) == 4, "[%s] two members times two phases, got %d rows", tc.name, len(report.Rows))

			ptrBefore := findRow(report.Rows, tc.name, "before-rebase", "member")
			ptrAfter := findRow(report.Rows, tc.name, "after-rebase", "member")
			intBefore := findRow(report.Rows, tc.name, "before-rebase", "value")
			intAfter := findRow(report.Rows, tc.name, "after-rebase", "value")

			c.Check(ptrBefore.StructID == 3 && ptrAfter.StructID == 4, "[%s] struct id moves with base", tc.name)
			c.Check(ptrBefore.StructSize == 16 && ptrAfter.StructSize == 16, "[%s] struct size reported from parsed graph", tc.name)
			c.Check(ptrBefore.BitOffset == 0 && intBefore.BitOffset == 64, "[%s] bit offsets come from members", tc.name)
			c.Check(ptrBefore.RefID == 2 && ptrAfter.RefID == 3, "[%s] pointer ref id renumbered", tc.name)
			c.Check(intBefore.RefID == 1 && intAfter.RefID == 2, "[%s] int ref id follows base mapping", tc.name)

			c.Check(ptrBefore.IsPointer && ptrAfter.IsPointer, "[%s] pointer classified from type graph", tc.name)
			c.Check(ptrBefore.TargetAddr == 0x4000 && ptrAfter.TargetAddr == 0x4000, "[%s] pointer value interpreted as address", tc.name)
			c.Check(ptrBefore.InFragment && ptrAfter.InFragment, "[%s] 0x4000 is inside the fragment", tc.name)
			c.Check(ptrBefore.PointedName == "leaf" && ptrAfter.PointedName == "leaf", "[%s] pointer target struct named from graph", tc.name)

			if tc.name == "LE" {
				c.Check(ptrBefore.RawBytes == "00 40 00 00 00 00 00 00", "[%s] LE pointer bytes: %s", tc.name, ptrBefore.RawBytes)
				c.Check(intBefore.RawBytes == "78 56 34 12", "[%s] LE int bytes: %s", tc.name, intBefore.RawBytes)
			} else {
				c.Check(ptrBefore.RawBytes == "00 00 00 00 00 00 40 00", "[%s] BE pointer bytes: %s", tc.name, ptrBefore.RawBytes)
				c.Check(intBefore.RawBytes == "12 34 56 78", "[%s] BE int bytes: %s", tc.name, intBefore.RawBytes)
			}
			c.Check(ptrBefore.RawBytes == ptrAfter.RawBytes, "[%s] pointer bytes identical across phases", tc.name)
			c.Check(intBefore.RawBytes == intAfter.RawBytes, "[%s] int bytes identical across phases", tc.name)
			c.Check(intBefore.Value == "0x12345678 (305419896)", "[%s] int interpreted value: %s", tc.name, intBefore.Value)
			c.Check(intAfter.Value == intBefore.Value, "[%s] int value identical across phases", tc.name)

			// Same retained leaf answers object and id queries.
			c.Check(report.LeafID == 4, "[%s] report leaf id is post-rebase id 4, got %d", tc.name, report.LeafID)
		}

		// Emit deliverables from the actual analysis output, both endiannesses.
		var allRows []btf.LayoutRow
		for _, report := range reports {
			allRows = append(allRows, report.Rows...)
		}

		analysisDir := filepath.Join("..", "analysis")
		if err := os.MkdirAll(analysisDir, 0o755); err != nil {
			t.Fatalf("create analysis dir: %v", err)
		}

		tsv := btf.RenderLayoutTSV(allRows)
		if err := os.WriteFile(filepath.Join(analysisDir, "btf-layout.tsv"), []byte(tsv), 0o644); err != nil {
			t.Fatalf("write tsv: %v", err)
		}

		md := btf.RenderLayoutMarkdown(reports)
		if err := os.WriteFile(filepath.Join(analysisDir, "btf-layout.md"), []byte(md), 0o644); err != nil {
			t.Fatalf("write markdown: %v", err)
		}

		c.Check(strings.Count(tsv, "\n") == 9, "tsv has header plus 8 data rows")
		c.Check(strings.Contains(tsv, "before-rebase") && strings.Contains(tsv, "after-rebase"), "tsv shows both phases")
		c.Check(strings.Contains(tsv, "LE") && strings.Contains(tsv, "BE"), "tsv shows both byte orders")
		c.Check(strings.Contains(md, "before-rebase") && strings.Contains(md, "after-rebase"), "md shows both phases")
		c.Check(strings.Contains(md, "LE") && strings.Contains(md, "BE"), "md shows both byte orders")
		c.Check(strings.Contains(md, "0x4000"), "md narrates the fragment target address")

		// An address outside the fragment must be labelled, not followed.
		oldBaseBlob, splitBlob := fixture.Original(binary.LittleEndian)
		oldBase, err := btf.LoadSpecFromReader(bytes.NewReader(oldBaseBlob))
		if err != nil {
			t.Fatalf("old base: %v", err)
		}
		spec, err := btf.LoadSplitSpecFromReader(bytes.NewReader(splitBlob), oldBase)
		if err != nil {
			t.Fatalf("split: %v", err)
		}
		outside := make([]byte, 16)
		binary.LittleEndian.PutUint64(outside, 0x9000)
		rows, err := btf.AnalyzeSpecLayout(spec, "before-rebase", "LE", btf.Fragment{
			Order: binary.LittleEndian, BaseAddress: 0x4000, PointerWidth: 8, Data: outside,
		})
		if err != nil {
			t.Fatalf("outside analysis: %v", err)
		}
		ptr := findRow(rows, "LE", "before-rebase", "member")
		c.Check(!ptr.InFragment && ptr.TargetAddr == 0x9000, "out of fragment address is marked, never dereferenced")
		c.Check(strings.Contains(btf.RenderLayoutTSV(rows), "outside fragment"), "tsv labels out of fragment pointer")
	})
}

// TestRebaseCopy checks copy isolation before and after Rebase, repeated
// rebases against differently sized bases and cold type resolution on copies.
func TestRebaseCopy(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		oldBase, split, newBase, _ := loadAnalysisSpecs(t, binary.LittleEndian)

		// Copy taken before Rebase must stay attached to the old base.
		beforeCopy := split.Copy()
		if _, err := split.Rebase(newBase, map[btf.TypeID]btf.TypeID{1: 2}); err != nil {
			t.Fatalf("rebase: %v", err)
		}

		var cpLeaf *btf.Struct
		if err := beforeCopy.TypeByName("leaf", &cpLeaf); err != nil {
			t.Fatalf("copy leaf: %v", err)
		}
		cpID, err := beforeCopy.TypeID(cpLeaf)
		c.Check(err == nil && cpID == 3, "pre-rebase copy keeps old numbering, got %d", cpID)
		cpInt, err := beforeCopy.TypeByID(1)
		c.Check(err == nil, "pre-rebase copy still resolves old base id")
		c.Check(cpLeaf.Members[1].Type == cpInt, "pre-rebase copy keeps old base edge")
		c.Check(cpLeaf.Members[0].Tags[0] == "mark", "copy keeps declaration tags")

		// Copy taken after Rebase is independent: internal cycles, own base
		// objects, no shared mutation with the original.
		afterCopy := split.Copy()
		var acLeaf *btf.Struct
		if err := afterCopy.TypeByName("leaf", &acLeaf); err != nil {
			t.Fatalf("rebased copy leaf: %v", err)
		}
		var origLeaf *btf.Struct
		if err := split.TypeByName("leaf", &origLeaf); err != nil {
			t.Fatalf("orig leaf: %v", err)
		}
		c.Check(acLeaf != origLeaf, "copy duplicates local types")
		acPtr, ok := acLeaf.Members[0].Type.(*btf.Pointer)
		c.Check(ok && acPtr.Target == acLeaf, "copy keeps its own local cycle")
		acInt, err := afterCopy.TypeByID(2)
		c.Check(err == nil, "copy resolves id through the rebased base numbering")
		c.Check(acLeaf.Members[1].Type == acInt, "copy edge points into the copied base decoder")
		c.Check(acInt != origLeaf.Members[1].Type, "copy base objects are independent instances")

		acLeaf.Name = "mutated_copy_name"
		c.Check(origLeaf.Name == "leaf", "mutating a copy does not affect the original spec")

		// Cold type on the copy: never inflated anywhere before.
		coldTag, err := afterCopy.TypeByID(5)
		c.Check(err == nil && coldTag != nil, "cold decl tag resolves through copied rebase state")
		c.Check(countAll(t, afterCopy) == 2, "copied All still hides decl tags and base types")

		// Repeated Rebase onto a larger base with a longer string table and a
		// larger type set. The correspondence is keyed by the current base ids
		// and must cover every non-zero base record.
		bigBuilder, err := btf.NewBuilder([]btf.Type{
			&btf.Int{Name: "alpha_type", Size: 4},
			&btf.Int{Name: "beta_type", Size: 8},
			&btf.Int{Name: "gamma_type", Size: 2},
		}, nil)
		if err != nil {
			t.Fatalf("big builder: %v", err)
		}
		bigBlob, err := bigBuilder.Marshal(nil, &btf.MarshalOptions{Order: binary.LittleEndian})
		if err != nil {
			t.Fatalf("marshal big base: %v", err)
		}
		bigBase, err := btf.LoadSpecFromReader(bytes.NewReader(bigBlob))
		if err != nil {
			t.Fatalf("load big base: %v", err)
		}

		mapping, err := split.Rebase(bigBase, map[btf.TypeID]btf.TypeID{1: 1, 2: 3})
		c.Check(err == nil, "repeated rebase onto larger base: %v", err)
		// Encoded base id 1 is current id 2 after the first rebase; the caller
		// maps current id 2 onto big base id 3.
		c.Check(mapping[1] == 3, "encoded base id reaches big base id 3")
		// Four-record larger base pushes local ids to start at 4.
		c.Check(mapping[3] == 5, "local leaf keeps order behind the larger base")

		// Names and offsets remain anchored to the original split strings.
		c.Check(origLeaf.Name == "leaf", "local name stable across repeated rebase")
		c.Check(origLeaf.Members[0].Name == "member" && origLeaf.Members[1].Name == "value", "member names stable across string table change")
		c.Check(origLeaf.Members[1].Offset == 64, "member offset stable")
		gammaInt, err := bigBase.TypeByID(3)
		c.Check(err == nil, "big base id 3 resolves")
		c.Check(origLeaf.Members[1].Type == gammaInt, "edge rewired a second time onto the larger base")
		c.Check(origLeaf.Members[0].Type.(*btf.Pointer).Target == origLeaf, "local cycle survives repeated rebase")

		// Cold inflation after the larger-base rebase resolves through the new
		// numbering; the pointer now sits at id 4.
		coldPtr, err := split.TypeByID(4)
		c.Check(err == nil, "cold local pointer after larger-base rebase")
		cp, ok := coldPtr.(*btf.Pointer)
		c.Check(ok, "cold pointer kind")
		if ok {
			c.Check(cp.Target == origLeaf, "cold pointer joins retained cycle")
		}

		// Rebase again onto a smaller base, collapsing every current base id
		// onto the single replacement record (many to one is allowed).
		smallBuilder, err := btf.NewBuilder([]btf.Type{&btf.Int{Name: "only_int", Size: 4}}, nil)
		if err != nil {
			t.Fatalf("small builder: %v", err)
		}
		smallBlob, err := smallBuilder.Marshal(nil, &btf.MarshalOptions{Order: binary.LittleEndian})
		if err != nil {
			t.Fatalf("marshal small base: %v", err)
		}
		smallBase, err := btf.LoadSpecFromReader(bytes.NewReader(smallBlob))
		if err != nil {
			t.Fatalf("load small base: %v", err)
		}

		smallMapping, err := split.Rebase(smallBase, map[btf.TypeID]btf.TypeID{1: 1, 2: 1, 3: 1})
		c.Check(err == nil, "rebase onto smaller many-to-one base: %v", err)
		c.Check(smallMapping[1] == 1, "encoded base id collapses onto the sole small-base record")
		c.Check(smallMapping[3] == 3, "local leaf restarts at id 3 behind the two-record small base")

		onlyInt, err := smallBase.TypeByID(1)
		c.Check(err == nil, "small base id 1 resolves")
		c.Check(origLeaf.Members[1].Type == onlyInt, "edge rewired a third time onto the smaller base")
		c.Check(origLeaf.Members[0].Name == "member", "member name still anchored to original split strings")
		smallPtr, err := split.TypeByID(2)
		c.Check(err == nil, "pointer now at id 2 behind the smaller base")
		sp, ok := smallPtr.(*btf.Pointer)
		c.Check(ok, "small-base pointer kind")
		if ok {
			c.Check(sp.Target == origLeaf, "cycle preserved through shrinking rebase")
		}

		// Old and earlier replacement bases remain intact and usable.
		_, err = oldBase.TypeByID(1)
		c.Check(err == nil, "old base untouched after repeated rebase")
		_, err = newBase.TypeByID(2)
		c.Check(err == nil, "first replacement base untouched")
		_, err = bigBase.TypeByID(3)
		c.Check(err == nil, "larger base untouched")
	})
}

// TestRebaseErrors covers invalid receivers, bases and mappings and ensures a
// failed Rebase leaves the split Spec fully usable and unchanged.
func TestRebaseErrors(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		oldBase, split, newBase, _ := loadAnalysisSpecs(t, binary.LittleEndian)

		var leaf *btf.Struct
		if err := split.TypeByName("leaf", &leaf); err != nil {
			t.Fatalf("leaf: %v", err)
		}
		leafID, err := split.TypeID(leaf)
		if err != nil {
			t.Fatalf("leaf id: %v", err)
		}

		// A base spec cannot be rebased.
		_, err = oldBase.Rebase(newBase, nil)
		c.Check(err != nil, "rebase on base spec errors")

		// Nil and split new bases are rejected.
		_, err = split.Rebase(nil, nil)
		c.Check(err != nil, "nil new base errors")

		_, splitBlob := fixture.Original(binary.LittleEndian)
		splitAsBase, err := btf.LoadSplitSpecFromReader(bytes.NewReader(splitBlob), oldBase)
		if err != nil {
			t.Fatalf("split as base: %v", err)
		}
		_, err = split.Rebase(splitAsBase, nil)
		c.Check(err != nil, "split spec used as new base errors")

		// Byte order mismatch.
		opposite := mustOppositeEndianBase(t)
		_, err = split.Rebase(opposite, map[btf.TypeID]btf.TypeID{1: 1})
		c.Check(err != nil, "byte order mismatch errors")

		// Mapping problems.
		_, err = split.Rebase(newBase, nil)
		c.Check(err != nil, "nil mapping errors")
		_, err = split.Rebase(newBase, map[btf.TypeID]btf.TypeID{})
		c.Check(err != nil, "missing entry errors")
		_, err = split.Rebase(newBase, map[btf.TypeID]btf.TypeID{1: 0})
		c.Check(err != nil, "mapping to void errors")
		_, err = split.Rebase(newBase, map[btf.TypeID]btf.TypeID{1: 99})
		c.Check(err != nil, "out of range target id errors")

		// A valid rebase after the failed attempts still works, proving the
		// decoder stayed consistent.
		mapping, err := split.Rebase(newBase, map[btf.TypeID]btf.TypeID{1: 2})
		c.Check(err == nil, "rebase recovers after validation failures: %v", err)
		c.Check(mapping[1] == 2, "valid mapping applied after failures")

		// Nothing changed before the successful call.
		c.Check(leafID == 3, "failed rebases kept original numbering")
		var sameLeaf *btf.Struct
		if err := split.TypeByName("leaf", &sameLeaf); err != nil {
			t.Fatalf("leaf after failures: %v", err)
		}
		c.Check(sameLeaf == leaf, "failed rebases kept the same objects")
		c.Check(sameLeaf.Members[0].Name == "member", "names intact after failures")
		c.Check(countAll(t, split) == 2, "All intact after failures and rebase")
	})
}

func mustOppositeEndianBase(t *testing.T) *btf.Spec {
	t.Helper()
	// Build a legal big-endian base independently of the little-endian split.
	builder, err := btf.NewBuilder([]btf.Type{&btf.Int{Name: "int", Size: 4}}, nil)
	if err != nil {
		t.Fatalf("builder: %v", err)
	}
	blob, err := builder.Marshal(nil, &btf.MarshalOptions{Order: binary.BigEndian})
	if err != nil {
		t.Fatalf("marshal: %v", err)
	}
	spec, err := btf.LoadSpecFromReader(bytes.NewReader(blob))
	if err != nil {
		t.Fatalf("load: %v", err)
	}
	return spec
}

// Example_rebaseLayoutAnalysis is a repeatable offline analysis demo. It uses
// only the production parsers and never touches a running kernel.
func Example_rebaseLayoutAnalysis() {
	for _, tc := range analysisOrders {
		oldBaseBlob, splitBlob, newBaseBlob, memory := fixture.Analysis(tc.bo)
		report, err := btf.RunRebaseLayoutAnalysis(
			oldBaseBlob, splitBlob, newBaseBlob, memory,
			tc.bo, tc.name, map[btf.TypeID]btf.TypeID{1: 2},
		)
		if err != nil {
			fmt.Println("analysis failed:", err)
			return
		}

		fmt.Printf("%s leaf=%d rows=%d base1->%d\n",
			report.Endianness, report.LeafID, len(report.Rows), report.Mapping[1])
	}
	// Output:
	// LE leaf=4 rows=4 base1->2
	// BE leaf=4 rows=4 base1->2
}
