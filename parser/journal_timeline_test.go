package parser

import (
	"context"
	"encoding/binary"
	"fmt"
	"strings"
	"testing"
)

// This file builds USN_RECORD_V2 bytes from Go code and feeds them to
// the JournalTimeline. At least one test group runs every record through
// the real ParseUSN channel instead of constructing timeline inputs by
// hand.

// usnSpec is one synthetic journal record. Timestamps are deliberately
// left out: the encoder writes values that do not track USN order, to
// prove the timeline orders strictly by USN.
type usnSpec struct {
	usn    uint64
	object JournalIdentity
	parent JournalIdentity
	reason uint32
	attrs  uint32
	name   string
}

func utf16Encode(s string) []byte {
	out := make([]byte, 0, 2*len(s))
	buf := make([]byte, 2)
	for _, r := range s {
		binary.LittleEndian.PutUint16(buf, uint16(r))
		out = append(out, buf...)
	}
	return out
}

func fileReference(id JournalIdentity) uint64 {
	return id.RecordNumber | uint64(id.Sequence)<<48
}

// encodeRawUSN packs the specs into a contiguous $J style byte stream.
func encodeRawUSN(specs []usnSpec) []byte {
	raw := []byte{}
	for _, s := range specs {
		utf16 := utf16Encode(s.name)
		length := (60 + len(utf16) + 7) &^ 7

		rec := make([]byte, length)
		binary.LittleEndian.PutUint32(rec[0:], uint32(length))
		binary.LittleEndian.PutUint16(rec[4:], 2) // MajorVersion
		binary.LittleEndian.PutUint16(rec[6:], 0) // MinorVersion
		binary.LittleEndian.PutUint64(rec[8:], fileReference(s.object))
		binary.LittleEndian.PutUint64(rec[16:], fileReference(s.parent))
		binary.LittleEndian.PutUint64(rec[24:], s.usn)
		// A timestamp that is plainly not ordered with the USN: the
		// timeline must ignore it for ordering purposes.
		binary.LittleEndian.PutUint64(rec[32:], 1000000-s.usn)
		binary.LittleEndian.PutUint32(rec[40:], s.reason)
		binary.LittleEndian.PutUint32(rec[52:], s.attrs)
		binary.LittleEndian.PutUint16(rec[56:], uint16(len(utf16)))
		binary.LittleEndian.PutUint16(rec[58:], 60)
		copy(rec[60:], utf16)

		raw = append(raw, rec...)
	}
	return raw
}

// parseSpecs sends synthetic bytes through the real ParseUSN pipeline
// and returns the parsed records in stream order. A nil testing.T is
// allowed so runnable examples can use the same harness.
func parseSpecs(t *testing.T, specs []usnSpec) []*USN_RECORD {
	if t != nil {
		t.Helper()
	}
	fail := func(format string, args ...interface{}) {
		if t != nil {
			t.Fatalf(format, args...)
		}
		panic(fmt.Sprintf(format, args...))
	}
	raw := encodeRawUSN(specs)
	ntfs := newTestContext(0x1000)

	out := []*USN_RECORD{}
	for record := range ParseUSN(context.Background(), ntfs,
		newRangeBuffer(raw), 0) {
		if !record.Validate() {
			fail("ParseUSN emitted an invalid record at %#x", record.Offset)
		}
		out = append(out, record)
	}
	if len(out) != len(specs) {
		fail("ParseUSN returned %d records, want %d", len(out), len(specs))
	}
	return out
}

func applySpecs(t *testing.T, tl *JournalTimeline, specs []usnSpec) {
	t.Helper()
	for _, record := range parseSpecs(t, specs) {
		if err := tl.ApplyRecord(record); err != nil {
			t.Fatalf("ApplyRecord(usn=%d) failed: %v", record.Usn(), err)
		}
	}
}

