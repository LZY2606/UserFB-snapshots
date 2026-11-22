package astits

import (
	"errors"
	"fmt"
	"io"
	"sort"
)

// Errors returned by the seek index API.
var (
	// ErrSeekIndexUnsupported is returned when the demuxer configuration or the
	// input is outside the supported seek index scope: unscrambled, lossless
	// 188-byte transport streams read through an io.ReadSeeker, without a custom
	// packets parser or packet skipper.
	ErrSeekIndexUnsupported = errors.New("astits: seek index unsupported")
	// ErrSeekIndexOwner is returned when a seek index is used with a demuxer
	// other than the one that built it.
	ErrSeekIndexOwner = errors.New("astits: seek index belongs to another demuxer")
	// ErrSeekIndexNotFound is returned when no indexed point matches the
	// requested program, epoch and PTS.
	ErrSeekIndexNotFound = errors.New("astits: no seek index point matches")
)

// SeekPoint represents a random access point in a transport stream.
type SeekPoint struct {
	// ProgramNumber is the program the point belongs to.
	ProgramNumber uint16
	// Epoch identifies the program topology generation the point belongs to.
	Epoch uint64
	// PID is the elementary stream PID carrying the random access PES.
	PID uint16
	// PTS90k is the presentation timestamp of the random access PES, in 90 kHz
	// units.
	PTS90k int64
	// Offset is the absolute position, from the beginning of the file, of the
	// first 188-byte TS packet of the random access PES.
	Offset int64
}

// SeekIndex is an immutable snapshot of the random access points of a
// transport stream. It is bound to the Demuxer that built it.
type SeekIndex struct {
	owner  *Demuxer
	points []seekIndexPoint
}

// seekIndexPoint carries the public point together with the classification
// state that was active when the point's first TS packet was read.
type seekIndexPoint struct {
	point SeekPoint
	snap  *seekSnapshot
}

// Points returns a copy of the indexed points, sorted by program, epoch and
// PTS. Mutating the returned slice does not affect the index.
func (i *SeekIndex) Points() []SeekPoint {
	if i == nil {
		return nil
	}
	ps := make([]SeekPoint, len(i.points))
	for idx, p := range i.points {
		ps[idx] = p.point
	}
	return ps
}

// seekSnapshot is the PAT/PMT classification state active when a PES first
// packet was read. SeekIndexed uses it to restore the demuxer without
// replaying tables from the beginning of the file.
type seekSnapshot struct {
	programMap map[uint32]uint16 // PMT PID -> program number
	streamMap  map[uint32]uint16 // elementary stream PID -> program number
}

// seekTopology is a complete PMT topology for a program.
type seekTopology struct {
	pmtPID      uint16
	epoch       uint64
	ready       bool
	fingerprint string
	esPIDs      map[uint16]struct{}
	videoPIDs   map[uint16]struct{}
}

// seekProgram is the scan-time state of a program.
type seekProgram struct {
	patPMTPID uint16 // PMT PID currently advertised by the PAT
	topo      seekTopology
}

// seekPES accumulates a PES during the scan. Only the header prefix and the
// total payload length are retained.
type seekPES struct {
	startOffset int64
	length      int
	header      []byte

	packetLength uint16
	pts          int64
	hasPTS       bool
	headerParsed bool

	// Eligibility is captured when the first packet is seen so that later
	// table changes cannot move the PES to a newer epoch.
	qualifies bool
	program   uint16
	epoch     uint64
	snap      *seekSnapshot
}

const seekPESHeaderPrefixMax = 6 + 3 + 255

// seekScanner performs a single isolated pass over the reader.
type seekScanner struct {
	r    io.ReadSeeker
	base int64

	programs  map[uint16]*seekProgram
	nextEpoch map[uint16]uint64 // program number -> next epoch value

	pmtOwner    map[uint16]uint16 // PMT PID -> program number
	streamOwner map[uint16]uint16 // elementary stream PID -> program number

	psiBuf map[uint16][]byte

	pes map[uint16]*seekPES

	points []seekIndexPoint
}

func newSeekScanner(r io.ReadSeeker) *seekScanner {
	return &seekScanner{
		r:           r,
		programs:    make(map[uint16]*seekProgram),
		nextEpoch:   make(map[uint16]uint64),
		pmtOwner:    make(map[uint16]uint16),
		streamOwner: make(map[uint16]uint16),
		psiBuf:      make(map[uint16][]byte),
		pes:         make(map[uint16]*seekPES),
	}
}

