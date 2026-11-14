package mjpeg

import (
	"bytes"
	"encoding/binary"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"reflect"
	"testing"

	"github.com/icza/mjpeg/task_support/taskcheck"
)

// makeFrame returns a deterministic pseudo-JPEG payload of exactly n bytes
// framed by the SOI/EOI markers.
func makeFrame(n int) []byte {
	b := make([]byte, n)
	for i := range b {
		b[i] = byte(i*131 + n*7)
	}
	b[0], b[1] = 0xFF, 0xD8
	b[n-2], b[n-1] = 0xFF, 0xD9
	return b
}

// oFrame is one 00dc chunk located by the test walker.
type oFrame struct {
	chunkOff int64
	dataOff  int64
	size     int64
}

// oSeg is one RIFF segment of the parsed file.
type oSeg struct {
	typ     string
	start   int64
	total   int64
	moviPos int64
	frames  []oFrame
	ixOff   int64
	ixTotal int64
	ixBase  uint64
	ix      [][2]uint32
	hasIdx1 bool
	idx1    [][4]uint32
}

// oSuper is one super index entry.
type oSuper struct {
	off  uint64
	size uint32
	dur  uint32
}

// oAvi is the parsed AVI file.
type oAvi struct {
	segs      []*oSeg
	avihTotal uint32
	strhLen   uint32
	hasIndx   bool
	indxCount uint32
	supers    []oSuper
	hasDmlh   bool
	dmlhTotal uint32
}

func u32(b []byte, off int64) uint32 { return binary.LittleEndian.Uint32(b[off:]) }
func u64(b []byte, off int64) uint64 { return binary.LittleEndian.Uint64(b[off:]) }

// parseAVI walks the file chunk by chunk, following the length fields.
func parseAVI(b []byte) (*oAvi, error) {
	a := &oAvi{}
	for p := int64(0); p < int64(len(b)); {
		if p+12 > int64(len(b)) {
			return nil, fmt.Errorf("truncated RIFF header at %d", p)
		}
		if string(b[p:p+4]) != "RIFF" {
			return nil, fmt.Errorf("no RIFF at %d, got %q", p, b[p:p+4])
		}
		size := int64(u32(b, p+4))
		end := p + 8 + size
		if end > int64(len(b)) {
			return nil, fmt.Errorf("RIFF at %d overruns file", p)
		}
		seg := &oSeg{typ: string(b[p+8 : p+12]), start: p, total: 8 + size}
		q := p + 12
		for q < end {
			if q+8 > end {
				return nil, fmt.Errorf("truncated chunk at %d", q)
			}
			id := string(b[q : q+4])
			csz := int64(u32(b, q+4))
			cend := q + 8 + csz
			if cend > end {
				return nil, fmt.Errorf("chunk %q at %d overruns RIFF", id, q)
			}
			switch id {
			case "LIST":
				if q+12 > end {
					return nil, fmt.Errorf("truncated LIST at %d", q)
				}
				switch string(b[q+8 : q+12]) {
				case "movi":
					seg.moviPos = q + 8
					if err := parseMovi(seg, b, q+12, cend); err != nil {
						return nil, err
					}
				case "hdrl":
					if err := parseHdrl(a, b, q+12, cend); err != nil {
						return nil, err
					}
				}
			case "idx1":
				if csz%16 != 0 {
					return nil, fmt.Errorf("idx1 size %d not a multiple of 16", csz)
				}
				seg.hasIdx1 = true
				for r := q + 8; r < cend; r += 16 {
					seg.idx1 = append(seg.idx1, [4]uint32{u32(b, r), u32(b, r+4), u32(b, r+8), u32(b, r+12)})
				}
			}
			q = cend + csz&1
		}
		if q != end {
			return nil, fmt.Errorf("RIFF walk stopped at %d, want %d", q, end)
		}
		a.segs = append(a.segs, seg)
		p = end
	}
	return a, nil
}