func makeObject(id uint64, seq uint16, isDir bool,
	links ...JournalLink) JournalObject {
	return JournalObject{
		Identity:    JournalIdentity{RecordNumber: id, Sequence: seq},
		IsDirectory: isDir,
		Links:       links,
	}
}

func linkTo(parent uint64, parentSeq uint16, name string) JournalLink {
	return JournalLink{
		Parent: JournalIdentity{RecordNumber: parent, Sequence: parentSeq},
		Name:   name,
	}
}

func mustPaths(t *testing.T, tl *JournalTimeline, usn uint64,
	object JournalIdentity) *JournalPathView {
	t.Helper()
	view, err := tl.PathsAt(usn, object)
	if err != nil {
		t.Fatalf("PathsAt(usn=%d, %v) failed: %v", usn, object, err)
	}
	return view
}

func expectStrings(t *testing.T, label string, got, want []string) {
	t.Helper()
	if len(got) != len(want) {
		t.Fatalf("%s: got %v, want %v", label, got, want)
	}
	for i := range want {
		if got[i] != want[i] {
			t.Fatalf("%s: got %v, want %v", label, got, want)
		}
	}
}

func expectReasons(t *testing.T, label string,
	items []JournalUnresolved, want ...UnresolvedReason) {
	t.Helper()
	if len(items) != len(want) {
		t.Fatalf("%s: got %d unresolved items %+v, want %d",
			label, len(items), items, len(want))
	}
	for i, reason := range want {
		if items[i].Reason != reason {
			t.Fatalf("%s: item %d reason %q, want %q (full: %+v)",
				label, i, items[i].Reason, reason, items[i])
		}
	}
}

