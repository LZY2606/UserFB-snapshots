package oggvorbis

import (
	"io"

	"github.com/jfreymuth/vorbis"
)

// segmentPacketReader replays a contiguous run of indexed pages as a packet
// stream. Pages are re-read and CRC-checked through the shared page parser;
// packets spanning several pages are reassembled exactly as on the sequential
// path. No compressed content beyond the pages currently being replayed is
// retained.
type segmentPacketReader struct {
	rs      io.ReadSeeker
	pages   []pageMeta
	base    int // absolute page index of pages[0] inside chainIndex.pages
	pos     int
	page    page
	curAbs  int
	ready   bool
	index   int
	cont    bool
	serial  uint32
	haveSer bool
}

func newSegmentPacketReader(rs io.ReadSeeker, pages []pageMeta, base int) *segmentPacketReader {
	return &segmentPacketReader{rs: rs, pages: pages, base: base, curAbs: -1}
}

// NextPacket returns the next reassembled packet or io.EOF after the run's
// last completed packet.
func (pr *segmentPacketReader) NextPacket() (packet []byte, err error) {
	defer normalizeError(&err)
	if !pr.ready {
		if err = pr.loadPage(); err != nil {
			return nil, err
		}
		pr.index = 0
		if pr.page.HeaderTypeFlag&headerFlagContinuedPacket != 0 {
			pr.index = 1
		}
		pr.ready = true
	}
	if pr.index == pr.page.packetCount {
		rest := pr.page.packets[pr.page.packetCount]
		if err = pr.loadPage(); err != nil {
			return nil, err
		}
		if len(rest) > 0 {
			pr.page.packets[0] = append(rest, pr.page.packets[0]...)
		}
		pr.index = 0
		return pr.NextPacket()
	}
	packet = pr.page.packets[pr.index]
	pr.index++
	return packet, nil
}

func (pr *segmentPacketReader) loadPage() error {
	if pr.pos >= len(pr.pages) {
		return io.EOF
	}
	m := pr.pages[pr.pos]
	p, err := readAtPage(pr.rs, m.offset)
	if err != nil {
		return err
	}
	pr.validate(&p)
	pr.page = p
	pr.curAbs = pr.base + pr.pos
	pr.pos++
	pr.ready = true
	return nil
}

func (pr *segmentPacketReader) validate(p *page) {
	continued := p.HeaderTypeFlag&headerFlagContinuedPacket != 0
	if continued != pr.cont {
		panic(oggError(errContinuation.Error()))
	}
	if pr.haveSer && p.StreamSerialNumber != pr.serial {
		panic(oggError(errSerialChanged.Error()))
	}
	if !pr.haveSer {
		pr.serial = p.StreamSerialNumber
		pr.haveSer = true
	}
	pr.cont = p.needsContinue
}

// curPageIndex returns the absolute index of the page the last returned
// packet completed on.
func (pr *segmentPacketReader) curPageIndex() int { return pr.curAbs }

// atPageEnd reports whether every completed packet of the current page has
// been handed out.
func (pr *segmentPacketReader) atPageEnd() bool {
	return pr.ready && pr.index == pr.page.packetCount
}

// pageState returns the current page's local granule, whether it is known,
// and whether all of the page's packets have been consumed.
func (pr *segmentPacketReader) pageState() (granule int64, known bool, consumed bool) {
	g := pr.page.AbsoluteGranulePosition
	return g, g != -1, pr.atPageEnd()
}

// seekStream is the packet source used after a SetPosition. It replays indexed
// pages through the same parser as sequential reads and crosses chain segment
// boundaries by replaying the next segment's own three headers.
type seekStream struct {
	rs  io.ReadSeeker
	idx *chainIndex
	seg int
	pr  *segmentPacketReader
}

func newSeekStream(rs io.ReadSeeker, idx *chainIndex, seg int, pr *segmentPacketReader) *seekStream {
	return &seekStream{rs: rs, idx: idx, seg: seg, pr: pr}
}

func (s *seekStream) NextPacket() ([]byte, error) {
	if s.pr == nil {
		return nil, io.EOF
	}
	return s.pr.NextPacket()
}

// SegmentEnd reports whether the last returned packet ends the current segment.
func (s *seekStream) SegmentEnd() bool {
	return s.pr.atPageEnd() && s.pr.curAbs == s.idx.segments[s.seg].lastPage
}

func (s *seekStream) pageState() (int64, bool, bool) { return s.pr.pageState() }

// NextSegment closes the current segment and returns the three header packets
// of the following one, or io.EOF after the last segment.
func (s *seekStream) NextSegment() (id, comment, setup []byte, err error) {
	s.seg++
	if s.seg >= len(s.idx.segments) {
		s.pr = nil
		return nil, nil, nil, io.EOF
	}
	meta := s.idx.segments[s.seg]
	pages := s.idx.pages[meta.firstPage : meta.lastPage+1]
	s.pr = newSegmentPacketReader(s.rs, pages, meta.firstPage)
	hdr := make([][]byte, 3)
	for i := range hdr {
		hdr[i], err = s.pr.NextPacket()
		if err != nil {
			return nil, nil, nil, err
		}
	}
	return hdr[0], hdr[1], hdr[2], nil
}