func parseMovi(seg *oSeg, b []byte, start, end int64) error {
	for q := start; q < end; {
		if q+8 > end {
			return fmt.Errorf("truncated movi chunk at %d", q)
		}
		id := string(b[q : q+4])
		csz := int64(u32(b, q+4))
		cend := q + 8 + csz
		if cend > end {
			return fmt.Errorf("movi chunk %q at %d overruns LIST", id, q)
		}
		switch id {
		case "00dc":
			seg.frames = append(seg.frames, oFrame{chunkOff: q, dataOff: q + 8, size: csz})
		case "ix00":
			if csz < 24 || (csz-24)%8 != 0 {
				return fmt.Errorf("bad ix00 size %d", csz)
			}
			d := q + 8
			if binary.LittleEndian.Uint16(b[d:]) != 2 {
				return fmt.Errorf("ix00 wLongsPerEntry != 2")
			}
			if b[d+2] != 0 || b[d+3] != 1 {
				return fmt.Errorf("ix00 not AVI_INDEX_OF_CHUNKS")
			}
			if string(b[d+8:d+12]) != "00dc" {
				return fmt.Errorf("ix00 chunk id is not 00dc")
			}
			if int64(u32(b, d+4)) != (csz-24)/8 {
				return fmt.Errorf("ix00 nEntriesInUse mismatch")
			}
			seg.ixOff = q
			seg.ixTotal = 8 + csz
			seg.ixBase = u64(b, d+12)
			for r := d + 24; r < cend; r += 8 {
				seg.ix = append(seg.ix, [2]uint32{u32(b, r), u32(b, r+4)})
			}
		}
		q = cend + csz&1
	}
	return nil
}

func parseHdrl(a *oAvi, b []byte, start, end int64) error {
	for q := start; q < end; {
		if q+8 > end {
			return fmt.Errorf("truncated hdrl chunk at %d", q)
		}
		id := string(b[q : q+4])
		csz := int64(u32(b, q+4))
		cend := q + 8 + csz
		if cend > end {
			return fmt.Errorf("hdrl chunk %q at %d overruns LIST", id, q)
		}
		d := q + 8
		switch id {
		case "avih":
			if csz < 20 {
				return fmt.Errorf("short avih")
			}
			a.avihTotal = u32(b, d+16)
		case "LIST":
			if q+12 > end {
				return fmt.Errorf("truncated LIST at %d", q)
			}
			switch string(b[q+8 : q+12]) {
			case "strl":
				if err := parseStrl(a, b, q+12, cend); err != nil {
					return err
				}
			case "odml":
				if err := parseOdml(a, b, q+12, cend); err != nil {
					return err
				}
			}
		}
		q = cend + csz&1
	}
	return nil
}

func parseStrl(a *oAvi, b []byte, start, end int64) error {
	for q := start; q < end; {
		if q+8 > end {
			return fmt.Errorf("truncated strl chunk at %d", q)
		}
		id := string(b[q : q+4])
		csz := int64(u32(b, q+4))
		cend := q + 8 + csz
		if cend > end {
			return fmt.Errorf("strl chunk %q at %d overruns LIST", id, q)
		}
		d := q + 8
		switch id {
		case "strh":
			if csz < 36 {
				return fmt.Errorf("short strh")
			}
			a.strhLen = u32(b, d+32)
		case "indx":
			if csz < 24 || (csz-24)%16 != 0 {
				return fmt.Errorf("bad indx size %d", csz)
			}
			if binary.LittleEndian.Uint16(b[d:]) != 4 {
				return fmt.Errorf("indx wLongsPerEntry != 4")
			}
			if b[d+2] != 0 || b[d+3] != 0 {
				return fmt.Errorf("indx not AVI_INDEX_OF_INDEXES")
			}
			if string(b[d+8:d+12]) != "00dc" {
				return fmt.Errorf("indx chunk id is not 00dc")
			}
			a.hasIndx = true
			a.indxCount = u32(b, d+4)
			for r := d + 24; r < cend; r += 16 {
				a.supers = append(a.supers, oSuper{u64(b, r), u32(b, r+8), u32(b, r+12)})
			}
		}
		q = cend + csz&1
	}
	return nil
}