// TestJournalTimelineRenames covers per object OLD/NEW pairing with
// records of other objects interleaving, retained hard links, hard link
// create/delete, orphan rename evidence, conflicting reasons and
// non increasing USNs.
func TestJournalTimelineRenames(t *testing.T) {
	root := JournalIdentity{RecordNumber: 5, Sequence: 1}
	dirA := JournalIdentity{RecordNumber: 10, Sequence: 1}
	dirB := JournalIdentity{RecordNumber: 11, Sequence: 1}
	fileF := JournalIdentity{RecordNumber: 20, Sequence: 1}
	fileG := JournalIdentity{RecordNumber: 21, Sequence: 1}

	initial := []JournalObject{
		makeObject(5, 1, true),
		makeObject(10, 1, true, linkTo(5, 1, "A")),
		makeObject(11, 1, true, linkTo(5, 1, "B")),
		// F starts with two hard links in different directories.
		makeObject(20, 1, false,
			linkTo(10, 1, "f.txt"), linkTo(11, 1, "f.txt")),
		makeObject(21, 1, false, linkTo(5, 1, "g.txt")),
	}

	tl, err := NewJournalTimeline(root, initial)
	if err != nil {
		t.Fatalf("NewJournalTimeline: %v", err)
	}

	specs := []usnSpec{
		// F's rename opens with OLD.
		{100, fileF, dirA, usnReasonRenameOldName, 0, "f.txt"},
		// A data change for an unrelated object interleaves before F's
		// NEW arrives; it must not disturb F's pending pairing.
		{110, fileG, root, usnReasonDataOverwrite, 0, "g.txt"},
		// G's own complete rename, sandwiched inside F's open pair.
		{120, fileG, root, usnReasonRenameOldName, 0, "g.txt"},
		{130, fileG, root, usnReasonRenameNewName, 0, "g2.txt"},
		// F's NEW finally arrives and completes its pair.
		{140, fileF, dirA, usnReasonRenameNewName, 0, "renamed.txt"},
		// F gains a third hard link at the root, then loses the B link.
		{150, fileF, root, usnReasonFileCreate, 0, "f-root.txt"},
		{160, fileF, dirB, usnReasonFileDelete, 0, "f.txt"},
	}
	applySpecs(t, tl, specs)

	// OLD alone never moves anything.
	view := mustPaths(t, tl, 100, fileF)
	expectStrings(t, "F at 100", view.Paths,
		[]string{`\A\f.txt`, `\B\f.txt`})
	if len(view.Unresolved) != 0 {
		t.Fatalf("F at 100 should be fully resolved, got %+v", view.Unresolved)
	}

	// G renames independently of F's open pair.
	view = mustPaths(t, tl, 130, fileG)
	expectStrings(t, "G at 130", view.Paths, []string{`\g2.txt`})

	// F's NEW replaces only the A link; its B hard link survives.
	view = mustPaths(t, tl, 140, fileF)
	expectStrings(t, "F at 140", view.Paths,
		[]string{`\A\renamed.txt`, `\B\f.txt`})

	// Third link added.
	view = mustPaths(t, tl, 150, fileF)
	expectStrings(t, "F at 150", view.Paths,
		[]string{`\A\renamed.txt`, `\B\f.txt`, `\f-root.txt`})

	// Deleting one named link leaves the other two.
	view = mustPaths(t, tl, 160, fileF)
	expectStrings(t, "F at 160", view.Paths,
		[]string{`\A\renamed.txt`, `\f-root.txt`})

	// A real hard link creation arrives as a combined reason
	// (FILE_CREATE | HARD_LINK_CHANGE). The combined reason must be
	// processed, not treated as a conflict: G gains a second link while
	// keeping its existing one.
	applySpecs(t, tl, []usnSpec{
		{170, fileG, dirB,
			usnReasonFileCreate | usnReasonHardLinkChange, 0, "g-link.txt"},
	})
	view = mustPaths(t, tl, 170, fileG)
	expectStrings(t, "G combined hardlink", view.Paths,
		[]string{`\B\g-link.txt`, `\g2.txt`})

	// Orphan NEW: H has no preceding OLD, so it must not be moved.
	objH := JournalIdentity{RecordNumber: 30, Sequence: 1}
	orphanNEW := parseSpecs(t, []usnSpec{
		{200, objH, root, usnReasonRenameNewName, 0, "h.txt"},
	})
	if err := tl.ApplyRecord(orphanNEW[0]); err != nil {
		t.Fatalf("orphan NEW apply: %v", err)
	}
	view = mustPaths(t, tl, 200, objH)
	if len(view.Paths) != 0 {
		t.Fatalf("orphan NEW produced paths %v, want none", view.Paths)
	}
	expectReasons(t, "H at 200", view.Unresolved, UnresolvedOrphan)

	// Two consecutive OLD records for K with no NEW between them: the
	// second is kept as diagnostic and the timeline does not guess.
	objK := JournalIdentity{RecordNumber: 31, Sequence: 1}
	applySpecs(t, tl, []usnSpec{
		{210, objK, root, usnReasonRenameOldName, 0, "k.txt"},
		{220, objK, root, usnReasonRenameOldName, 0, "k2.txt"},
	})
	diags := tl.Diagnostics()
	if len(diags) < 2 {
		t.Fatalf("want at least 2 diagnostics (orphan NEW, double OLD), got %+v", diags)
	}
	foundDoubleOld := false
	for _, d := range diags {
		if strings.Contains(d.Evidence, "second RENAME_OLD_NAME") {
			foundDoubleOld = true
		}
	}
	if !foundDoubleOld {
		t.Fatalf("double OLD diagnostic missing: %+v", diags)
	}
	view = mustPaths(t, tl, 220, objK)
	expectReasons(t, "K at 220", view.Unresolved, UnresolvedOrphan)

	// Diagnostics return independent copies.
	diags[0].Evidence = "mutated"
	diags[0].Segment = append(diags[0].Segment, "mutated")
	if tl.Diagnostics()[0].Evidence == "mutated" {
		t.Fatal("Diagnostics() borrowed internal storage")
	}

	// Query results do not borrow internal slices either.
	view.Paths = append(view.Paths, "injected")
	again := mustPaths(t, tl, 220, objK)
	for _, p := range again.Paths {
		if p == "injected" {
			t.Fatal("PathsAt borrowed its Paths slice")
		}
	}

	countBefore := tl.AppliedCount()
	lastBefore, _ := tl.LastUSN()

	// Conflicting reason combination: explicit error, state untouched.
	conflict := parseSpecs(t, []usnSpec{
		{230, fileF, dirA,
			usnReasonRenameOldName | usnReasonRenameNewName, 0, "x.txt"},
	})
	err = tl.ApplyRecord(conflict[0])
	if err == nil || !strings.Contains(err.Error(), "RENAME_OLD_NAME") {
		t.Fatalf("conflicting reasons error missing, got %v", err)
	}

	// Repeated / backwards USN: error, and the prior state is preserved.
	backwards := parseSpecs(t, []usnSpec{
		{50, fileG, root, usnReasonDataOverwrite, 0, "g2.txt"},
	})
	if err := tl.ApplyRecord(backwards[0]); err == nil {
		t.Fatal("backwards USN should be rejected")
	}
	sameUSN := parseSpecs(t, []usnSpec{
		{220, fileG, root, usnReasonDataOverwrite, 0, "g2.txt"},
	})
	if err := tl.ApplyRecord(sameUSN[0]); err == nil {
		t.Fatal("duplicate USN should be rejected")
	}
	if tl.AppliedCount() != countBefore {
		t.Fatalf("failed apply changed record count: %d != %d",
			tl.AppliedCount(), countBefore)
	}
	if last, _ := tl.LastUSN(); last != lastBefore {
		t.Fatalf("failed apply moved last USN: %d != %d", last, lastBefore)
	}
}