// packetStream abstracts the two packet sources: sequential playback and
// index-backed seek playback.
type packetStream interface {
	NextPacket() ([]byte, error)
	SegmentEnd() bool
	pageState() (granule int64, known bool, consumed bool)
	NextSegment() (id, comment, setup []byte, err error)
}

// sequentialStream adapts oggReader to the packetStream interface.
type sequentialStream struct {
	r *oggReader
}

func (s *sequentialStream) NextPacket() ([]byte, error) { return s.r.NextPacket() }

func (s *sequentialStream) SegmentEnd() bool { return s.r.atSegmentEnd }

func (s *sequentialStream) pageState() (int64, bool, bool) {
	g := s.r.currentPage.AbsoluteGranulePosition
	consumed := s.r.ready && s.r.packetIndex == s.r.currentPage.packetCount
	return g, g != -1, consumed
}

func (s *sequentialStream) NextSegment() (id, comment, setup []byte, err error) {
	if err = s.r.NextSegment(); err != nil {
		return nil, nil, nil, err
	}
	hdr := make([][]byte, 3)
	for i := range hdr {
		hdr[i], err = s.r.NextPacket()
		if err != nil {
			return nil, nil, nil, err
		}
	}
	return hdr[0], hdr[1], hdr[2], nil
}

func readThreeHeaders(dec *vorbis.Decoder, id, comment, setup []byte) error {
	for _, h := range [][]byte{id, comment, setup} {
		if err := dec.ReadHeader(h); err != nil {
			return err
		}
	}
	return nil
}

// prepareSeek locates the segment and anchor page for pos, rebuilds the
// decoder with the target segment's headers and prerolls the overlap state.
// It returns the global frame the decoder position corresponds to; decoding
// resumes at the packet immediately following the anchor.
func (r *Reader) prepareSeek(pos int64) (int64, error) {
	idx := r.idx
	seg := idx.segmentFor(pos)
	meta := idx.segments[seg]

	var dec vorbis.Decoder
	if err := readThreeHeaders(&dec, meta.id, meta.comment, meta.setup); err != nil {
		return 0, err
	}
	// The target segment may use different block sizes, so the decode buffer
	// must be sized for this decoder, not for the first segment's.
	r.originalBuffer = make([]float32, dec.BufferSize())

	anchorAbs, granule := idx.seekPage(seg, pos)
	if granule == 0 {
		// No audio page with a known granule precedes the target: start the
		// segment cleanly from its first audio packet.
		pages := idx.pages[meta.firstPage : meta.lastPage+1]
		pr := newSegmentPacketReader(r.seeker, pages, meta.firstPage)
		for i := 0; i < 3; i++ {
			if _, err := pr.NextPacket(); err != nil {
				return 0, err
			}
		}
		dec.Clear()
		r.dec = dec
		r.ps = newSeekStream(r.seeker, idx, seg, pr)
		r.segStart = meta.start
		r.segProduced = 0
		return meta.start, nil
	}

	// Backtrack over continuation pages to the page where the anchor's last
	// completed packet begins, so a multi-page packet is reassembled whole.
	startAbs := anchorAbs
	for startAbs > meta.firstPage &&
		idx.pages[startAbs].header.HeaderTypeFlag&headerFlagContinuedPacket != 0 {
		startAbs--
	}
	pages := idx.pages[startAbs : meta.lastPage+1]
	pr := newSegmentPacketReader(r.seeker, pages, startAbs)

	// Consume through the last packet completed on the anchor page.
	var anchorPacket []byte
	for {
		p, err := pr.NextPacket()
		if err != nil {
			return 0, err
		}
		if pr.curPageIndex() == anchorAbs {
			anchorPacket = p
			if pr.atPageEnd() {
				break
			}
		} else if pr.curPageIndex() > anchorAbs {
			return 0, errContinuation
		}
	}

	// A single decode after Clear rebuilds the overlap window; its samples are
	// discarded and the position is taken from the page's granule instead.
	dec.Clear()
	buf := r.makeDecodeBuf(dec.BufferSize())
	if _, err := dec.DecodeInto(anchorPacket, buf); err != nil {
		return 0, err
	}
	r.dec = dec
	r.ps = newSeekStream(r.seeker, idx, seg, pr)
	r.segStart = meta.start
	r.segProduced = granule
	return meta.start + granule, nil
}

// makeDecodeBuf returns a scratch buffer of at least size floats, retaining
// its capacity across seeks and segment switches.
func (r *Reader) makeDecodeBuf(size int) []float32 {
	if cap(r.decodeBuf) < size {
		r.decodeBuf = make([]float32, size)
	}
	return r.decodeBuf[:size]
}