func parseOdml(a *oAvi, b []byte, start, end int64) error {
	for q := start; q < end; {
		if q+8 > end {
			return fmt.Errorf("truncated odml chunk at %d", q)
		}
		id := string(b[q : q+4])
		csz := int64(u32(b, q+4))
		cend := q + 8 + csz
		if cend > end {
			return fmt.Errorf("odml chunk %q at %d overruns LIST", id, q)
		}
		if id == "dmlh" {
			if csz < 4 {
				return fmt.Errorf("short dmlh")
			}
			a.hasDmlh = true
			a.dmlhTotal = u32(b, q+8)
		}
		q = cend + csz&1
	}
	return nil
}

func segCounts(a *oAvi) []int {
	var out []int
	for _, s := range a.segs {
		out = append(out, len(s.frames))
	}
	return out
}

func segTotals(a *oAvi) []int64 {
	var out []int64
	for _, s := range a.segs {
		out = append(out, s.total)
	}
	return out
}

// verifyOpenDML parses the file and checks the OpenDML structure against
// the expected frames: segment types and sizes, frame bytes reached from
// indx through ix00, the first segment's idx1, and the frame counters.
func verifyOpenDML(c *taskcheck.C, b []byte, frames [][]byte, segLimit int64, maxSeg uint32) *oAvi {
	a, err := parseAVI(b)
	c.Check(err == nil, "parse AVI: %v", err)
	if err != nil {
		return nil
	}
	c.Check(len(a.segs) >= 1, "at least one segment")
	if len(a.segs) == 0 {
		return nil
	}
	c.Check(a.segs[0].typ == "AVI ", "first segment is RIFF AVI, got %q", a.segs[0].typ)
	for i, s := range a.segs[1:] {
		c.Check(s.typ == "AVIX", "segment %d is RIFF AVIX, got %q", i+1, s.typ)
	}
	for i, s := range a.segs {
		c.Check(s.total <= segLimit, "segment %d size %d within limit %d", i, s.total, segLimit)
		c.Check(s.start%2 == 0, "segment %d starts at even offset", i)
		for _, fr := range s.frames {
			c.Check(fr.chunkOff%2 == 0, "segment %d frame chunk at even offset", i)
		}
	}

	total := 0
	for _, s := range a.segs {
		total += len(s.frames)
	}
	c.Check(total == len(frames), "total frames %d == %d", total, len(frames))
	if total != len(frames) {
		return nil
	}
	gi := 0
	for si, s := range a.segs {
		for _, fr := range s.frames {
			want := frames[gi]
			ok := fr.size == int64(len(want)) &&
				string(b[fr.chunkOff:fr.chunkOff+4]) == "00dc" &&
				int64(u32(b, fr.chunkOff+4)) == fr.size &&
				bytes.Equal(b[fr.dataOff:fr.dataOff+fr.size], want)
			c.Check(ok, "frame %d (segment %d) chunk header and bytes", gi, si)
			if fr.size&1 != 0 {
				c.Check(b[fr.dataOff+fr.size] == 0, "frame %d pad byte", gi)
			}
			gi++
		}
	}

	// Standard indexes: qwBaseOffset + dwOffset must address the first
	// byte of the JPEG body, dwSize is the un-padded frame length.
	gi = 0
	for si, s := range a.segs {
		n := len(s.frames)
		if n == 0 {
			c.Check(s.ixOff == 0, "empty segment %d has no ix00", si)
			continue
		}
		c.Check(s.ixOff != 0, "segment %d has ix00", si)
		if s.ixOff == 0 {
			return nil
		}
		c.Check(s.ixOff >= s.start && s.ixOff+s.ixTotal <= s.start+s.total,
			"segment %d ix00 lies inside the segment", si)
		c.Check(len(s.ix) == n, "segment %d ix00 entries %d == %d", si, len(s.ix), n)
		if len(s.ix) != n {
			return nil
		}
		for j, e := range s.ix {
			abs := int64(s.ixBase) + int64(e[0])
			fr := s.frames[j]
			c.Check(abs == fr.dataOff, "frame %d: qwBaseOffset+dwOffset points at JPEG body", gi)
			c.Check(e[1] == uint32(fr.size) && e[1]&0x80000000 == 0,
				"frame %d: ix00 dwSize is the un-padded length, high bit clear", gi)
			c.Check(b[abs] == 0xFF && b[abs+1] == 0xD8, "frame %d: JPEG SOI at indexed offset", gi)
			gi++
		}
	}

	// Super index.
	c.Check(a.hasIndx, "super index present")
	if !a.hasIndx {
		return nil
	}
	c.Check(len(a.supers) == int(maxSeg), "reserved super index entries %d == %d", len(a.supers), maxSeg)
	nonEmpty := 0
	for _, s := range a.segs {
		if len(s.frames) > 0 {
			nonEmpty++
		}
	}
	c.Check(int(a.indxCount) == nonEmpty, "nEntriesInUse %d == %d", a.indxCount, nonEmpty)
	if int(a.indxCount) != nonEmpty {
		return nil
	}
	si := 0
	for _, s := range a.segs {
		if len(s.frames) == 0 {
			continue
		}
		sup := a.supers[si]
		c.Check(sup.off == uint64(s.ixOff), "super index entry %d qwOffset is the ix00 chunk header", si)
		c.Check(sup.size == uint32(s.ixTotal), "super index entry %d dwSize includes the 8-byte shell", si)
		c.Check(sup.dur == uint32(len(s.frames)), "super index entry %d dwDuration is the segment frame count", si)
		si++
	}
	for i := nonEmpty; i < len(a.supers); i++ {
		c.Check(a.supers[i] == oSuper{}, "unused super index entry %d is zeroed", i)
	}

	// idx1 of the first segment, movi-relative offsets.
	s0 := a.segs[0]
	c.Check(s0.hasIdx1, "idx1 present in first segment")
	c.Check(len(s0.idx1) == len(s0.frames), "idx1 indexes the first segment only")
	if s0.hasIdx1 && len(s0.idx1) == len(s0.frames) {
		for j, e := range s0.idx1 {
			c.Check(e[0] == 0x63643030, "idx1 entry %d ckid is 00dc", j)
			c.Check(e[1] == 0x10, "idx1 entry %d flags AVIIF_KEYFRAME", j)
			c.Check(int64(e[2]) == s0.frames[j].chunkOff-s0.moviPos, "idx1 entry %d movi-relative offset", j)
			c.Check(int64(e[3]) == s0.frames[j].size, "idx1 entry %d size", j)
		}
	}
	for i, s := range a.segs[1:] {
		c.Check(!s.hasIdx1, "segment %d has no idx1", i+1)
	}

	// Frame counters.
	c.Check(int(a.avihTotal) == len(s0.frames), "avih.dwTotalFrames counts the first segment")
	c.Check(int(a.strhLen) == total, "strh.dwLength counts all segments")
	c.Check(a.hasDmlh && int(a.dmlhTotal) == total, "dmlh.dwTotalFrames counts all segments")

	return a
}