func (s *seekScanner) snapshot() *seekSnapshot {
	snap := &seekSnapshot{
		programMap: make(map[uint32]uint16, len(s.pmtOwner)),
		streamMap:  make(map[uint32]uint16, len(s.streamOwner)),
	}
	for pid, pn := range s.pmtOwner {
		snap.programMap[uint32(pid)] = pn
	}
	for pid, pn := range s.streamOwner {
		snap.streamMap[uint32(pid)] = pn
	}
	return snap
}

// isH264PID reports the program owning pid when pid currently carries an H.264
// elementary stream with a complete PMT topology.
func (s *seekScanner) isH264PID(pid uint16) (uint16, bool) {
	pn, ok := s.streamOwner[pid]
	if !ok {
		return 0, false
	}
	p, ok := s.programs[pn]
	if !ok || !p.topo.ready {
		return 0, false
	}
	if _, ok = p.topo.videoPIDs[pid]; !ok {
		return 0, false
	}
	return pn, true
}

// scan reads the stream from base and fills s.points.
func (s *seekScanner) scan() error {
	if _, err := s.r.Seek(s.base, io.SeekStart); err != nil {
		return fmt.Errorf("astits: seeking to scan start failed: %w", err)
	}

	buf := make([]byte, MpegTsPacketSize)
	var off int64
	for {
		n, err := io.ReadFull(s.r, buf)
		if err != nil {
			if err == io.EOF {
				s.finishRemaining()
				return nil
			}
			if err == io.ErrUnexpectedEOF {
				// A trailing partial packet is outside the supported scope.
				return fmt.Errorf("%w: truncated packet at offset %d", ErrSeekIndexUnsupported, s.base+off+int64(n))
			}
			return fmt.Errorf("astits: reading packet at offset %d failed: %w", s.base+off, err)
		}
		if buf[0] != syncByte {
			return fmt.Errorf("%w: packet at offset %d does not start with a sync byte", ErrSeekIndexUnsupported, s.base+off)
		}
		s.handlePacket(buf, s.base+off)
		off += MpegTsPacketSize
	}
}

// finishRemaining completes the pending zero-length PES at EOF.
func (s *seekScanner) finishRemaining() {
	for pid := range s.pes {
		s.finalizePES(pid)
	}
}

// finalizePES records a PES that is complete or has a zero length, then drops
// it. A bounded PES that is still incomplete is simply dropped.
func (s *seekScanner) finalizePES(pid uint16) {
	p, ok := s.pes[pid]
	if !ok {
		return
	}
	delete(s.pes, pid)
	if p.packetLength == 0 || p.length >= 6+int(p.packetLength) {
		s.recordPoint(pid, p)
	}
}

// handlePacket processes one 188-byte TS packet.
func (s *seekScanner) handlePacket(buf []byte, off int64) {
	pusi := buf[1]&0x40 > 0
	pid := uint16(buf[1]&0x1f)<<8 | uint16(buf[2])
	scrambling := (buf[3] >> 6) & 0x3
	afc := (buf[3] >> 4) & 0x3

	if pid == PIDNull || scrambling != 0 {
		return
	}

	// The TS packet header is the sync byte followed by
	// mpegTsPacketHeaderSize bytes.
	payloadStart := 1 + mpegTsPacketHeaderSize
	randomAccess := false
	if afc&0x2 != 0 {
		if payloadStart >= len(buf) {
			return
		}
		afLen := int(buf[payloadStart])
		if afLen > 0 && payloadStart+1 < len(buf) {
			randomAccess = buf[payloadStart+1]&0x40 > 0
		}
		payloadStart += 1 + afLen
		if payloadStart > len(buf) {
			return
		}
	}
	if afc&0x1 == 0 || payloadStart >= len(buf) {
		return
	}
	payload := buf[payloadStart:]

	if pid == PIDPAT {
		s.handlePSIPacket(pid, payload, pusi)
		return
	}
	if _, ok := s.pmtOwner[pid]; ok {
		s.handlePSIPacket(pid, payload, pusi)
		return
	}
	if _, ok := s.streamOwner[pid]; !ok {
		// A PES that started before a topology change keeps being collected so
		// the tables that follow cannot drop it; it keeps the generation bound
		// when its first packet arrived.
		if _, pending := s.pes[pid]; !pending {
			return
		}
	}
	s.handlePESPacket(pid, payload, pusi, randomAccess, off)
}

