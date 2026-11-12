/*
JournalTimeline reconstructs historical file paths from a USN change
journal.

USN_RECORD.Links() and USN_RECORD.FullPath() resolve a record's parent
through the *current* MFT, so they only describe the filesystem as it is
now. The journal itself is a history: paired RENAME_OLD_NAME /
RENAME_NEW_NAME records, hard link creates and deletes, and MFT record
reuse events. JournalTimeline replays that history on top of a caller
provided snapshot of a directory state and answers "where was this
object when USN X had just been processed?"

The timeline is deliberately self contained: it never touches the host
filesystem or the live MFT. Every name and parent relationship comes
from the initial snapshot or from a journal record, so a missing
ancestor stays missing instead of being filled in with a present day
name.

Each structural change appends an immutable version to the affected
object. A path query at USN X walks the versions that were effective at
X, which is why the same descendant resolves correctly both before and
after an ancestor directory move, without synthesising rename records
for the descendants.
*/
package parser

import (
	"errors"
	"fmt"
	"sort"
	"strings"
)

// USN reason bits, as defined for USN_RECORD_V2.Reason.
const (
	usnReasonDataOverwrite  uint32 = 1 << 0
	usnReasonFileCreate     uint32 = 1 << 8
	usnReasonFileDelete     uint32 = 1 << 9
	usnReasonRenameOldName  uint32 = 1 << 12
	usnReasonRenameNewName  uint32 = 1 << 13
	usnReasonHardLinkChange uint32 = 1 << 16

	usnReasonRenameMask = usnReasonRenameOldName | usnReasonRenameNewName
	usnReasonStructMask = usnReasonFileCreate | usnReasonFileDelete |
		usnReasonRenameOldName | usnReasonRenameNewName | usnReasonHardLinkChange
)

// FILE_ATTRIBUTE_DIRECTORY in USN_RECORD_V2.FileAttributes.
const fileAttributeDirectory uint32 = 1 << 4

// JournalIdentity identifies an object by its full 64 bit file
// reference number: the 48 bit MFT record number and the 16 bit
// sequence number, including its highest bit. Two objects that reuse
// the same record number but carry different sequence numbers are
// unrelated identities.
type JournalIdentity struct {
	RecordNumber uint64 // low 48 bits of the file reference number
	Sequence     uint16 // full 16 bit sequence number, highest bit kept
}

// String renders an identity as record-number/sequence.
func (self JournalIdentity) String() string {
	return fmt.Sprintf("%d/%d", self.RecordNumber, self.Sequence)
}

// JournalLink is one known directory entry for an object: the parent
// object's identity and the name this object is stored under there.
// Names keep the exact UTF-16 decoded spelling (case included); the
// timeline never folds case.
type JournalLink struct {
	Parent JournalIdentity
	Name   string
}

// JournalObject declares an object in the initial directory state.
type JournalObject struct {
	Identity    JournalIdentity
	IsDirectory bool

	// Links are the object's directory entries at the snapshot point.
	// A file may carry several hard links; a directory has exactly one
	// parent link (the root has none).
	Links []JournalLink
}

// UnresolvedReason explains why a path could not be reconstructed all
// the way to the root.
type UnresolvedReason string

const (
	// UnresolvedMissingParent means a parent identity has no object in
	// the timeline as of the queried USN (it was neither part of the
	// initial state nor ever created by the journal records processed so
	// far).
	UnresolvedMissingParent UnresolvedReason = "missing_parent"

	// UnresolvedCycle means following parent links revisited an object
	// already on the path, so the directory graph has a cycle.
	UnresolvedCycle UnresolvedReason = "cycle"

	// UnresolvedOrphan means the object itself has no directory entry at
	// this point in time (all links were deleted, or a rename has not
	// been completed yet).
	UnresolvedOrphan UnresolvedReason = "orphan"

	// UnresolvedBrokenLink means a link names a parent that is a file,
	// not a directory, so it cannot hold children.
	UnresolvedBrokenLink UnresolvedReason = "broken_link"
)