func TestOpenDMLSegments(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		dir := t.TempDir()
		f := filepath.Join(dir, "segments.avi")
		const segLimit = 4336
		const maxSeg = 8
		aw, err := NewOpenDML(f, 16, 16, 25, OpenDMLOptions{SegmentBytes: segLimit, MaxSegments: maxSeg})
		c.Check(err == nil, "NewOpenDML: %v", err)
		if err != nil {
			return
		}
		var frames [][]byte
		add := func(n int) bool {
			fr := makeFrame(n)
			err := aw.AddFrame(fr)
			c.Check(err == nil, "AddFrame (%d bytes)", n)
			if err != nil {
				return false
			}
			frames = append(frames, fr)
			return true
		}
		// Mixed frame sizes. The ninth 400-byte frame fills the first
		// segment exactly; the 112-byte frame would fit segment 2 if its
		// ix00 entry did not push the total over the limit.
		sizes := []int{
			400, 400, 400, 400, 400, 400, 400, 400, 400, // segment 1, exactly 4336 bytes
			400, 400, 400, 400, 400, 400, 400, 400, 400, 400, // segment 2
			112, 400, 400, 400, 400, 400, 400, 400, 400, 400, // segment 3 so far
		}
		for _, n := range sizes {
			if !add(n) {
				return
			}
		}
		// Too large for any segment: rejected, recording continues.
		err = aw.AddFrame(makeFrame(5000))
		c.Check(errors.Is(err, ErrTooLarge), "oversized frame rejected with ErrTooLarge, got %v", err)
		if !add(392) { // still joins the open segment 3, filling it exactly
			return
		}
		for _, n := range []int{400, 401, 400} { // segment 4, odd length included
			if !add(n) {
				return
			}
		}
		err = aw.Close()
		c.Check(err == nil, "Close: %v", err)
		if err != nil {
			return
		}
		_, statErr := os.Stat(f + ".idx_")
		c.Check(os.IsNotExist(statErr), "temporary index file removed")

		b, err := os.ReadFile(f)
		c.Check(err == nil, "read file: %v", err)
		if err != nil {
			return
		}
		a := verifyOpenDML(c, b, frames, segLimit, maxSeg)
		if a == nil {
			return
		}
		c.Check(reflect.DeepEqual(segCounts(a), []int{9, 10, 11, 3}),
			"frames per segment %v", segCounts(a))
		c.Check(reflect.DeepEqual(segTotals(a), []int64{4336, 4216, 4336, 1306}),
			"segment sizes %v", segTotals(a))
		c.Check(a.segs[0].total == segLimit, "first segment exactly fills the limit")
		c.Check(a.segs[2].total == segLimit, "third segment exactly fills the limit")

		// The classic single-RIFF writer keeps its original layout.
		f2 := filepath.Join(dir, "classic.avi")
		w2, err := New(f2, 16, 16, 25)
		c.Check(err == nil, "New: %v", err)
		if err != nil {
			return
		}
		var frames2 [][]byte
		for i := 0; i < 3; i++ {
			fr := makeFrame(120 + i)
			if err := w2.AddFrame(fr); err != nil {
				c.Check(false, "classic AddFrame %d: %v", i, err)
				return
			}
			frames2 = append(frames2, fr)
		}
		c.Check(len(frames2) == 3, "classic: 3 frames written")
		err = w2.Close()
		c.Check(err == nil, "classic Close: %v", err)
		if err != nil {
			return
		}
		b2, err := os.ReadFile(f2)
		c.Check(err == nil, "classic read: %v", err)
		if err != nil {
			return
		}
		a2, err := parseAVI(b2)
		c.Check(err == nil, "classic parse: %v", err)
		if err != nil {
			return
		}
		c.Check(len(a2.segs) == 1 && a2.segs[0].typ == "AVI ", "classic: single RIFF AVI segment")
		c.Check(!a2.hasIndx && !a2.hasDmlh, "classic: no indx/dmlh chunks")
		c.Check(a2.segs[0].ixOff == 0, "classic: no ix00 chunk")
		c.Check(len(a2.segs[0].idx1) == 3, "classic: idx1 has 3 entries")
		if len(a2.segs) == 1 && len(a2.segs[0].idx1) == 3 {
			for i, e := range a2.segs[0].idx1 {
				off := a2.segs[0].moviPos + int64(e[2])
				ok := string(b2[off:off+4]) == "00dc" &&
					bytes.Equal(b2[off+8:off+8+int64(e[3])], frames2[i])
				c.Check(ok, "classic: idx1 entry %d locates the frame", i)
			}
		}
		c.Check(a2.avihTotal == 3 && a2.strhLen == 3, "classic: frame counters")
	})
}

