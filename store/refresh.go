package store

import (
	"errors"
	"os"
	"path/filepath"
)

// ErrRefreshOpen and ErrNoRefresh report a refresh used out of order.
var (
	ErrRefreshOpen = errors.New("store: a full refresh is already in progress")
	ErrNoRefresh   = errors.New("store: no full refresh is in progress")
)

// A full refresh replaces everything a group holds with the result of one
// complete pass over the endpoint. It exists for rebuilding the local view of
// an endpoint whose history changed - a repository reindexed, a migration
// upstream, records removed that an incremental harvest never hears about -
// without deleting the shard and losing the old view before the new one
// exists.
//
// The unit of replacement is the group, the same unit a Writer writes: the
// new pass is the group's whole new content, and ranges the old view covered
// outside the refreshed span are not kept. Other formats and sets of the same
// endpoint are other groups and are not touched.
//
// While a refresh runs, its windows commit into a staging index beside the
// live one, and its bytes append to the same segments past the length the
// live index vouches for. Nothing that reads - Records, Stat, another
// process, this writer's own Resume and HasWindow - sees any of it: the
// staging index is a file no reader opens, and the appended bytes are named
// by no extent in the live index. A reader that started before the switch
// reads the old view to its end, since a committed refresh never shortens a
// segment. CommitRefresh renames the staging index over the live one, and
// that one rename is the switch: the new windows, their record counts and
// their coverage become visible together, and the superseded bytes stay in
// the segments as the unreachable runs a supersede always leaves behind.
//
// A refresh that does not complete publishes nothing. AbortRefresh cuts the
// segments back to what the live index vouches for and drops the staging
// index, and Close aborts a refresh still in progress, so the old view
// survives any failed attempt and the next one starts from it.

// stagingName is the file a refresh builds its index in, beside the live one.
// It is deliberately not stateName: every reader opens stateName and nothing
// else, which is what keeps a half-built refresh invisible.
const stagingName = stateName + ".refresh"

// BeginRefresh starts a full refresh of this writer's group. Every window
// committed from here to CommitRefresh belongs to the refresh and is
// invisible to readers; Resume and HasWindow keep answering for the view that
// is being replaced, so a harvest that wants the old coverage taken into
// account should not be refreshing.
//
// The staging index starts empty but over the same segments: the refresh's
// bytes append where the live index's committed lengths end, and its extents
// are offsets into the same files.
func (w *Writer) BeginRefresh() error {
	if w.refresh != nil {
		return ErrRefreshOpen
	}
	if w.win != nil {
		return ErrWindowOpen
	}
	st := &state{
		path:    filepath.Join(groupDir(w.shard, w.group.Format, w.group.Set), stagingName),
		Version: stateVersion,
		Format:  w.group.Format,
		Set:     w.group.Set,
	}
	for _, seg := range w.st.Segments {
		st.Segments = append(st.Segments, &segRow{Num: seg.Num, Committed: seg.Committed})
	}
	w.refresh = st
	return nil
}

// Refreshing reports whether a full refresh is in progress.
func (w *Writer) Refreshing() bool { return w.refresh != nil }

// CommitRefresh makes the refreshed windows the group's content: the staging
// index is renamed over the live one, and new data, record counts and
// coverage become visible in that one rename. What the old index covered is
// gone, ranges outside the refreshed span included; the bytes it named stay
// in the segments, unreachable, as superseded bytes always do.
func (w *Writer) CommitRefresh() error {
	st := w.refresh
	if st == nil {
		return ErrNoRefresh
	}
	if w.win != nil {
		return ErrWindowOpen
	}
	// The last window's commit flushed and synced already, but a refresh
	// committed through Begin and Commit is not the only way here - a caller
	// can stage nothing at all, which is a refresh to an empty group and
	// still a thing worth switching to.
	if w.seg != nil {
		if err := w.seg.flush(); err != nil {
			return err
		}
		if err := w.seg.sync(); err != nil {
			return err
		}
	}
	// The shard announces itself before the switch, as it does for any first
	// commit: a group the meta does not list reads as never harvested, and
	// the moment after the rename is not the moment to learn that.
	if err := w.announce(); err != nil {
		return err
	}
	if err := st.save(); err != nil {
		return err
	}
	live := statePath(w.shard, w.group.Format, w.group.Set)
	if err := os.Rename(st.path, live); err != nil {
		return err
	}
	st.path = live
	w.st = st
	w.refresh = nil
	return nil
}

// AbortRefresh drops the refresh in progress: the staging index is removed
// and the segments are cut back to the lengths the live index vouches for,
// which is where the refresh's bytes began. The live view is exactly what it
// was before BeginRefresh, and the writer can go on to another refresh or to
// ordinary incremental windows.
func (w *Writer) AbortRefresh() error {
	st := w.refresh
	if st == nil {
		return ErrNoRefresh
	}
	w.refresh = nil
	w.win = nil
	var errs []error
	// The segment writer is pointed past the live committed lengths and may
	// sit on a segment the live index does not know at all, so it cannot be
	// reused: closing it lets the next append reopen from the live index.
	if w.seg != nil {
		errs = append(errs, w.seg.close())
		w.seg, w.segNum, w.segCommitted = nil, 0, 0
	}
	live := make(map[int]int64)
	for _, seg := range w.st.Segments {
		live[seg.Num] = seg.Committed
	}
	dir := groupDir(w.shard, w.group.Format, w.group.Set)
	for _, seg := range st.Segments {
		committed, ok := live[seg.Num]
		path := filepath.Join(dir, segFileName(seg.Num))
		if !ok {
			// A segment the refresh started holds only its bytes.
			if err := os.Remove(path); err != nil && !errors.Is(err, os.ErrNotExist) {
				errs = append(errs, err)
			}
			continue
		}
		if err := os.Truncate(path, committed); err != nil && !errors.Is(err, os.ErrNotExist) {
			errs = append(errs, err)
		}
	}
	if err := os.Remove(st.path); err != nil && !errors.Is(err, os.ErrNotExist) {
		errs = append(errs, err)
	}
	return errors.Join(errs...)
}