// JournalUnresolved is one unresolved path segment. Segment holds the
// components that could be reconstructed, from the queried object up to
// the point resolution stopped, in child-to-parent order.
type JournalUnresolved struct {
	Reason   UnresolvedReason
	Object   JournalIdentity
	Parent   JournalIdentity
	Segment  []string
	Evidence string
}

// JournalPathView is the answer to a point in time path query. Paths
// holds every fully reconstructed path (backslash separated, rooted at
// "\"); Unresolved holds the chain segments that could not reach the
// root, with structured reasons. The slices are owned by the caller and
// may be modified freely.
type JournalPathView struct {
	Paths      []string
	Unresolved []JournalUnresolved
}

// objectVersion is an immutable link-set state effective from USN
// (inclusive) until the next version. The version at index 0 carries the
// initial snapshot and uses the USN of the first applicable record.
type objectVersion struct {
	usn         uint64
	isDirectory bool
	links       []JournalLink
}

// objectHistory is the full version chain of one object identity.
type objectHistory struct {
	versions []objectVersion
}

func (self *objectHistory) current() objectVersion {
	return self.versions[len(self.versions)-1]
}

// versionAt returns the version effective immediately after usn was
// processed. It fails when the object did not exist yet at that USN.
func (self *objectHistory) versionAt(usn uint64) (objectVersion, bool) {
	effective := -1
	for i, version := range self.versions {
		if version.usn <= usn {
			effective = i
		} else {
			break
		}
	}
	if effective < 0 {
		return objectVersion{}, false
	}
	return self.versions[effective], true
}

// pendingRename is the evidence left by an unpaired RENAME_OLD_NAME.
type pendingRename struct {
	usn       uint64
	oldParent JournalIdentity
	oldName   string
}

// JournalTimeline replays USN_RECORD_V2 records over a fixed starting
// directory state and answers historical path queries. A JournalTimeline
// is not safe for concurrent use.
type JournalTimeline struct {
	root JournalIdentity

	// history keyed by full object identity.
	history map[JournalIdentity]*objectHistory

	// lastUSN enforces strictly increasing journal order. haveLastUSN
	// distinguishes "nothing applied" from a legal first USN of zero.
	lastUSN      uint64
	haveLastUSN  bool
	appliedCount uint64

	// Pending, unpaired RENAME_OLD_NAME evidence keyed by object
	// identity. Pairing is per object: records for other objects may
	// interleave between OLD and NEW.
	pending map[JournalIdentity]*pendingRename

	// Diagnostics that do not mutate the graph: orphan rename halves,
	// double OLD records, deletes of unknown links, contradictory
	// directory link changes.
	diagnostics []JournalUnresolved
}