func TestOpenDMLIndexes(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		dir := t.TempDir()
		f := filepath.Join(dir, "indexes.avi")
		const segLimit = 4096
		const maxSeg = 4
		aw, err := NewOpenDML(f, 32, 24, 30, OpenDMLOptions{SegmentBytes: segLimit, MaxSegments: maxSeg})
		c.Check(err == nil, "NewOpenDML: %v", err)
		if err != nil {
			return
		}
		var frames [][]byte
		for i := 0; i < 51; i++ {
			n := 200
			if i&1 != 0 {
				n = 201 // odd length exercises padding
			}
			fr := makeFrame(n)
			err := aw.AddFrame(fr)
			c.Check(err == nil, "AddFrame %d: %v", i, err)
			if err != nil {
				return
			}
			frames = append(frames, fr)
		}
		err = aw.Close()
		c.Check(err == nil, "Close: %v", err)
		if err != nil {
			return
		}
		_, statErr := os.Stat(f + ".idx_")
		c.Check(os.IsNotExist(statErr), "temporary index file removed")

		b, err := os.ReadFile(f)
		c.Check(err == nil, "read file: %v", err)
		if err != nil {
			return
		}
		a := verifyOpenDML(c, b, frames, segLimit, maxSeg)
		if a == nil {
			return
		}
		c.Check(len(a.segs) == 3, "segment count %d", len(a.segs))
		c.Check(reflect.DeepEqual(segCounts(a), []int{15, 18, 18}),
			"frames per segment %v", segCounts(a))

		// Read every JPEG back through the super index alone.
		gi := 0
		for si := 0; si < int(a.indxCount); si++ {
			sup := a.supers[si]
			ixOff := int64(sup.off)
			ok := string(b[ixOff:ixOff+4]) == "ix00" &&
				int64(u32(b, ixOff+4))+8 == int64(sup.size)
			c.Check(ok, "super index entry %d addresses an ix00 chunk", si)
			if !ok {
				return
			}
			base := u64(b, ixOff+8+12)
			n := int(u32(b, ixOff+8+4))
			for j := 0; j < n; j++ {
				e := ixOff + 8 + 24 + int64(j)*8
				off := int64(base) + int64(u32(b, e))
				size := int64(u32(b, e+4))
				c.Check(bytes.Equal(b[off:off+size], frames[gi]),
					"frame %d read back via indx/ix00", gi)
				gi++
			}
		}
		c.Check(gi == len(frames), "super index reaches all %d frames", len(frames))
	})
}