// TestJournalTimelineAncestors moves a directory and checks descendant
// paths before and after the move, with no rename records synthesised
// for the descendants. Every record enters through the real ParseUSN
// channel.
func TestJournalTimelineAncestors(t *testing.T) {
	root := JournalIdentity{RecordNumber: 5, Sequence: 1}
	dirOld := JournalIdentity{RecordNumber: 40, Sequence: 1}
	dirChild := JournalIdentity{RecordNumber: 41, Sequence: 1}
	fileDeep := JournalIdentity{RecordNumber: 42, Sequence: 1}
	dirNew := JournalIdentity{RecordNumber: 43, Sequence: 1}
	same1 := JournalIdentity{RecordNumber: 44, Sequence: 1}
	same2 := JournalIdentity{RecordNumber: 45, Sequence: 1}

	initial := []JournalObject{
		makeObject(5, 1, true),
		makeObject(40, 1, true, linkTo(5, 1, "Old")),
		makeObject(41, 1, true, linkTo(40, 1, "Child")),
		makeObject(42, 1, false, linkTo(41, 1, "deep.txt")),
		makeObject(43, 1, true, linkTo(5, 1, "New")),
		// Two distinct identities sharing a name under the root.
		makeObject(44, 1, false, linkTo(5, 1, "same.txt")),
		makeObject(45, 1, false, linkTo(5, 1, "same.txt")),
	}

	tl, err := NewJournalTimeline(root, initial)
	if err != nil {
		t.Fatalf("NewJournalTimeline: %v", err)
	}

	specs := []usnSpec{
		// Move Old from under root to under New.
		{300, dirOld, root, usnReasonRenameOldName,
			fileAttributeDirectory, "Old"},
		{310, dirOld, dirNew, usnReasonRenameNewName,
			fileAttributeDirectory, "Old"},
		// Rename the child directory itself; deep.txt gets no record.
		{320, dirChild, dirOld, usnReasonRenameOldName,
			fileAttributeDirectory, "Child"},
		{330, dirChild, dirOld, usnReasonRenameNewName,
			fileAttributeDirectory, "Kid"},
		// Case preservation: a mixed case rename on the deep file.
		{340, fileDeep, dirChild, usnReasonRenameOldName, 0, "deep.txt"},
		{350, fileDeep, dirChild, usnReasonRenameNewName, 0, "Deep.TXT"},
	}

	// This group deliberately goes through ParseUSN -> channel ->
	// ApplyFromChannel rather than hand built structures.
	raw := encodeRawUSN(specs)
	ntfs := newTestContext(0x1000)
	channel := ParseUSN(context.Background(), ntfs, newRangeBuffer(raw), 0)
	if err := tl.ApplyFromChannel(channel); err != nil {
		t.Fatalf("ApplyFromChannel: %v", err)
	}
	if tl.AppliedCount() != uint64(len(specs)) {
		t.Fatalf("applied %d records, want %d", tl.AppliedCount(), len(specs))
	}

	// Before the move: the OLD record alone changes nothing.
	view := mustPaths(t, tl, 300, fileDeep)
	expectStrings(t, "deep at 300", view.Paths,
		[]string{`\Old\Child\deep.txt`})
	view = mustPaths(t, tl, 300, dirChild)
	expectStrings(t, "child at 300", view.Paths, []string{`\Old\Child`})
	view = mustPaths(t, tl, 300, dirOld)
	expectStrings(t, "old dir at 300", view.Paths, []string{`\Old`})

	// After the move, descendants follow the ancestor version effective
	// at the queried USN even though they have no records of their own.
	view = mustPaths(t, tl, 310, fileDeep)
	expectStrings(t, "deep at 310", view.Paths,
		[]string{`\New\Old\Child\deep.txt`})
	view = mustPaths(t, tl, 310, dirChild)
	expectStrings(t, "child at 310", view.Paths,
		[]string{`\New\Old\Child`})
	view = mustPaths(t, tl, 310, dirOld)
	expectStrings(t, "old dir at 310", view.Paths,
		[]string{`\New\Old`})

	// After the child rename, and case is preserved exactly.
	view = mustPaths(t, tl, 330, fileDeep)
	expectStrings(t, "deep at 330", view.Paths,
		[]string{`\New\Old\Kid\deep.txt`})
	view = mustPaths(t, tl, 350, fileDeep)
	expectStrings(t, "deep at 350", view.Paths,
		[]string{`\New\Old\Kid\Deep.TXT`})

	// Historical query still works after later records were applied:
	// the version chain keeps the old graph reachable.
	view = mustPaths(t, tl, 310, fileDeep)
	expectStrings(t, "deep historical at 310", view.Paths,
		[]string{`\New\Old\Child\deep.txt`})
	view = mustPaths(t, tl, 300, fileDeep)
	expectStrings(t, "deep historical at 300", view.Paths,
		[]string{`\Old\Child\deep.txt`})

	// Same name under root, different identities: resolution stays
	// identity based.
	view = mustPaths(t, tl, 350, same1)
	expectStrings(t, "same1", view.Paths, []string{`\same.txt`})
	view = mustPaths(t, tl, 350, same2)
	expectStrings(t, "same2", view.Paths, []string{`\same.txt`})
}