// NewJournalTimeline builds a timeline from the directory state as of a
// known point in time. root is the identity of the volume root. objects
// describes every object and its links at that instant. The snapshot is
// copied, so later edits by the caller do not affect the timeline.
//
// Initialization rejects duplicate object identities, duplicate links,
// links into unknown parents, non directory parents, and directory
// cycles. Nothing here reads the host filesystem.
func NewJournalTimeline(root JournalIdentity,
	objects []JournalObject) (*JournalTimeline, error) {

	self := &JournalTimeline{
		root:    root,
		history: make(map[JournalIdentity]*objectHistory),
		pending: make(map[JournalIdentity]*pendingRename),
	}

	initial := make(map[JournalIdentity]*journalEntry)
	for _, obj := range objects {
		if _, pres := initial[obj.Identity]; pres {
			return nil, fmt.Errorf("duplicate object identity %v in initial state",
				obj.Identity)
		}

		entry := &journalEntry{
			identity:    obj.Identity,
			isDirectory: obj.IsDirectory,
			links:       make([]JournalLink, 0, len(obj.Links)),
		}

		seenLinks := make(map[JournalLink]bool, len(obj.Links))
		for _, link := range obj.Links {
			if seenLinks[link] {
				return nil, fmt.Errorf(
					"duplicate link %q under %v for object %v in initial state",
					link.Name, link.Parent, obj.Identity)
			}
			seenLinks[link] = true
			entry.links = append(entry.links, JournalLink{
				Parent: link.Parent,
				Name:   strings.Clone(link.Name),
			})
		}
		initial[obj.Identity] = entry
	}

	if rootEntry, pres := initial[root]; pres {
		if !rootEntry.isDirectory {
			return nil, fmt.Errorf("root identity %v is declared as a file", root)
		}
		if len(rootEntry.links) != 0 {
			return nil, fmt.Errorf("root object %v must not declare a parent link", root)
		}
	} else {
		// The root need not be listed explicitly; register it as a
		// linkless directory so children can attach to it.
		initial[root] = &journalEntry{identity: root, isDirectory: true}
	}

	for id, entry := range initial {
		if entry.isDirectory && id != root && len(entry.links) != 1 {
			return nil, fmt.Errorf(
				"directory %v must have exactly one parent link, has %d",
				id, len(entry.links))
		}
		for _, link := range entry.links {
			parent, pres := initial[link.Parent]
			if !pres {
				return nil, fmt.Errorf(
					"object %v links under unknown parent %v", id, link.Parent)
			}
			if !parent.isDirectory {
				return nil, fmt.Errorf(
					"object %v links under %v which is not a directory",
					id, link.Parent)
			}
		}
	}

	// Reject cycles among the directory parent links.
	for id := range initial {
		if err := checkInitialCycle(root, id, initial); err != nil {
			return nil, err
		}
	}

	// Freeze the validated snapshot as version 0 of every object.
	for id, entry := range initial {
		links := make([]JournalLink, len(entry.links))
		copy(links, entry.links)
		self.history[id] = &objectHistory{
			versions: []objectVersion{{
				isDirectory: entry.isDirectory,
				links:       links,
			}},
		}
	}

	return self, nil
}

// journalEntry is a mutable scratch type used while building the
// initial graph.
type journalEntry struct {
	identity    JournalIdentity
	isDirectory bool
	links       []JournalLink
}

func checkInitialCycle(root, start JournalIdentity,
	initial map[JournalIdentity]*journalEntry) error {

	visited := make(map[JournalIdentity]bool)
	current := start
	for {
		if current == root {
			return nil
		}
		if visited[current] {
			return fmt.Errorf("directory cycle in initial state at %v", current)
		}
		visited[current] = true
		entry, pres := initial[current]
		if !pres || len(entry.links) == 0 {
			return nil
		}
		current = entry.links[0].Parent
	}
}

// recordInput is the part of a USN_RECORD_V2 the timeline depends on,
// extracted without touching the underlying reader more than once.
type recordInput struct {
	usn        uint64
	reason     uint32
	attributes uint32
	object     JournalIdentity
	parent     JournalIdentity
	name       string
}

// extractRecordInput reads the fields the timeline needs from a parsed
// V2 record.
func extractRecordInput(record *USN_RECORD) recordInput {
	v2 := record.USN_RECORD_V2

	// The generated sequence accessors mask the highest sequence bit
	// off. The timeline promises to keep all 16 sequence bits, so decode
	// the raw 64 bit reference numbers directly.
	fileRef := ParseUint64(v2.Reader,
		v2.Profile.Off_USN_RECORD_V2_FileReferenceNumberSequence+v2.Offset)
	parentRef := ParseUint64(v2.Reader,
		v2.Profile.Off_USN_RECORD_V2_ParentFileReferenceNumberSequence+v2.Offset)

	return recordInput{
		usn:        v2.Usn(),
		reason:     uint32(v2.Reason().Value),
		attributes: uint32(v2.FileAttributes().Value),
		object: JournalIdentity{
			RecordNumber: fileRef & 0xffffffffffff,
			Sequence:     uint16(fileRef >> 48),
		},
		parent: JournalIdentity{
			RecordNumber: parentRef & 0xffffffffffff,
			Sequence:     uint16(parentRef >> 48),
		},
		name: record.Filename(),
	}
}

// ApplyRecord appends one record, typically obtained from ParseUSN.
//
// USNs must be strictly increasing; a repeated or backwards USN is an
// error and leaves the timeline state untouched. The timestamp is kept
// only as evidence and never participates in ordering.
func (self *JournalTimeline) ApplyRecord(record *USN_RECORD) error {
	if record == nil {
		return errors.New("nil USN record")
	}
	return self.apply(extractRecordInput(record))
}

