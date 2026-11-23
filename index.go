package oggvorbis

import (
	"io"
)

// chainIndex is the container-level index built once for seekable inputs.
// It stores per-page metadata and per-segment headers and lengths, but never
// compressed page content or decoded PCM. Its size therefore grows with the
// number of pages, segments and header packets, not with the audio's duration.
type chainIndex struct {
	pages    []pageMeta
	segments []chainMeta
	// total number of valid (trimmed) frames across all segments
	length int64
	// physical EOF was reached right after a complete page, without a pending
	// continuation packet
	cleanEOF bool
}

// buildChainIndex scans the whole container once, sequentially. Every page is
// fully parsed and CRC-checked through the shared page implementation; page
// content is dropped again as soon as the page has been validated.
func buildChainIndex(rs io.ReadSeeker) (_ *chainIndex, err error) {
	if _, err := rs.Seek(0, io.SeekStart); err != nil {
		return nil, err
	}
	idx := &chainIndex{}

	var (
		cur          *chainMeta
		expectCont   bool
		prevEndsOpen bool
		headerCount  int
		openPacket   []byte
		lastKnown    int64 = -1
		anchorPage   int   = -1
		seqExpected  uint32
	)
	serials := map[uint32]bool{}
	var offset int64

	for {
		var p page
		readErr := p.read(io.Reader(rs))
		if readErr != nil {
			if readErr == io.EOF {
				// Physical EOF between pages.
				if prevEndsOpen {
					return nil, io.ErrUnexpectedEOF
				}
				idx.cleanEOF = true
				break
			}
			if readErr == io.ErrUnexpectedEOF {
				// A truncated page header/content at physical EOF is always an error.
				return nil, readErr
			}
			return nil, readErr
		}
		// Page sequence numbers are continuous within each logical stream and
		// restart at zero on a new beginning-of-stream page.
		if p.isFirst() {
			seqExpected = 0
		}
		if p.PageSequenceNumber != seqExpected {
			return nil, errSequence
		}
		seqExpected++

		meta := pageMeta{
			offset:           offset,
			size:             27 + int(p.PageSegments) + p.totalSize,
			header:           p.pageHeader,
			packetCount:      p.packetCount,
			needsContinue:    p.needsContinue,
			lastKnownGranule: lastKnown,
			anchorPage:       anchorPage,
		}
		offset += int64(meta.size)
		idx.pages = append(idx.pages, meta)
		if p.AbsoluteGranulePosition >= 0 {
			lastKnown = p.AbsoluteGranulePosition
			anchorPage = len(idx.pages) - 1
			idx.pages[len(idx.pages)-1].lastKnownGranule = lastKnown
			idx.pages[len(idx.pages)-1].anchorPage = anchorPage
		}

		// Reassemble packets that span pages. A page's trailing fragment
		// (packets[packetCount]) becomes the open packet; when the next page
		// carries the continued-packet flag, its first completed packet is
		// prefixed with the accumulated fragment.
		continued := p.HeaderTypeFlag&headerFlagContinuedPacket != 0
		completed := make([][]byte, p.packetCount)
		for i := 0; i < p.packetCount; i++ {
			completed[i] = p.packets[i]
		}
		if continued {
			if p.packetCount > 0 {
				joined := make([]byte, 0, len(openPacket)+len(p.packets[0]))
				joined = append(joined, openPacket...)
				joined = append(joined, p.packets[0]...)
				completed[0] = joined
				openPacket = nil
			} else {
				openPacket = append(openPacket, p.packets[p.packetCount]...)
			}
		}
		if p.needsContinue {
			if p.packetCount > 0 || !continued {
				openPacket = cloneBytes(p.packets[p.packetCount])
			}
		} else {
			openPacket = nil
		}

		if continued != expectCont {
			return nil, errContinuation
		}
		expectCont = p.needsContinue
		prevEndsOpen = p.needsContinue

		if p.isFirst() {
			if cur != nil {
				// Another BOS appears while the previous segment was never
				// closed with an EOS page.
				return nil, errUnexpectedHeader
			}
			if serials[p.StreamSerialNumber] {
				return nil, errReusedSerial
			}
			serials[p.StreamSerialNumber] = true
			cur = &chainMeta{
				serial:    p.StreamSerialNumber,
				firstPage: len(idx.pages) - 1,
				lastPage:  len(idx.pages) - 1,
			}
			headerCount = 0
			// Granule ancestry does not cross segment boundaries.
			lastKnown = -1
			anchorPage = -1
			if p.AbsoluteGranulePosition >= 0 {
				lastKnown = p.AbsoluteGranulePosition
				anchorPage = len(idx.pages) - 1
			}
			idx.pages[len(idx.pages)-1].lastKnownGranule = lastKnown
			idx.pages[len(idx.pages)-1].anchorPage = anchorPage
		} else {
			if cur == nil {
				return nil, errNoBOS
			}
			if p.StreamSerialNumber != cur.serial {
				return nil, errSerialChanged
			}
			cur.lastPage = len(idx.pages) - 1
		}
		// The first three completed packets of every segment are the
		// identification, comment and setup headers. They may share pages or
		// span pages, so use the reassembled packets.
		if headerCount < 3 {
			for i := 0; i < len(completed) && headerCount < 3; i++ {
				pkt := cloneBytes(completed[i])
				switch headerCount {
				case 0:
					cur.id = pkt
				case 1:
					cur.comment = pkt
				case 2:
					cur.setup = pkt
				}
				headerCount++
			}
		}

		if p.isLast() {
			if cur.id == nil || cur.comment == nil || cur.setup == nil {
				return nil, errNoHeaders
			}
			if p.AbsoluteGranulePosition < 0 {
				return nil, errEOSNoGranule
			}
			cur.length = p.AbsoluteGranulePosition
			cur.start = idx.length
			idx.length += cur.length
			idx.segments = append(idx.segments, *cur)
			cur = nil
			expectCont = false
			prevEndsOpen = false
			headerCount = 0
		}
	}

	if cur != nil {
		// Legacy compatibility: the file ends at physical EOF and the last
		// segment has no EOS page. This is only acceptable when the last page
		// was complete and no packet was left unfinished.
		if !idx.cleanEOF || prevEndsOpen {
			return nil, io.ErrUnexpectedEOF
		}
		last := idx.pages[len(idx.pages)-1]
		gran := last.header.AbsoluteGranulePosition
		if gran < 0 {
			return nil, errNoGranule
		}
		cur.length = gran
		cur.start = idx.length
		idx.length += cur.length
		idx.segments = append(idx.segments, *cur)
	}

	if len(idx.segments) == 0 {
		return nil, io.ErrUnexpectedEOF
	}
	return idx, nil
}