// TestJournalTimelineReuse verifies that a reused record number with a
// new sequence number is a different object that inherits no links, and
// that the sequence's highest bit is preserved.
func TestJournalTimelineReuse(t *testing.T) {
	root := JournalIdentity{RecordNumber: 5, Sequence: 1}
	oldObject := JournalIdentity{RecordNumber: 50, Sequence: 1}
	// Sequence 0x8003 keeps the highest sequence bit set.
	newObject := JournalIdentity{RecordNumber: 50, Sequence: 0x8003}

	initial := []JournalObject{
		makeObject(5, 1, true),
		makeObject(50, 1, false, linkTo(5, 1, "old.txt")),
	}
	tl, err := NewJournalTimeline(root, initial)
	if err != nil {
		t.Fatalf("NewJournalTimeline: %v", err)
	}

	applySpecs(t, tl, []usnSpec{
		// A benign data change gives us an applied USN before deletion.
		{390, oldObject, root, usnReasonDataOverwrite, 0, "old.txt"},
		// The old incarnation loses its only link.
		{400, oldObject, root, usnReasonFileDelete, 0, "old.txt"},
		// The record number is reused with a new sequence: a brand new
		// object, born with its own link.
		{410, newObject, root, usnReasonFileCreate, 0, "new.txt"},
		// The new incarnation gets renamed on its own.
		{420, newObject, root, usnReasonRenameOldName, 0, "new.txt"},
		{430, newObject, root, usnReasonRenameNewName, 0, "moved.txt"},
	})

	// The old identity's history is still queryable.
	view := mustPaths(t, tl, 390, oldObject)
	expectStrings(t, "old identity before delete", view.Paths,
		[]string{`\old.txt`})

	view = mustPaths(t, tl, 400, oldObject)
	if len(view.Paths) != 0 {
		t.Fatalf("deleted identity has paths %v, want none", view.Paths)
	}
	expectReasons(t, "old identity after delete",
		view.Unresolved, UnresolvedOrphan)

	// The new identity must not inherit old.txt.
	view = mustPaths(t, tl, 410, newObject)
	expectStrings(t, "new identity at birth", view.Paths,
		[]string{`\new.txt`})

	view = mustPaths(t, tl, 430, newObject)
	expectStrings(t, "new identity after rename", view.Paths,
		[]string{`\moved.txt`})

	// And the old identity never sees the new object's links.
	view = mustPaths(t, tl, 430, oldObject)
	if len(view.Paths) != 0 {
		t.Fatalf("old identity gained new object's paths: %v", view.Paths)
	}
	expectReasons(t, "old identity at end",
		view.Unresolved, UnresolvedOrphan)

	// Prove the high sequence bit survived the raw V2 round trip: the
	// parsed record's identity must equal the 0x8003 identity.
	parsed := parseSpecs(t, []usnSpec{
		{440, newObject, root, usnReasonDataOverwrite, 0, "moved.txt"},
	})
	got := extractRecordInput(parsed[0]).object
	if got != newObject {
		t.Fatalf("sequence high bit lost: got %v, want %v", got, newObject)
	}

	// Querying a USN before the new object existed is an error.
	if _, err := tl.PathsAt(390, newObject); err == nil {
		t.Fatal("expected error querying reused object before its creation")
	}
}