// ApplyFromChannel consumes every record emitted by a ParseUSN channel
// until it closes, applying each in order. The first record that fails
// to apply is returned; the channel is fully drained and the timeline
// retains every record applied before the failure.
func (self *JournalTimeline) ApplyFromChannel(records chan *USN_RECORD) error {
	var firstErr error
	for record := range records {
		if firstErr == nil {
			if err := self.ApplyRecord(record); err != nil {
				firstErr = err
			}
		}
	}
	return firstErr
}

func (self *JournalTimeline) apply(in recordInput) error {
	if self.haveLastUSN && in.usn <= self.lastUSN {
		return fmt.Errorf("USN %#x is not greater than last applied USN %#x",
			in.usn, self.lastUSN)
	}

	if err := self.validateReasons(in); err != nil {
		return err
	}

	// Record number reuse: the same 48 bit record number with a new
	// sequence is a brand new object. It inherits no links from whatever
	// occupied the record number before.
	hist, known := self.history[in.object]
	if !known {
		hist = &objectHistory{
			versions: []objectVersion{{
				usn:         in.usn,
				isDirectory: in.attributes&fileAttributeDirectory != 0,
				links:       []JournalLink{},
			}},
		}
		self.history[in.object] = hist
	}

	if in.reason&usnReasonStructMask != 0 {
		self.applyStructural(in, hist)
	}

	self.lastUSN = in.usn
	self.haveLastUSN = true
	self.appliedCount++
	return nil
}

// validateReasons rejects reason combinations that demand contradictory
// structural changes. The timeline never picks one bit and silently
// ignores the other.
func (self *JournalTimeline) validateReasons(in recordInput) error {
	reason := in.reason

	if reason&usnReasonRenameMask == usnReasonRenameMask {
		return fmt.Errorf(
			"USN %#x on %v carries both RENAME_OLD_NAME and RENAME_NEW_NAME",
			in.usn, in.object)
	}
	if reason&usnReasonFileCreate != 0 && reason&usnReasonFileDelete != 0 {
		return fmt.Errorf(
			"USN %#x on %v carries both FILE_CREATE and FILE_DELETE",
			in.usn, in.object)
	}
	if reason&usnReasonFileCreate != 0 && reason&usnReasonRenameMask != 0 {
		return fmt.Errorf(
			"USN %#x on %v mixes FILE_CREATE with a rename reason",
			in.usn, in.object)
	}
	if reason&usnReasonFileDelete != 0 && reason&usnReasonRenameMask != 0 {
		return fmt.Errorf(
			"USN %#x on %v mixes FILE_DELETE with a rename reason",
			in.usn, in.object)
	}
	return nil
}

// applyStructural handles every reason bit that can change the link
// graph. All mutations for one record are folded into a single new
// object version.
func (self *JournalTimeline) applyStructural(in recordInput,
	hist *objectHistory) {

	current := hist.current()
	working := make([]JournalLink, len(current.links))
	copy(working, current.links)
	mutated := false

	link := JournalLink{Parent: in.parent, Name: in.name}

	if in.reason&usnReasonFileCreate != 0 {
		if indexOfLink(working, link) < 0 {
			if current.isDirectory && len(working) >= 1 && in.object != self.root {
				// A directory cannot gain a second parent. Surface the
				// contradiction instead of corrupting the graph.
				self.diagnostics = append(self.diagnostics, JournalUnresolved{
					Reason:   UnresolvedBrokenLink,
					Object:   in.object,
					Parent:   link.Parent,
					Segment:  []string{link.Name},
					Evidence: fmt.Sprintf("USN %#x: second parent link for directory", in.usn),
				})
			} else {
				// Re-adding an identical link (indexOfLink >= 0) is
				// idempotent evidence, e.g. overlapping journal windows,
				// and simply changes nothing.
				working = append(working, link)
				mutated = true
			}
		}
	}

	if in.reason&usnReasonFileDelete != 0 {
		if idx := indexOfLink(working, link); idx >= 0 {
			working = append(working[:idx], working[idx+1:]...)
			mutated = true
		} else {
			// Deleting a link the timeline never knew about: keep the
			// evidence, invent nothing.
			self.diagnostics = append(self.diagnostics, JournalUnresolved{
				Reason:   UnresolvedOrphan,
				Object:   in.object,
				Parent:   link.Parent,
				Segment:  []string{link.Name},
				Evidence: fmt.Sprintf("USN %#x: delete of unknown link", in.usn),
			})
		}
	}

	if in.reason&usnReasonRenameOldName != 0 {
		self.recordRenameOld(in)
	}

	if in.reason&usnReasonRenameNewName != 0 {
		if newLinks, ok := self.recordRenameNew(in, working, link); ok {
			working = newLinks
			mutated = true
		}
	}

	// HARD_LINK_CHANGE alone carries no link payload: the accompanying
	// FILE_CREATE / FILE_DELETE record describes the link.

	if mutated {
		hist.versions = append(hist.versions, objectVersion{
			usn:         in.usn,
			isDirectory: current.isDirectory,
			links:       working,
		})
	}
}

