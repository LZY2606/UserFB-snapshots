package oggvorbis

import (
	"bytes"
	"encoding/binary"
	"errors"
	"io"
)

// Errors returned for malformed or unsupported chained streams.
var (
	errBadCRC           = errors.New("ogg: wrong checksum")
	errBadCapture       = errors.New("ogg: missing capture pattern")
	errUnsupportedVer   = errors.New("ogg: unsupported version")
	errUnexpectedHeader = errors.New("ogg: unexpected beginning of stream page")
	errSerialChanged    = errors.New("ogg: stream serial number changed without end of stream")
	errContinuation     = errors.New("ogg: invalid continued packet flag")
	errLacing           = errors.New("ogg: end of stream page leaves an unfinished packet")
	errNoHeaders        = errors.New("ogg: segment is missing vorbis headers")
	errReusedSerial     = errors.New("ogg: chained stream reuses a serial number")
	errNoBOS            = errors.New("ogg: data page without a beginning of stream page")
	errNoGranule        = errors.New("ogg: final page has no granule position")
	errEOSNoGranule     = errors.New("ogg: end of stream page has no granule position")
	errSequence         = errors.New("ogg: page sequence number out of order")
)

// oggError lets page validation failures abort a recursive NextPacket call
// without changing its signature; normalizeError turns them back into errors.
type oggError string

func (e oggError) Error() string { return string(e) }

func normalizeError(errp *error) {
	if rec := recover(); rec != nil {
		if e, ok := rec.(oggError); ok {
			*errp = error(e)
			return
		}
		panic(rec)
	}
}

const (
	headerFlagContinuedPacket   = 1
	headerFlagBeginningOfStream = 2
	headerFlagEndOfStream       = 4
)

var capturePattern = [4]byte{'O', 'g', 'g', 'S'}

type pageHeader struct {
	CapturePattern          [4]byte
	StreamStructureVersion  uint8
	HeaderTypeFlag          byte
	AbsoluteGranulePosition int64
	StreamSerialNumber      uint32
	PageSequenceNumber      uint32
	PageChecksum            uint32
	PageSegments            uint8
}

func (p *pageHeader) isFirst() bool { return p.HeaderTypeFlag&headerFlagBeginningOfStream != 0 }
func (p *pageHeader) isLast() bool  { return p.HeaderTypeFlag&headerFlagEndOfStream != 0 }

// pageMeta holds everything the index remembers about one ogg page. The
// page's compressed content is never retained.
type pageMeta struct {
	offset int64
	size   int
	header pageHeader
	// number of packets completed on this page
	packetCount int
	// the page ends with a 0xFF lacing value, i.e. a packet continues on the next page
	needsContinue bool
	// lastKnownGranule is the page's own granule when known, otherwise the
	// granule of the most recent earlier page in the same segment. It is -1
	// when no page in the segment up to this one carries a known granule.
	// Unlike the raw granule field this sequence is monotonically
	// non-decreasing, which makes it safe to binary search.
	lastKnownGranule int64
	// anchorPage is the absolute page index carrying lastKnownGranule, or -1
	// when no earlier page in the segment has a known granule. A page with an
	// unknown granule (a continuation page completing no packet) can never
	// anchor a seek itself.
	anchorPage int
}

// chainMeta describes one logical stream inside a chained file.
type chainMeta struct {
	serial uint32
	// first and last page index belonging to the segment
	firstPage, lastPage int
	// number of valid frames in the segment, after end trimming
	length int64
	// global sample offset of the segment's local frame zero
	start int64
	// the segment's three header packets, retained for seek re-initialization
	id, comment, setup []byte
}

// page is a fully materialized ogg page.
type page struct {
	pageHeader
	headerChecksum uint32
	packetCount    int
	packetSizes    []int
	totalSize      int
	needsContinue  bool
	packets        [][]byte
}

func (p *page) read(r io.Reader) error {
	err := p.readHeader(r)
	if err != nil {
		return err
	}
	return p.readContent(r)
}

// readHeader reads only the page header and segment table, validating the
// capture pattern, version and header/segment-table checksum.
func (p *page) readHeader(r io.Reader) error {
	data := make([]byte, 27)
	_, err := io.ReadFull(r, data)
	if err != nil {
		return err
	}
	binary.Read(bytes.NewReader(data), binary.LittleEndian, &p.pageHeader)
	if p.CapturePattern != capturePattern {
		return errBadCapture
	}
	if p.StreamStructureVersion != 0 {
		return errUnsupportedVer
	}
	segmentTable := make([]byte, p.PageSegments)
	_, err = io.ReadFull(r, segmentTable)
	if err != nil {
		return noEOF(err)
	}
	data[22], data[23], data[24], data[25] = 0, 0, 0, 0
	p.headerChecksum = crcUpdate(0, data)
	p.headerChecksum = crcUpdate(p.headerChecksum, segmentTable)

	size := 0
	p.totalSize = 0
	p.packetCount = 0
	p.packetSizes = nil
	for _, s := range segmentTable {
		size += int(s)
		p.totalSize += int(s)
		if s < 0xFF {
			p.packetCount++
			p.packetSizes = append(p.packetSizes, size)
			size = 0
		}
	}
	// A page may legitimately have zero segments; guard against the
	// uint8 underflow of PageSegments-1 that would index an empty table.
	p.needsContinue = len(segmentTable) > 0 && segmentTable[len(segmentTable)-1] == 0xFF
	return nil
}