// TestJournalTimelineUnknown covers missing parents, cycles formed by
// later records, orphan objects, and initial graph validation failures.
func TestJournalTimelineUnknown(t *testing.T) {
	root := JournalIdentity{RecordNumber: 5, Sequence: 1}
	missingParent := JournalIdentity{RecordNumber: 99, Sequence: 7}
	objX := JournalIdentity{RecordNumber: 80, Sequence: 1}
	dirC1 := JournalIdentity{RecordNumber: 81, Sequence: 1}
	dirC2 := JournalIdentity{RecordNumber: 82, Sequence: 1}
	fileY := JournalIdentity{RecordNumber: 83, Sequence: 1}
	objZ := JournalIdentity{RecordNumber: 84, Sequence: 1}

	initial := []JournalObject{
		makeObject(5, 1, true),
		makeObject(81, 1, true, linkTo(5, 1, "C1")),
		makeObject(82, 1, true, linkTo(81, 1, "C2")),
		makeObject(83, 1, false, linkTo(82, 1, "y.txt")),
		makeObject(84, 1, false, linkTo(5, 1, "z.txt")),
	}
	tl, err := NewJournalTimeline(root, initial)
	if err != nil {
		t.Fatalf("NewJournalTimeline: %v", err)
	}

	applySpecs(t, tl, []usnSpec{
		// X is created under a parent the timeline has never seen.
		{500, objX, missingParent, usnReasonFileCreate, 0, "x.txt"},
		// Move C1 under C2, whose own parent is C1: this closes a
		// directory cycle once the NEW lands.
		{510, dirC1, root, usnReasonRenameOldName,
			fileAttributeDirectory, "C1"},
		{520, dirC1, dirC2, usnReasonRenameNewName,
			fileAttributeDirectory, "C1"},
		// Z loses its only link and becomes an orphan.
		{530, objZ, root, usnReasonFileDelete, 0, "z.txt"},
	})

	// Before the cycle exists, the chain resolves fully.
	view := mustPaths(t, tl, 500, fileY)
	expectStrings(t, "y before cycle", view.Paths,
		[]string{`\C1\C2\y.txt`})

	// Missing parent: only the known chain segment is reported; no
	// intermediate directory is skipped and nothing is invented.
	view = mustPaths(t, tl, 500, objX)
	if len(view.Paths) != 0 {
		t.Fatalf("missing parent produced paths %v", view.Paths)
	}
	expectReasons(t, "x missing parent",
		view.Unresolved, UnresolvedMissingParent)
	u := view.Unresolved[0]
	if u.Parent != missingParent {
		t.Fatalf("missing parent identity = %v, want %v",
			u.Parent, missingParent)
	}
	expectStrings(t, "x segment", u.Segment, []string{"x.txt"})

	// After the cycle closes: the segment up to the repeat is returned
	// with a cycle reason.
	view = mustPaths(t, tl, 520, fileY)
	if len(view.Paths) != 0 {
		t.Fatalf("cyclic chain produced paths %v", view.Paths)
	}
	expectReasons(t, "y after cycle", view.Unresolved, UnresolvedCycle)
	if view.Unresolved[0].Parent != dirC2 {
		t.Fatalf("cycle parent = %v, want %v",
			view.Unresolved[0].Parent, dirC2)
	}
	expectStrings(t, "y cycle segment", view.Unresolved[0].Segment,
		[]string{"y.txt", "C2", "C1"})

	view = mustPaths(t, tl, 520, dirC2)
	expectReasons(t, "C2 after cycle", view.Unresolved, UnresolvedCycle)

	// Orphan after its only link is deleted.
	view = mustPaths(t, tl, 530, objZ)
	expectReasons(t, "z orphan", view.Unresolved, UnresolvedOrphan)

	// Unresolved segments are independent copies.
	view.Unresolved[0].Segment = append(view.Unresolved[0].Segment, "hack")
	again := mustPaths(t, tl, 530, objZ)
	if len(again.Unresolved[0].Segment) != 0 {
		t.Fatal("PathsAt borrowed its Unresolved segment storage")
	}

	// Initial graph validation.
	t.Run("duplicate_identity", func(t *testing.T) {
		_, err := NewJournalTimeline(root, []JournalObject{
			makeObject(5, 1, true),
			makeObject(70, 1, false, linkTo(5, 1, "a.txt")),
			makeObject(70, 1, false, linkTo(5, 1, "b.txt")),
		})
		if err == nil || !strings.Contains(err.Error(), "duplicate object") {
			t.Fatalf("want duplicate identity error, got %v", err)
		}
	})

	t.Run("duplicate_link", func(t *testing.T) {
		_, err := NewJournalTimeline(root, []JournalObject{
			makeObject(5, 1, true),
			makeObject(71, 1, false,
				linkTo(5, 1, "a.txt"), linkTo(5, 1, "a.txt")),
		})
		if err == nil || !strings.Contains(err.Error(), "duplicate link") {
			t.Fatalf("want duplicate link error, got %v", err)
		}
	})

	t.Run("initial_cycle", func(t *testing.T) {
		_, err := NewJournalTimeline(root, []JournalObject{
			makeObject(5, 1, true),
			makeObject(72, 1, true, linkTo(73, 1, "d1")),
			makeObject(73, 1, true, linkTo(72, 1, "d2")),
		})
		if err == nil || !strings.Contains(err.Error(), "cycle") {
			t.Fatalf("want cycle error, got %v", err)
		}
	})

	t.Run("directory_two_parents", func(t *testing.T) {
		_, err := NewJournalTimeline(root, []JournalObject{
			makeObject(5, 1, true),
			makeObject(74, 1, true,
				linkTo(5, 1, "d1"), linkTo(5, 1, "d2")),
		})
		if err == nil || !strings.Contains(err.Error(), "exactly one parent") {
			t.Fatalf("want single parent error, got %v", err)
		}
	})

	t.Run("link_under_file", func(t *testing.T) {
		_, err := NewJournalTimeline(root, []JournalObject{
			makeObject(5, 1, true),
			makeObject(75, 1, false, linkTo(5, 1, "f.txt")),
			makeObject(76, 1, false, linkTo(75, 1, "child.txt")),
		})
		if err == nil || !strings.Contains(err.Error(), "not a directory") {
			t.Fatalf("want non directory parent error, got %v", err)
		}
	})

	t.Run("root_must_be_directory", func(t *testing.T) {
		badRoot := JournalIdentity{RecordNumber: 5, Sequence: 1}
		_, err := NewJournalTimeline(badRoot, []JournalObject{
			makeObject(5, 1, false),
		})
		if err == nil || !strings.Contains(err.Error(), "root") {
			t.Fatalf("want root type error, got %v", err)
		}
	})
}