func indexOfLink(links []JournalLink, want JournalLink) int {
	for i, link := range links {
		if link == want {
			return i
		}
	}
	return -1
}

// recordRenameOld stores the OLD half of a rename, keyed by object
// identity so other objects' records can interleave before the NEW.
func (self *JournalTimeline) recordRenameOld(in recordInput) {
	if pending, pres := self.pending[in.object]; pres {
		// Two OLD records in a row for the same object with no NEW: keep
		// the first one as the unpaired diagnostic and refuse to guess
		// which old name an eventual NEW belongs to.
		self.diagnostics = append(self.diagnostics, JournalUnresolved{
			Reason: UnresolvedOrphan,
			Object: in.object,
			Parent: in.parent,
			Segment: []string{
				in.name,
			},
			Evidence: fmt.Sprintf(
				"USN %#x: second RENAME_OLD_NAME while USN %#x is unpaired",
				in.usn, pending.usn),
		})
		return
	}

	self.pending[in.object] = &pendingRename{
		usn:       in.usn,
		oldParent: in.parent,
		oldName:   in.name,
	}
}

// recordRenameNew completes a rename when a matching OLD exists. It
// returns the replacement link set; a false result means the NEW is
// orphan evidence and must not move anything.
func (self *JournalTimeline) recordRenameNew(in recordInput,
	working []JournalLink, newLink JournalLink) ([]JournalLink, bool) {

	pending, pres := self.pending[in.object]
	if !pres {
		// Orphan NEW: no old link to replace. Never move on a NEW alone.
		self.diagnostics = append(self.diagnostics, JournalUnresolved{
			Reason:  UnresolvedOrphan,
			Object:  in.object,
			Parent:  newLink.Parent,
			Segment: []string{newLink.Name},
			Evidence: fmt.Sprintf(
				"USN %#x: RENAME_NEW_NAME without a preceding RENAME_OLD_NAME",
				in.usn),
		})
		return working, false
	}

	oldLink := JournalLink{Parent: pending.oldParent, Name: pending.oldName}
	idx := indexOfLink(working, oldLink)
	if idx < 0 {
		// The OLD half names a link the object no longer has. Keep both
		// halves as evidence and leave the graph alone.
		self.diagnostics = append(self.diagnostics, JournalUnresolved{
			Reason:  UnresolvedOrphan,
			Object:  in.object,
			Parent:  oldLink.Parent,
			Segment: []string{oldLink.Name},
			Evidence: fmt.Sprintf(
				"USN %#x: rename OLD names a link the object no longer has",
				pending.usn),
		})
		delete(self.pending, in.object)
		return working, false
	}

	working[idx] = newLink
	delete(self.pending, in.object)
	return working, true
}

// AppliedCount reports how many records have been applied successfully.
func (self *JournalTimeline) AppliedCount() uint64 {
	return self.appliedCount
}

// LastUSN reports the most recently applied USN and whether any record
// has been applied at all.
func (self *JournalTimeline) LastUSN() (uint64, bool) {
	return self.lastUSN, self.haveLastUSN
}