func TestOpenDMLLimit(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		dir := t.TempDir()

		// Invalid options and oversized first-segment reservations fail
		// before any file or temporary index file is created.
		bad := []struct {
			name      string
			w, h, fps int32
			opts      OpenDMLOptions
		}{
			{"segment limit too small", 16, 16, 25, OpenDMLOptions{SegmentBytes: 100}},
			{"segment limit too big", 16, 16, 25, OpenDMLOptions{SegmentBytes: 2 << 30}},
			{"too many segments", 16, 16, 25, OpenDMLOptions{MaxSegments: 65536}},
			{"zero width", 0, 16, 25, OpenDMLOptions{}},
			{"negative height", 16, -1, 25, OpenDMLOptions{}},
			{"zero fps", 16, 16, 0, OpenDMLOptions{}},
			{"first segment reservation too large", 16, 16, 25, OpenDMLOptions{SegmentBytes: 4096, MaxSegments: 65535}},
		}
		for i, bc := range bad {
			f := filepath.Join(dir, fmt.Sprintf("bad%d.avi", i))
			aw, err := NewOpenDML(f, bc.w, bc.h, bc.fps, bc.opts)
			c.Check(err != nil && aw == nil, "%s: constructor fails", bc.name)
			_, err1 := os.Stat(f)
			_, err2 := os.Stat(f + ".idx_")
			c.Check(os.IsNotExist(err1) && os.IsNotExist(err2), "%s: no files left behind", bc.name)
		}

		// Empty recording: a single, complete AVI segment, no empty AVIX.
		f := filepath.Join(dir, "empty.avi")
		aw, err := NewOpenDML(f, 16, 16, 25, OpenDMLOptions{SegmentBytes: 4096, MaxSegments: 4})
		c.Check(err == nil, "empty: NewOpenDML: %v", err)
		if err != nil {
			return
		}
		err = aw.Close()
		c.Check(err == nil, "empty: Close: %v", err)
		if err != nil {
			return
		}
		b, err := os.ReadFile(f)
		c.Check(err == nil, "empty: read: %v", err)
		if err != nil {
			return
		}
		a := verifyOpenDML(c, b, nil, 4096, 4)
		if a == nil {
			return
		}
		c.Check(len(a.segs) == 1, "empty: single AVI segment, no AVIX")
		c.Check(int64(len(b)) == 288+16*4, "empty: file size %d", len(b))

		// A frame too large for any segment is rejected; the recording
		// continues with smaller frames.
		f = filepath.Join(dir, "toobig.avi")
		aw, err = NewOpenDML(f, 16, 16, 25, OpenDMLOptions{SegmentBytes: 4096, MaxSegments: 4})
		c.Check(err == nil, "toobig: NewOpenDML: %v", err)
		if err != nil {
			return
		}
		err = aw.AddFrame(makeFrame(4000))
		c.Check(errors.Is(err, ErrTooLarge), "toobig: ErrTooLarge, got %v", err)
		var frames [][]byte
		for _, n := range []int{3000, 500} {
			fr := makeFrame(n)
			err := aw.AddFrame(fr)
			c.Check(err == nil, "toobig: AddFrame(%d): %v", n, err)
			if err != nil {
				return
			}
			frames = append(frames, fr)
		}
		err = aw.Close()
		c.Check(err == nil, "toobig: Close: %v", err)
		if err != nil {
			return
		}
		b, err = os.ReadFile(f)
		c.Check(err == nil, "toobig: read: %v", err)
		if err != nil {
			return
		}
		a = verifyOpenDML(c, b, frames, 4096, 4)
		if a == nil {
			return
		}
		c.Check(len(a.segs) == 1, "toobig: accepted frames stayed in the first segment")

		// Super index capacity exhausted.
		f = filepath.Join(dir, "capacity.avi")
		aw, err = NewOpenDML(f, 16, 16, 25, OpenDMLOptions{SegmentBytes: 4096, MaxSegments: 2})
		c.Check(err == nil, "capacity: NewOpenDML: %v", err)
		if err != nil {
			return
		}
		frames = nil
		for _, n := range []int{3000, 500, 500, 3000} {
			fr := makeFrame(n)
			err := aw.AddFrame(fr)
			c.Check(err == nil, "capacity: AddFrame(%d): %v", n, err)
			if err != nil {
				return
			}
			frames = append(frames, fr)
		}
		err = aw.AddFrame(makeFrame(500))
		c.Check(errors.Is(err, ErrTooLarge), "capacity: ErrTooLarge, got %v", err)
		// The failed call must not have finalized the current segment:
		// a small frame still joins it.
		fr := makeFrame(100)
		err = aw.AddFrame(fr)
		c.Check(err == nil, "capacity: AddFrame after failure: %v", err)
		if err != nil {
			return
		}
		frames = append(frames, fr)
		err = aw.Close()
		c.Check(err == nil, "capacity: Close: %v", err)
		if err != nil {
			return
		}
		_, statErr := os.Stat(f + ".idx_")
		c.Check(os.IsNotExist(statErr), "capacity: temporary index file removed")
		b, err = os.ReadFile(f)
		c.Check(err == nil, "capacity: read: %v", err)
		if err != nil {
			return
		}
		a = verifyOpenDML(c, b, frames, 4096, 2)
		if a == nil {
			return
		}
		c.Check(len(a.segs) == 2, "capacity: two segments")
		c.Check(reflect.DeepEqual(segCounts(a), []int{2, 3}),
			"capacity: frames per segment %v", segCounts(a))
	})
}