// handlePSIPacket accumulates a PSI PID and consumes complete sections.
func (s *seekScanner) handlePSIPacket(pid uint16, payload []byte, pusi bool) {
	if pusi {
		if len(payload) == 0 {
			s.psiBuf[pid] = nil
			return
		}
		pointer := int(payload[0])
		if 1+pointer > len(payload) {
			s.psiBuf[pid] = nil
			return
		}
		if pointer > 0 {
			// The bytes before the pointer finish the section that started in
			// the previous packet.
			s.psiBuf[pid] = append(s.psiBuf[pid], payload[1:1+pointer]...)
			s.consumePSI(pid)
		}
		// A payload unit start is a hard section boundary: anything left over
		// belongs to the previous section and is discarded.
		s.psiBuf[pid] = append(s.psiBuf[pid][:0], payload[1+pointer:]...)
	} else {
		s.psiBuf[pid] = append(s.psiBuf[pid], payload...)
	}
	s.consumePSI(pid)
}

// consumePSI parses every complete PAT/PMT section buffered for pid.
func (s *seekScanner) consumePSI(pid uint16) {
	b := s.psiBuf[pid]
	for {
		if len(b) < 3 {
			break
		}
		if b[0] == 0xff {
			b = b[:0]
			break
		}
		tableID := PSITableID(b[0])
		if tableID != PSITableIDPAT && tableID != PSITableIDPMT {
			b = b[:0]
			break
		}
		sectionLength := int(b[1]&0x0f)<<8 | int(b[2])
		total := 3 + sectionLength
		if len(b) < total {
			break
		}
		// Only current, single-section tables are supported.
		section := append([]byte(nil), b[:total]...)
		b = b[total:]
		s.handleSection(pid, tableID, section)
	}
	// Keep the (possibly advanced) slice.
	s.psiBuf[pid] = b
}

// handleSection validates a section and dispatches it.
func (s *seekScanner) handleSection(pid uint16, tableID PSITableID, section []byte) {
	if len(section) < 12 {
		return
	}
	// CRC32
	want := uint32(section[len(section)-4])<<24 | uint32(section[len(section)-3])<<16 |
		uint32(section[len(section)-2])<<8 | uint32(section[len(section)-1])
	if computeCRC32(section[:len(section)-4]) != want {
		return
	}
	// Valid current single-section.
	if section[1]&0x80 == 0 || section[5]&0x1 == 0 || section[6] != 0 || section[7] != 0 {
		return
	}

	switch tableID {
	case PSITableIDPAT:
		s.handlePAT(section)
	case PSITableIDPMT:
		s.handlePMT(pid, section)
	}
}

func (s *seekScanner) handlePAT(section []byte) {
	bindings := make(map[uint16]uint16)
	for o := 8; o+4 <= len(section)-4; o += 4 {
		pn := uint16(section[o])<<8 | uint16(section[o+1])
		pmtPID := uint16(section[o+2]&0x1f)<<8 | uint16(section[o+3])
		if pn == 0 {
			continue
		}
		bindings[pn] = pmtPID
	}

	// Programs removed from the PAT stop being recorded immediately.
	for pn := range s.programs {
		if _, ok := bindings[pn]; !ok {
			s.removeProgram(pn)
		}
	}

	for pn, pmtPID := range bindings {
		p, ok := s.programs[pn]
		if !ok {
			p = &seekProgram{patPMTPID: pmtPID, topo: seekTopology{pmtPID: pmtPID}}
			s.programs[pn] = p
			s.pmtOwner[pmtPID] = pn
			continue
		}
		if p.patPMTPID != pmtPID {
			// PMT migration: the current topology stays active until a complete
			// PMT appears on the new PID.
			p.patPMTPID = pmtPID
			s.pmtOwner[pmtPID] = pn
		}
	}
}

func (s *seekScanner) removeProgram(pn uint16) {
	if _, ok := s.programs[pn]; !ok {
		return
	}
	for pid, owner := range s.pmtOwner {
		if owner == pn {
			delete(s.pmtOwner, pid)
		}
	}
	for pid, owner := range s.streamOwner {
		if owner == pn {
			delete(s.streamOwner, pid)
			// A PES whose first packet arrived before the removal keeps the
			// epoch it was bound to; finalize it now and stop recording.
			if p, ok := s.pes[pid]; ok {
				s.recordPoint(pid, p)
				delete(s.pes, pid)
			}
		}
	}
	delete(s.programs, pn)
}