// Diagnostics returns a copy of the structural anomalies observed so
// far: unpaired rename halves, double OLD records, deletes of unknown
// links, and contradictory directory link changes.
func (self *JournalTimeline) Diagnostics() []JournalUnresolved {
	result := make([]JournalUnresolved, len(self.diagnostics))
	copy(result, self.diagnostics)
	for i := range result {
		result[i].Segment = append([]string(nil), result[i].Segment...)
	}
	return result
}

// PathsAt returns the path view for object as it stood immediately after
// the record carrying usn was applied. Querying a USN that has not been
// applied, or an object that did not exist yet at that USN, is an error.
// Descendants resolve against ancestor versions effective at usn, so
// queries before and after an ancestor directory move both work.
func (self *JournalTimeline) PathsAt(usn uint64,
	object JournalIdentity) (*JournalPathView, error) {

	if !self.haveLastUSN {
		return nil, errors.New("no USN records have been applied yet")
	}
	if usn > self.lastUSN {
		return nil, fmt.Errorf(
			"USN %#x has not been applied (last is %#x)", usn, self.lastUSN)
	}

	hist, pres := self.history[object]
	if !pres {
		return nil, fmt.Errorf("object %v is not known to the timeline", object)
	}
	version, exists := hist.versionAt(usn)
	if !exists {
		return nil, fmt.Errorf("object %v did not exist yet at USN %#x", object, usn)
	}

	view := &JournalPathView{
		Paths:      []string{},
		Unresolved: []JournalUnresolved{},
	}

	if object == self.root {
		view.Paths = append(view.Paths, `\`)
		return view, nil
	}

	if len(version.links) == 0 {
		view.Unresolved = append(view.Unresolved, JournalUnresolved{
			Reason: UnresolvedOrphan,
			Object: object,
		})
		return view, nil
	}

	// Resolve each link independently so a file with several hard links
	// reports one path per link, and a broken branch never hides the
	// working ones.
	for _, link := range version.links {
		segment, reason, parent, ok := self.resolveChain(object, link, usn)
		if ok {
			// segment runs child-to-parent; reverse into root-first
			// order and prefix the root separator.
			components := append([]string(nil), segment...)
			ReverseStringSlice(components)
			view.Paths = append(view.Paths,
				`\`+strings.Join(components, `\`))
		} else {
			view.Unresolved = append(view.Unresolved, JournalUnresolved{
				Reason:  reason,
				Object:  object,
				Parent:  parent,
				Segment: segment,
			})
		}
	}

	sort.Strings(view.Paths)
	return view, nil
}

// resolveChain follows parent links from the link on object up toward
// the root, using only object versions effective at usn. It returns the
// known segment in child-to-parent order, a structured reason if the
// chain cannot reach the root, and the parent identity where resolution
// stopped. No intermediate directory is ever skipped.
func (self *JournalTimeline) resolveChain(object JournalIdentity,
	firstLink JournalLink, usn uint64) (
	[]string, UnresolvedReason, JournalIdentity, bool) {

	segment := []string{firstLink.Name}
	visited := map[JournalIdentity]bool{object: true}
	parentID := firstLink.Parent

	for {
		if parentID == self.root {
			return segment, "", JournalIdentity{}, true
		}

		if visited[parentID] {
			return segment, UnresolvedCycle, parentID, false
		}
		visited[parentID] = true

		hist, pres := self.history[parentID]
		if !pres {
			return segment, UnresolvedMissingParent, parentID, false
		}
		version, exists := hist.versionAt(usn)
		if !exists {
			return segment, UnresolvedMissingParent, parentID, false
		}
		if !version.isDirectory {
			return segment, UnresolvedBrokenLink, parentID, false
		}
		if len(version.links) == 0 {
			return segment, UnresolvedOrphan, parentID, false
		}

		// Directories carry exactly one parent link; anything else here
		// is not a valid directory ancestor.
		parentLink := version.links[0]
		segment = append(segment, parentLink.Name)
		parentID = parentLink.Parent
	}
}