func cloneBytes(b []byte) []byte {
	c := make([]byte, len(b))
	copy(c, b)
	return c
}

// segmentFor returns the segment containing global frame pos.
func (idx *chainIndex) segmentFor(pos int64) int {
	// Segments are stored in order; binary search on their start offset.
	lo, hi := 0, len(idx.segments)
	for lo < hi {
		mid := (lo + hi) / 2
		s := idx.segments[mid]
		if s.start <= pos && pos < s.start+s.length {
			return mid
		}
		if pos < s.start {
			hi = mid
		} else {
			lo = mid + 1
		}
	}
	// pos may equal the total length (one-past-the-end): return the last segment.
	return len(idx.segments) - 1
}

// seekPage finds the page to start decoding from when targeting global frame
// pos inside segment seg. It returns the absolute index of an anchor page
// (a page with a known granule) and that granule in local segment
// coordinates. When no page with a granule at or before pos exists, the
// segment's first page and granule zero are returned.
//
// Pages whose granule is -1 (a page with no completed packet) never anchor a
// seek: such a page contains only a continuation of a packet whose granule
// is not known yet.
func (idx *chainIndex) seekPage(seg int, pos int64) (pageIdx int, localGranule int64) {
	s := idx.segments[seg]
	local := pos - s.start
	// lastKnownGranule is monotonically non-decreasing within the segment.
	lo, hi := s.firstPage, s.lastPage+1
	for lo < hi {
		mid := (lo + hi) / 2
		g := idx.pages[mid].lastKnownGranule
		if g < 0 || g > local {
			hi = mid
		} else {
			lo = mid + 1
		}
	}
	if lo == s.firstPage {
		return s.firstPage, 0
	}
	anchor := idx.pages[lo-1].anchorPage
	if anchor < 0 {
		return s.firstPage, 0
	}
	return anchor, idx.pages[anchor].header.AbsoluteGranulePosition
}