func (s *seekScanner) handlePMT(pid uint16, section []byte) {
	programNumber := uint16(section[3])<<8 | uint16(section[4])
	p, ok := s.programs[programNumber]
	if !ok || p.patPMTPID != pid {
		return
	}

	o := 8
	if o+4 > len(section)-4 {
		return
	}
	pcrPID := uint16(section[o]&0x1f)<<8 | uint16(section[o+1])
	programInfoLength := int(section[o+2]&0x0f)<<8 | int(section[o+3])
	o += 4
	if o+programInfoLength > len(section)-4 {
		return
	}
	programDescriptors := section[o : o+programInfoLength]
	o += programInfoLength

	fingerprint := fmt.Sprintf("%d|", pcrPID)
	fingerprint += string(programDescriptors) + "|"

	esPIDs := make(map[uint16]struct{})
	videoPIDs := make(map[uint16]struct{})
	for o+5 <= len(section)-4 {
		streamType := StreamType(section[o])
		esPID := uint16(section[o+1]&0x1f)<<8 | uint16(section[o+2])
		esInfoLength := int(section[o+3]&0x0f)<<8 | int(section[o+4])
		o += 5
		if o+esInfoLength > len(section)-4 {
			return
		}
		esDescriptors := section[o : o+esInfoLength]
		o += esInfoLength

		fingerprint += fmt.Sprintf("%d,%d,%s;", streamType, esPID, esDescriptors)
		esPIDs[esPID] = struct{}{}
		if streamType == StreamTypeH264Video {
			videoPIDs[esPID] = struct{}{}
		}
	}

	migrated := p.topo.ready && p.topo.pmtPID != pid
	changed := !p.topo.ready || migrated || fingerprint != p.topo.fingerprint
	if changed {
		epoch := s.nextEpoch[programNumber]
		s.nextEpoch[programNumber] = epoch + 1
		p.topo = seekTopology{
			pmtPID:      pid,
			epoch:       epoch,
			ready:       true,
			fingerprint: fingerprint,
			esPIDs:      esPIDs,
			videoPIDs:   videoPIDs,
		}
	} else {
		p.topo.esPIDs = esPIDs
		p.topo.videoPIDs = videoPIDs
	}

	// Refresh stream ownership for this program. A PES that is still being
	// assembled keeps the epoch it was bound to when its first packet arrived,
	// so it is not dropped here.
	for esPID, owner := range s.streamOwner {
		if owner == programNumber {
			if _, still := esPIDs[esPID]; !still {
				delete(s.streamOwner, esPID)
			}
		}
	}
	for esPID := range esPIDs {
		s.streamOwner[esPID] = programNumber
	}

	// Once migrated, the old PMT PID is no longer classified.
	if p.patPMTPID == pid {
		for oldPID, owner := range s.pmtOwner {
			if owner == programNumber && oldPID != pid {
				delete(s.pmtOwner, oldPID)
			}
		}
	}
}

// handlePESPacket accumulates a PES for pid.
func (s *seekScanner) handlePESPacket(pid uint16, payload []byte, pusi, randomAccess bool, off int64) {
	cur := s.pes[pid]

	if pusi {
		// A new payload start terminates the previous PES of this PID.
		s.finalizePES(pid)
		cur = &seekPES{startOffset: off}
		s.pes[pid] = cur
		if pn, ok := s.isH264PID(pid); ok && randomAccess {
			cur.qualifies = true
			cur.program = pn
			cur.epoch = s.programs[pn].topo.epoch
			cur.snap = s.snapshot()
		}
	}
	if cur == nil {
		return
	}

	cur.length += len(payload)
	if len(cur.header) < seekPESHeaderPrefixMax {
		n := seekPESHeaderPrefixMax - len(cur.header)
		if n > len(payload) {
			n = len(payload)
		}
		cur.header = append(cur.header, payload[:n]...)
	}

	if !cur.headerParsed {
		cur.parseHeader()
	}

	// A zero-length PES is delivered at the next payload start or at EOF.
	if cur.packetLength > 0 && cur.length >= 6+int(cur.packetLength) {
		s.recordPoint(pid, cur)
		delete(s.pes, pid)
	}
}