// ExampleJournalTimeline shows the time point assumption behind the
// initial graph and what the timeline reports when the journal is
// incomplete. The records travel through the real ParseUSN pipeline.
func ExampleJournalTimeline() {
	root := JournalIdentity{RecordNumber: 5, Sequence: 1}
	work := JournalIdentity{RecordNumber: 60, Sequence: 1}
	notes := JournalIdentity{RecordNumber: 61, Sequence: 1}
	unknown := JournalIdentity{RecordNumber: 99, Sequence: 1}

	// The initial graph is the directory state at one chosen instant:
	// everything below is assumed true as of that point, and the journal
	// only explains changes after it.
	tl, err := NewJournalTimeline(root, []JournalObject{
		makeObject(5, 1, true),
		makeObject(60, 1, true, linkTo(5, 1, "work")),
		makeObject(61, 1, false, linkTo(60, 1, "notes.txt")),
	})
	if err != nil {
		panic(err)
	}

	records := parseSpecs(nil, []usnSpec{
		// Rename inside the known graph.
		{10, notes, work, usnReasonRenameOldName, 0, "notes.txt"},
		{20, notes, work, usnReasonRenameNewName, 0, "notes-old.txt"},
		// A create under a parent the snapshot never mentioned and no
		// later record introduces: the journal window is incomplete.
		{30, JournalIdentity{RecordNumber: 62, Sequence: 1}, unknown,
			usnReasonFileCreate, 0, "orphan.txt"},
	})
	for _, record := range records {
		if err := tl.ApplyRecord(record); err != nil {
			panic(err)
		}
	}

	// The OLD half alone does not move the file.
	before, _ := tl.PathsAt(10, notes)
	for _, p := range before.Paths {
		fmt.Println(p)
	}

	after, _ := tl.PathsAt(20, notes)
	for _, p := range after.Paths {
		fmt.Println(p)
	}

	// The incomplete journal can only offer the known segment and a
	// structured reason; it never fills the gap from the current MFT.
	broken, _ := tl.PathsAt(30, JournalIdentity{RecordNumber: 62, Sequence: 1})
	for _, u := range broken.Unresolved {
		fmt.Printf("unresolved: %s -> %s\n", u.Reason, u.Segment[0])
	}
	// Output:
	// \work\notes.txt
	// \work\notes-old.txt
	// unresolved: missing_parent -> orphan.txt
}