func (p *page) readContent(r io.Reader) error {
	content := make([]byte, p.totalSize)
	_, err := io.ReadFull(r, content)
	if err != nil {
		return noEOF(err)
	}
	checksum := crcUpdate(p.headerChecksum, content)
	if checksum != p.PageChecksum {
		return errBadCRC
	}
	p.packets = make([][]byte, p.packetCount+1)
	offset := 0
	for i, size := range p.packetSizes {
		p.packets[i] = content[offset : offset+size]
		offset += size
	}
	p.packets[p.packetCount] = content[offset:]
	return nil
}

func noEOF(err error) error {
	if err == io.EOF {
		return io.ErrUnexpectedEOF
	}
	return err
}

// readAtPage reads and checksum-validates a complete page at the given offset.
func readAtPage(rs io.ReadSeeker, offset int64) (page, error) {
	if _, err := rs.Seek(offset, io.SeekStart); err != nil {
		return page{}, err
	}
	var p page
	if err := p.read(rs); err != nil {
		return page{}, err
	}
	return p, nil
}

// oggReader is the sequential packet reader used by non-seekable inputs and
// during normal playback. It owns page parsing, continuation handling and
// chain validation. The index build and the seek machinery reuse the same
// page implementation instead of carrying a second parser.
//
// At every page boundary it reads at most one additional page ahead so that a
// physical EOF is known before the last page's packets are handed out. This
// keeps the legacy no-EOS end trimming exact without ever buffering the whole
// chain.
type oggReader struct {
	source io.Reader

	currentPage  page
	pending      *page
	physicalEOF  bool
	packetIndex  int
	ready        bool
	atSegmentEnd bool

	serial      uint32
	haveSerial  bool
	segmentOpen bool
	serials     map[uint32]bool
	prevCont    bool
	seqExpected uint32
	haveSeq     bool
}

func (r *oggReader) Read(p []byte) (int, error) {
	return r.source.Read(p)
}

// NextPacket returns the next complete packet of the current segment.
func (r *oggReader) NextPacket() (pkt []byte, err error) {
	defer normalizeError(&err)
	if !r.ready {
		if err = r.loadPage(); err != nil {
			return nil, err
		}
		r.packetIndex = 0
		if r.currentPage.HeaderTypeFlag&headerFlagContinuedPacket != 0 {
			r.packetIndex = 1
		}
		r.ready = true
	}
	if r.packetIndex == r.currentPage.packetCount {
		rest := r.currentPage.packets[r.currentPage.packetCount]
		if err = r.loadPage(); err != nil {
			return nil, err
		}
		if len(rest) > 0 {
			r.currentPage.packets[0] = append(rest, r.currentPage.packets[0]...)
		}
		r.packetIndex = 0
		r.ready = true
		return r.NextPacket()
	}
	packet := r.currentPage.packets[r.packetIndex]
	r.packetIndex++
	r.atSegmentEnd = false
	if r.packetIndex == r.currentPage.packetCount {
		if r.currentPage.isLast() || r.physicalEOF {
			r.atSegmentEnd = true
		}
	}
	return packet, nil
}

// loadPage makes the next page current, applying chain validation, then reads
// one page ahead to detect physical EOF early.
func (r *oggReader) loadPage() (err error) {
	defer normalizeError(&err)
	if r.pending != nil {
		r.currentPage = *r.pending
		r.pending = nil
	} else {
		if err = r.currentPage.read(r); err != nil {
			return err
		}
	}
	r.acceptPage()
	r.ready = false

	var next page
	if readErr := next.read(r); readErr != nil {
		if readErr == io.EOF {
			// Clean physical EOF. The compatibility case requires a complete
			// page with no unfinished packet; anything else is an error.
			if r.currentPage.needsContinue {
				return io.ErrUnexpectedEOF
			}
			r.physicalEOF = true
			return nil
		}
		return readErr
	}
	r.pending = &next
	r.physicalEOF = false
	return nil
}

// acceptPage validates the page that just became current against the chain rules.
func (r *oggReader) acceptPage() {
	p := &r.currentPage
	continued := p.HeaderTypeFlag&headerFlagContinuedPacket != 0
	if continued != r.prevCont {
		panic(oggError(errContinuation.Error()))
	}
	if p.isFirst() {
		if r.segmentOpen {
			panic(oggError(errUnexpectedHeader.Error()))
		}
		if r.serials == nil {
			r.serials = make(map[uint32]bool)
		}
		if r.serials[p.StreamSerialNumber] {
			panic(oggError(errReusedSerial.Error()))
		}
		r.serials[p.StreamSerialNumber] = true
		r.serial = p.StreamSerialNumber
		r.segmentOpen = true
		r.seqExpected = 0
		r.haveSeq = true
	} else {
		if !r.haveSerial {
			panic(oggError(errNoBOS.Error()))
		}
		if p.StreamSerialNumber != r.serial {
			panic(oggError(errSerialChanged.Error()))
		}
	}
	if r.haveSeq {
		if p.PageSequenceNumber != r.seqExpected {
			panic(oggError(errSequence.Error()))
		}
		r.seqExpected++
	}
	if p.isLast() {
		if p.needsContinue {
			panic(oggError(errLacing.Error()))
		}
		r.segmentOpen = false
	}
	r.prevCont = p.needsContinue
	r.haveSerial = true
}

// NextSegment advances from a finished segment to the next BOS page. It
// returns io.EOF when no further segment exists.
func (r *oggReader) NextSegment() (err error) {
	defer normalizeError(&err)
	r.atSegmentEnd = false
	r.ready = false
	r.prevCont = false
	if r.physicalEOF {
		return io.EOF
	}
	// The lookahead page (if any) becomes current on the next NextPacket call.
	return nil
}