// parseHeader parses as much of the PES header as is available.
func (p *seekPES) parseHeader() {
	b := p.header
	if len(b) < 6 {
		return
	}
	if b[0] != 0 || b[1] != 0 || b[2] != 1 {
		p.headerParsed = true
		return
	}
	p.packetLength = uint16(b[4])<<8 | uint16(b[5])
	streamID := b[3]
	if !hasPESOptionalHeader(streamID) {
		p.headerParsed = true
		return
	}
	if len(b) < 9 {
		return
	}
	if b[6]&0xc0 != 0x80 {
		// MPEG-1 PES header: PTS parsing is not supported.
		p.headerParsed = true
		return
	}
	optionalLength := int(b[8])
	if len(b) < 9+optionalLength {
		return
	}
	p.headerParsed = true
	ptsIndicator := (b[7] >> 6) & 0x3
	if ptsIndicator == PTSDTSIndicatorOnlyPTS || ptsIndicator == PTSDTSIndicatorBothPresent {
		if optionalLength >= 5 {
			p.pts = decodeIndexPTS(b[9:14])
			p.hasPTS = true
		}
	}
}

// decodeIndexPTS decodes the 33-bit PTS field.
func decodeIndexPTS(bs []byte) int64 {
	return int64(uint64(bs[0])>>1&0x7<<30 | uint64(bs[1])<<22 | uint64(bs[2])>>1&0x7f<<15 |
		uint64(bs[3])<<7 | uint64(bs[4])>>1&0x7f)
}

// detectSeekPacketSize looks for the 188-byte sync grid from the beginning of
// the stream. It returns false when no regular 188-byte layout is found.
func detectSeekPacketSize(seeker io.ReadSeeker) (int, bool) {
	if _, err := seeker.Seek(0, io.SeekStart); err != nil {
		return 0, false
	}
	defer func() { _, _ = seeker.Seek(0, io.SeekStart) }()

	const probes = 4
	buf := make([]byte, MpegTsPacketSize*probes)
	n, err := io.ReadFull(seeker, buf)
	if err != nil && err != io.ErrUnexpectedEOF {
		return 0, false
	}
	if n < MpegTsPacketSize {
		// A stream shorter than one packet is treated as a single packet.
		return MpegTsPacketSize, true
	}
	if buf[0] != syncByte {
		return 0, false
	}
	for i := 1; i*MpegTsPacketSize < n; i++ {
		if buf[i*MpegTsPacketSize] != syncByte {
			return 0, false
		}
	}
	return MpegTsPacketSize, true
}

// recordPoint appends a point when the PES qualifies.
func (s *seekScanner) recordPoint(pid uint16, p *seekPES) {
	if p == nil || !p.qualifies || !p.hasPTS {
		return
	}
	s.points = append(s.points, seekIndexPoint{
		point: SeekPoint{
			ProgramNumber: p.program,
			Epoch:         p.epoch,
			PID:           pid,
			PTS90k:        p.pts,
			Offset:        p.startOffset,
		},
		snap: p.snap,
	})
}

// BuildSeekIndex scans the whole transport stream from its beginning and
// returns an index of random access points. The physical reader position and
// the demuxer state are preserved whatever the outcome.
func (dmx *Demuxer) BuildSeekIndex() (*SeekIndex, error) {
	if err := dmx.ctx.Err(); err != nil {
		return nil, err
	}
	if dmx.optPacketsParser != nil || dmx.optPacketSkipper != nil {
		return nil, fmt.Errorf("%w: a custom packets parser or packet skipper is set", ErrSeekIndexUnsupported)
	}
	if dmx.optPacketSize != 0 && dmx.optPacketSize != MpegTsPacketSize {
		return nil, fmt.Errorf("%w: only 188-byte packets are supported", ErrSeekIndexUnsupported)
	}
	seeker, ok := dmx.r.(io.ReadSeeker)
	if !ok {
		return nil, fmt.Errorf("%w: reader is not an io.ReadSeeker", ErrSeekIndexUnsupported)
	}

	pos, err := seeker.Seek(0, io.SeekCurrent)
	if err != nil {
		return nil, fmt.Errorf("%w: getting reader position failed: %v", ErrSeekIndexUnsupported, err)
	}

	// When the packet size has not been configured explicitly, detect it so
	// that other packet lengths are rejected instead of being misread.
	if dmx.optPacketSize == 0 {
		if size, ok := detectSeekPacketSize(seeker); !ok {
			_, _ = seeker.Seek(pos, io.SeekStart)
			return nil, fmt.Errorf("%w: cannot detect a 188-byte packet layout", ErrSeekIndexUnsupported)
		} else if size != MpegTsPacketSize {
			_, _ = seeker.Seek(pos, io.SeekStart)
			return nil, fmt.Errorf("%w: only 188-byte packets are supported", ErrSeekIndexUnsupported)
		}
		if _, err := seeker.Seek(pos, io.SeekStart); err != nil {
			return nil, fmt.Errorf("astits: restoring reader position failed: %w", err)
		}
	}

	s := newSeekScanner(seeker)
	scanErr := s.scan()

	if _, err := seeker.Seek(pos, io.SeekStart); err != nil && scanErr == nil {
		scanErr = fmt.Errorf("astits: restoring reader position failed: %w", err)
	}
	if scanErr != nil {
		return nil, scanErr
	}

	sort.SliceStable(s.points, func(i, j int) bool {
		a, b := s.points[i].point, s.points[j].point
		if a.ProgramNumber != b.ProgramNumber {
			return a.ProgramNumber < b.ProgramNumber
		}
		if a.Epoch != b.Epoch {
			return a.Epoch < b.Epoch
		}
		return a.PTS90k < b.PTS90k
	})

	return &SeekIndex{owner: dmx, points: s.points}, nil
}

// SeekIndexed positions the demuxer at the last indexed point of the given
// program and epoch whose PTS is not greater than pts90k. On success the
// demuxer is ready to resume reading sequentially from that point.
func (dmx *Demuxer) SeekIndexed(index *SeekIndex, program uint16, epoch uint64, pts90k int64) (SeekPoint, error) {
	if err := dmx.ctx.Err(); err != nil {
		return SeekPoint{}, err
	}
	if index == nil || index.owner != dmx {
		return SeekPoint{}, ErrSeekIndexOwner
	}
	if dmx.optPacketsParser != nil || dmx.optPacketSkipper != nil {
		return SeekPoint{}, fmt.Errorf("%w: a custom packets parser or packet skipper is set", ErrSeekIndexUnsupported)
	}
	if dmx.optPacketSize != 0 && dmx.optPacketSize != MpegTsPacketSize {
		return SeekPoint{}, fmt.Errorf("%w: only 188-byte packets are supported", ErrSeekIndexUnsupported)
	}
	seeker, ok := dmx.r.(io.ReadSeeker)
	if !ok {
		return SeekPoint{}, fmt.Errorf("%w: reader is not an io.ReadSeeker", ErrSeekIndexUnsupported)
	}

	var best *seekIndexPoint
	for i := range index.points {
		p := &index.points[i]
		if p.point.ProgramNumber != program || p.point.Epoch != epoch {
			continue
		}
		if p.point.PTS90k <= pts90k && (best == nil || p.point.PTS90k >= best.point.PTS90k) {
			best = p
		}
	}
	if best == nil {
		return SeekPoint{}, ErrSeekIndexNotFound
	}

	if _, err := seeker.Seek(best.point.Offset, io.SeekStart); err != nil {
		return SeekPoint{}, fmt.Errorf("astits: seeking to indexed offset %d failed: %w", best.point.Offset, err)
	}

	// Restore the PAT/PMT classification state active at the first packet.
	dmx.programMap.p = make(map[uint32]uint16, len(best.snap.programMap))
	for k, v := range best.snap.programMap {
		dmx.programMap.p[k] = v
	}
	dmx.streamMap.es = make(map[uint32]uint16, len(best.snap.streamMap))
	for k, v := range best.snap.streamMap {
		dmx.streamMap.es[k] = v
	}

	// Drop pending data, half PES and stale buffered packets.
	dmx.dataBuffer = nil
	dmx.packetPool = newPacketPool(dmx.programMap)
	dmx.packetBuffer = &packetBuffer{
		packetSize: MpegTsPacketSize,
		s:          dmx.optPacketSkipper,
		r:          dmx.r,
	}

	// Each PID must be resynchronized on its own payload unit start.
	dmx.seekSync = true
	dmx.seekAligned = make(map[uint32]bool)

	return best.point, nil
}
