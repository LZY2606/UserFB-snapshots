package astits

import (
	"errors"
	"fmt"
	"io"
)

// ProgramRemuxConfig configures a single-program remuxer.
type ProgramRemuxConfig struct {
	ProgramNumber uint16
	OutputPMTPID  uint16
	FirstMediaPID uint16
}

// ProgramRemuxer extracts one program from a multi-program transport stream.
//
// The remuxer is intentionally byte-stream oriented: Write may receive chunks
// cut at any byte offset. Its successful output is always a sequence of
// complete 188-byte transport stream packets.
type ProgramRemuxer struct {
	w   io.Writer
	cfg ProgramRemuxConfig
	err error

	input []byte

	patASM      *sectionAssembler
	patGroups   map[programTableKey]*programSectionGroup
	havePAT     bool
	tsID        uint16
	patVer      *uint8
	patVersions map[uint16]*uint8

	patHasProgram bool
	patPMTPID     uint16

	pmtASM map[uint16]*sectionAssembler
	pmtVer *uint8

	active *programTopology

	nextMediaPID uint16
	cc           map[uint16]*programCC

	outVersion uint8
	published  bool
	lastSig    []byte
}

type programTableKey struct {
	tableIDExtension uint16
	version          uint8
	lastSection      uint8
}

type programSectionGroup struct {
	sections map[uint8][]byte
	done     bool
}

type sectionAssembler struct {
	buf []byte
}

type programCC struct {
	nextPayload uint8
	haveLast    bool
	lastPayload uint8
}

func (c *programCC) payload() uint8 {
	cc := c.nextPayload
	c.lastPayload = cc
	c.haveLast = true
	c.nextPayload = (cc + 1) & 0x0f
	return cc
}

func (c *programCC) adaptationOnly() uint8 {
	if c.haveLast {
		return c.lastPayload
	}
	return 0
}

type programIdentity struct {
	inputPID       uint16
	outputPID      uint16
	streamType     uint8
	independentPCR bool
	payloadStarted bool
}

type programES struct {
	streamType  uint8
	inputPID    uint16
	descriptors []byte
	identity    *programIdentity
}

type programTopology struct {
	inputPMTPID        uint16
	programNumber      uint16
	inputPCRPID        uint16
	outputPCRPID       uint16
	programDescriptors []byte
	streams            []*programES
	byInputPID         map[uint16]*programIdentity
	independentPCR     *programIdentity
}

var (
	errProgramRemuxInvalidConfig = errors.New("astits: invalid program remuxer configuration")
	errProgramRemuxMalformedTS   = errors.New("astits: malformed MPEG-TS packet")
	errProgramRemuxMalformedPSI  = errors.New("astits: malformed PSI section")
	errProgramRemuxIncomplete    = errors.New("astits: incomplete transport stream section")
	errProgramRemuxPIDExhausted  = errors.New("astits: output PID space exhausted")
	errProgramRemuxShortWrite    = errors.New("astits: short write without error")
)

const (
	programRemuxMinConfigPID = uint16(0x0020)
	programRemuxMaxPID       = uint16(0x1ffe)
	programRemuxNullPID      = uint16(0x1fff)
)

// NewProgramRemuxer creates a ProgramRemuxer.
func NewProgramRemuxer(w io.Writer, cfg ProgramRemuxConfig) (*ProgramRemuxer, error) {
	if w == nil {
		return nil, fmt.Errorf("%w: nil writer", errProgramRemuxInvalidConfig)
	}
	if cfg.ProgramNumber == 0 {
		return nil, fmt.Errorf("%w: program number must not be zero", errProgramRemuxInvalidConfig)
	}
	if cfg.OutputPMTPID < programRemuxMinConfigPID || cfg.OutputPMTPID > programRemuxMaxPID {
		return nil, fmt.Errorf("%w: output PMT PID must be between 0x20 and 0x1ffe", errProgramRemuxInvalidConfig)
	}
	if cfg.FirstMediaPID < programRemuxMinConfigPID || cfg.FirstMediaPID > programRemuxMaxPID {
		return nil, fmt.Errorf("%w: first media PID must be between 0x20 and 0x1ffe", errProgramRemuxInvalidConfig)
	}

	return &ProgramRemuxer{
		w:            w,
		cfg:          cfg,
		patASM:       &sectionAssembler{},
		patGroups:    map[programTableKey]*programSectionGroup{},
		patVersions:  map[uint16]*uint8{},
		pmtASM:       map[uint16]*sectionAssembler{},
		nextMediaPID: cfg.FirstMediaPID,
		cc:           map[uint16]*programCC{},
	}, nil
}

// Write accepts transport-stream bytes at arbitrary chunk boundaries.
func (r *ProgramRemuxer) Write(p []byte) (int, error) {
	if r.err != nil {
		return 0, r.err
	}

	consumed := 0

	if len(r.input) > 0 {
		need := MpegTsPacketSize - len(r.input)
		take := len(p)
		if take > need {
			take = need
		}
		r.input = append(r.input, p[:take]...)
		p = p[take:]
		consumed += take

		if len(r.input) < MpegTsPacketSize {
			return consumed, nil
		}

		packet := r.input
		r.input = nil
		if err := r.processPacket(packet); err != nil {
			return consumed, r.fail(err)
		}
	}

	for len(p) >= MpegTsPacketSize {
		packet := p[:MpegTsPacketSize]
		if err := r.processPacket(packet); err != nil {
			return consumed, r.fail(err)
		}
		p = p[MpegTsPacketSize:]
		consumed += MpegTsPacketSize
	}

	if len(p) > 0 {
		r.input = append(r.input[:0], p...)
		consumed += len(p)
	}

	return consumed, nil
}

// Flush ensures no partial TS packet or partially received PSI section remains.
// A complete section belonging to an incomplete multi-section table is valid.
func (r *ProgramRemuxer) Flush() error {
	if r.err != nil {
		return r.err
	}
	if len(r.input) > 0 {
		return r.fail(fmt.Errorf("%w: %d trailing byte(s)", errProgramRemuxMalformedTS, len(r.input)))
	}
	if len(r.patASM.buf) > 0 {
		return r.fail(fmt.Errorf("%w: PAT", errProgramRemuxIncomplete))
	}
	for pid, asm := range r.pmtASM {
		if len(asm.buf) > 0 {
			return r.fail(fmt.Errorf("%w: PMT on PID 0x%04x", errProgramRemuxIncomplete, pid))
		}
	}
	return nil
}

func (r *ProgramRemuxer) fail(err error) error {
	if r.err == nil {
		r.err = err
	}
	return r.err
}

func (r *ProgramRemuxer) writeFull(b []byte) error {
	for len(b) > 0 {
		n, err := r.w.Write(b)
		if err != nil {
			return r.fail(err)
		}
		if n < 0 || n > len(b) {
			return r.fail(fmt.Errorf("astits: writer returned invalid byte count %d for %d bytes", n, len(b)))
		}
		if n == 0 {
			return r.fail(errProgramRemuxShortWrite)
		}
		b = b[n:]
	}
	return nil
}

func (r *ProgramRemuxer) writePacket(packet []byte) error {
	if len(packet) != MpegTsPacketSize {
		return fmt.Errorf("astits: generated %d-byte TS packet", len(packet))
	}
	return r.writeFull(packet)
}

func (r *ProgramRemuxer) counter(pid uint16) *programCC {
	c, ok := r.cc[pid]
	if !ok {
		c = &programCC{}
		r.cc[pid] = c
	}
	return c
}

func (r *ProgramRemuxer) allocateOutputPID() (uint16, error) {
	for {
		pid := r.nextMediaPID
		if pid > programRemuxMaxPID {
			return 0, errProgramRemuxPIDExhausted
		}
		r.nextMediaPID++
		if pid == r.cfg.OutputPMTPID {
			continue
		}
		return pid, nil
	}
}

type tsHeaderInfo struct {
	pid            uint16
	pusi           bool
	hasAdaptation  bool
	hasPayload     bool
	adaptationOnly bool
	payloadOffset  int
}

func parseTSHeader(packet []byte) (tsHeaderInfo, error) {
	if len(packet) != MpegTsPacketSize || packet[0] != syncByte {
		return tsHeaderInfo{}, errProgramRemuxMalformedTS
	}

	h := tsHeaderInfo{
		pid:  uint16(packet[1]&0x1f)<<8 | uint16(packet[2]),
		pusi: packet[1]&0x40 != 0,
	}
	h.hasAdaptation = packet[3]&0x20 != 0
	h.hasPayload = packet[3]&0x10 != 0
	h.adaptationOnly = h.hasAdaptation && !h.hasPayload
	h.payloadOffset = 4

	if h.hasAdaptation {
		length := int(packet[4])
		h.payloadOffset = 5 + length
		if h.payloadOffset > MpegTsPacketSize || length < 0 {
			return tsHeaderInfo{}, errProgramRemuxMalformedTS
		}
	}
	if h.hasPayload && h.payloadOffset >= MpegTsPacketSize {
		return tsHeaderInfo{}, errProgramRemuxMalformedTS
	}
	if !h.hasPayload {
		h.payloadOffset = MpegTsPacketSize
	}
	return h, nil
}

func (r *ProgramRemuxer) processPacket(packet []byte) error {
	h, err := parseTSHeader(packet)
	if err != nil {
		return err
	}

	switch {
	case h.pid == 0x0000:
		return r.processPATPacket(packet, h)
	case r.isPMTPID(h.pid):
		return r.processPMTPacket(h.pid, packet, h)
	default:
		return r.processMediaPacket(packet, h)
	}
}

func (r *ProgramRemuxer) isPMTPID(pid uint16) bool {
	if r.havePAT && r.patHasProgram && pid == r.patPMTPID {
		return true
	}
	return r.active != nil && pid == r.active.inputPMTPID
}

func (r *ProgramRemuxer) payload(packet []byte, h tsHeaderInfo) []byte {
	if !h.hasPayload || h.payloadOffset >= len(packet) {
		return nil
	}
	return packet[h.payloadOffset:]
}

func (r *ProgramRemuxer) processPATPacket(packet []byte, h tsHeaderInfo) error {
	payload := r.payload(packet, h)
	if len(payload) == 0 {
		return nil
	}
	return r.patASM.feed(h.pusi, payload, func(section []byte) error {
		return r.collectPATSection(section)
	})
}

func (r *ProgramRemuxer) processPMTPacket(pid uint16, packet []byte, h tsHeaderInfo) error {
	asm, ok := r.pmtASM[pid]
	if !ok {
		asm = &sectionAssembler{}
		r.pmtASM[pid] = asm
	}
	payload := r.payload(packet, h)
	if len(payload) == 0 {
		return nil
	}
	return asm.feed(h.pusi, payload, func(section []byte) error {
		return r.handlePMTSection(pid, section)
	})
}

func (r *ProgramRemuxer) processMediaPacket(packet []byte, h tsHeaderInfo) error {
	if r.active == nil {
		return nil
	}
	id := r.active.byInputPID[h.pid]
	if id == nil {
		return nil
	}

	if id.independentPCR {
		// Adaptation-only PCR packets do not require a PES/PUSI start.
		if !h.adaptationOnly {
			if !id.payloadStarted {
				if !h.pusi || !h.hasPayload {
					return nil
				}
				id.payloadStarted = true
			}
		}
	} else if !id.payloadStarted {
		if !h.pusi || !h.hasPayload {
			return nil
		}
		id.payloadStarted = true
	}

	cc := r.counter(id.outputPID)
	var newCC uint8
	if h.adaptationOnly {
		newCC = cc.adaptationOnly()
	} else {
		newCC = cc.payload()
	}

	out := make([]byte, MpegTsPacketSize)
	copy(out, packet)
	out[1] = (out[1] & 0xe0) | byte(id.outputPID>>8)
	out[2] = byte(id.outputPID)
	out[3] = (out[3] & 0xf0) | (newCC & 0x0f)
	return r.writePacket(out)
}

// feed appends one TS payload to a streaming section assembler.
func (a *sectionAssembler) feed(pusi bool, payload []byte, emit func([]byte) error) error {
	data := payload
	if pusi {
		if len(payload) < 1 {
			return errProgramRemuxMalformedPSI
		}
		pointer := int(payload[0])
		if pointer > len(payload)-1 {
			return fmt.Errorf("%w: pointer_field %d exceeds payload", errProgramRemuxMalformedPSI, pointer)
		}
		boundary := 1 + pointer

		if len(a.buf) > 0 {
			prefix := payload[1:boundary]
			// If the old section header already gives its length, feed only
			// the bytes needed to finish it. The pointer boundary explicitly
			// starts a new section, so pointer filler/stuffing before that
			// boundary must not be parsed as part of the next section.
			needed := -1
			if len(a.buf) >= 3 {
				sectionLength := int(uint16(a.buf[1]&0x0f)<<8 | uint16(a.buf[2]))
				if sectionLength >= 9 && sectionLength <= 1021 {
					if total := sectionLength + 3; total >= len(a.buf) {
						needed = total - len(a.buf)
					}
				}
			}
			if needed >= 0 {
				if len(prefix) < needed {
					return fmt.Errorf("%w: pointer_field did not complete previous section", errProgramRemuxMalformedPSI)
				}
				prefix = prefix[:needed]
			}
			a.buf = append(a.buf, prefix...)
			if err := a.drain(emit); err != nil {
				return err
			}
			// Any unconsumed pointer prefix is filler before boundary.
			a.buf = nil
		}
		data = payload[boundary:]
	}

	a.buf = append(a.buf, data...)
	return a.drain(emit)
}

func (a *sectionAssembler) drain(emit func([]byte) error) error {
	for len(a.buf) > 0 {
		// 0xff is the reserved table ID used for stuffing to the end of a
		// TS payload.
		if a.buf[0] == 0xff {
			a.buf = nil
			return nil
		}
		if len(a.buf) < 3 {
			return nil
		}

		sectionLength := int(uint16(a.buf[1]&0x0f)<<8 | uint16(a.buf[2]))
		total := sectionLength + 3
		if sectionLength < 9 || sectionLength > 1021 || total > 1024 {
			return fmt.Errorf("%w: invalid section length %d", errProgramRemuxMalformedPSI, sectionLength)
		}
		if len(a.buf) < total {
			return nil
		}

		section := make([]byte, total)
		copy(section, a.buf[:total])
		a.buf = a.buf[total:]

		if err := emit(section); err != nil {
			return err
		}
	}
	return nil
}

type programSectionHeader struct {
	tableID       byte
	tableIDExt    uint16
	version       uint8
	currentNext   bool
	sectionNumber uint8
	lastNumber    uint8
}

func parseProgramSectionHeader(section []byte) (programSectionHeader, error) {
	if len(section) < 12 {
		return programSectionHeader{}, fmt.Errorf("%w: short section", errProgramRemuxMalformedPSI)
	}
	sectionLength := int(uint16(section[1]&0x0f)<<8 | uint16(section[2]))
	if len(section) != sectionLength+3 {
		return programSectionHeader{}, fmt.Errorf("%w: section length mismatch", errProgramRemuxMalformedPSI)
	}
	return programSectionHeader{
		tableID:       section[0],
		tableIDExt:    uint16(section[3])<<8 | uint16(section[4]),
		version:       (section[5] >> 1) & 0x1f,
		currentNext:   section[5]&0x01 != 0,
		sectionNumber: section[6],
		lastNumber:    section[7],
	}, nil
}

func verifyProgramSectionCRC(section []byte) error {
	if len(section) < 4 {
		return fmt.Errorf("%w: short CRC", errProgramRemuxMalformedPSI)
	}
	want := uint32(section[len(section)-4])<<24 |
		uint32(section[len(section)-3])<<16 |
		uint32(section[len(section)-2])<<8 |
		uint32(section[len(section)-1])
	got := computeCRC32(section[:len(section)-4])
	if got != want {
		return fmt.Errorf("%w: wanted %08x got %08x", ErrCRC32Mismatch, want, got)
	}
	return nil
}

func (r *ProgramRemuxer) collectPATSection(section []byte) error {
	if err := verifyProgramSectionCRC(section); err != nil {
		return r.fail(err)
	}
	h, err := parseProgramSectionHeader(section)
	if err != nil {
		return r.fail(err)
	}
	if h.tableID != 0x00 || !h.currentNext {
		return nil
	}

	key := programTableKey{tableIDExtension: h.tableIDExt, version: h.version, lastSection: h.lastNumber}
	group := r.patGroups[key]
	if group == nil {
		group = &programSectionGroup{sections: map[uint8][]byte{}}
		r.patGroups[key] = group
	}
	if _, ok := group.sections[h.sectionNumber]; !ok {
		group.sections[h.sectionNumber] = section
	}
	if group.done || !programGroupComplete(group, h.lastNumber) {
		return nil
	}
	group.done = true

	currentPATVersion := r.patVersions[h.tableIDExt]
	if !acceptProgramVersion(currentPATVersion, h.version) {
		return nil
	}

	r.havePAT = true
	r.tsID = h.tableIDExt
	v := h.version
	r.patVer = &v
	r.patVersions[h.tableIDExt] = &v

	found := false
	var pmtPID uint16
	for number := uint8(0); number <= h.lastNumber; number++ {
		sec := group.sections[number]
		for offset := 8; offset+4 <= len(sec)-4; offset += 4 {
			programNumber := uint16(sec[offset])<<8 | uint16(sec[offset+1])
			pid := uint16(sec[offset+2]&0x1f)<<8 | uint16(sec[offset+3])
			if programNumber == r.cfg.ProgramNumber {
				found = true
				pmtPID = pid
			}
		}
	}

	r.patHasProgram = found
	if found {
		r.patPMTPID = pmtPID
		if _, ok := r.pmtASM[pmtPID]; !ok {
			r.pmtASM[pmtPID] = &sectionAssembler{}
		}
		// A TSID-only PAT change is still an output PAT change. When the
		// signaled PMT binding is unchanged, republish both tables together.
		if r.active != nil && r.active.inputPMTPID == pmtPID {
			if err := r.emitProgramTables(r.active); err != nil {
				return err
			}
		}
	} else {
		// Removal is immediate and retires every active identity.
		r.active = nil
		r.patPMTPID = 0
	}
	return nil
}

func programGroupComplete(g *programSectionGroup, last uint8) bool {
	if len(g.sections) < int(last)+1 {
		return false
	}
	for i := uint8(0); i <= last; i++ {
		if _, ok := g.sections[i]; !ok {
			return false
		}
	}
	return true
}

// acceptProgramVersion applies the modulo-32 forward-distance rule.
func acceptProgramVersion(current *uint8, version uint8) bool {
	if current == nil {
		return true
	}
	distance := (int(version) - int(*current) + 32) % 32
	return distance >= 1 && distance <= 15
}

type parsedPMT struct {
	programNumber      uint16
	pcrPID             uint16
	programDescriptors []byte
	streams            []*programES
}

func parseProgramPMTSection(section []byte) (*parsedPMT, programSectionHeader, error) {
	h, err := parseProgramSectionHeader(section)
	if err != nil {
		return nil, h, err
	}
	if h.tableID != 0x02 {
		return nil, h, fmt.Errorf("%w: unexpected table id 0x%02x", errProgramRemuxMalformedPSI, h.tableID)
	}
	if h.sectionNumber != 0 || h.lastNumber != 0 {
		return nil, h, fmt.Errorf("%w: multi-section PMT is not supported", errProgramRemuxMalformedPSI)
	}
	if len(section) < 16 {
		return nil, h, fmt.Errorf("%w: short PMT", errProgramRemuxMalformedPSI)
	}

	p := &parsedPMT{programNumber: h.tableIDExt}
	p.pcrPID = uint16(section[8]&0x1f)<<8 | uint16(section[9])
	infoLength := int(uint16(section[10]&0x0f)<<8 | uint16(section[11]))
	descriptorEnd := 12 + infoLength
	loopEnd := len(section) - 4
	if descriptorEnd > loopEnd {
		return nil, h, fmt.Errorf("%w: program descriptors exceed section", errProgramRemuxMalformedPSI)
	}
	p.programDescriptors = append([]byte(nil), section[12:descriptorEnd]...)

	offset := descriptorEnd
	for offset < loopEnd {
		if offset+5 > loopEnd {
			return nil, h, fmt.Errorf("%w: short elementary stream entry", errProgramRemuxMalformedPSI)
		}
		es := &programES{streamType: section[offset]}
		es.inputPID = uint16(section[offset+1]&0x1f)<<8 | uint16(section[offset+2])
		esInfoLength := int(uint16(section[offset+3]&0x0f)<<8 | uint16(section[offset+4]))
		descEnd := offset + 5 + esInfoLength
		if descEnd > loopEnd {
			return nil, h, fmt.Errorf("%w: ES descriptors exceed section", errProgramRemuxMalformedPSI)
		}
		es.descriptors = append([]byte(nil), section[offset+5:descEnd]...)
		p.streams = append(p.streams, es)
		offset = descEnd
	}
	if offset != loopEnd {
		return nil, h, fmt.Errorf("%w: trailing PMT bytes", errProgramRemuxMalformedPSI)
	}
	return p, h, nil
}

func (r *ProgramRemuxer) handlePMTSection(pid uint16, section []byte) error {
	if err := verifyProgramSectionCRC(section); err != nil {
		return r.fail(err)
	}
	pmt, h, err := parseProgramPMTSection(section)
	if err != nil {
		return r.fail(err)
	}
	if !h.currentNext || pmt.programNumber != r.cfg.ProgramNumber {
		return nil
	}

	signaled := r.havePAT && r.patHasProgram && r.patPMTPID == pid
	activePID := r.active != nil && r.active.inputPMTPID == pid
	if !signaled && !activePID {
		return nil
	}

	if r.pmtVer != nil {
		distance := (int(h.version) - int(*r.pmtVer) + 32) % 32
		if distance == 0 {
			// A replayed version cannot regress state. It can, however,
			// complete a newly signaled binding or re-establish a program
			// after removal. Identities/table suppression handle idempotence.
			if signaled && (r.active == nil || r.active.inputPMTPID != pid) {
				return r.commitPMT(pid, h.version, pmt)
			}
			return nil
		}
		if distance == 16 || distance >= 17 {
			// Distance 16 is not accepted; 17..31 is an old version.
			return nil
		}
	}

	if !signaled && !activePID {
		return nil
	}
	return r.commitPMT(pid, h.version, pmt)
}

func (r *ProgramRemuxer) commitPMT(pid uint16, inputVersion uint8, pmt *parsedPMT) error {
	next := &programTopology{
		inputPMTPID:        pid,
		programNumber:      pmt.programNumber,
		inputPCRPID:        pmt.pcrPID,
		programDescriptors: append([]byte(nil), pmt.programDescriptors...),
		byInputPID:         map[uint16]*programIdentity{},
	}

	for _, es := range pmt.streams {
		var id *programIdentity
		if existing := next.byInputPID[es.inputPID]; existing != nil && existing.streamType == es.streamType && !existing.independentPCR {
			id = existing
		} else if r.active != nil {
			if old := r.active.byInputPID[es.inputPID]; old != nil && !old.independentPCR && old.streamType == es.streamType {
				id = old
			}
		}
		if id == nil {
			outPID, err := r.allocateOutputPID()
			if err != nil {
				return r.fail(err)
			}
			id = &programIdentity{
				inputPID:   es.inputPID,
				outputPID:  outPID,
				streamType: es.streamType,
			}
		}

		es.identity = id
		next.streams = append(next.streams, es)
		next.byInputPID[id.inputPID] = id
	}

	next.outputPCRPID = programRemuxNullPID
	if pmt.pcrPID != programRemuxNullPID {
		if id, ok := next.byInputPID[pmt.pcrPID]; ok {
			next.outputPCRPID = id.outputPID
		} else {
			var id *programIdentity
			if r.active != nil && r.active.independentPCR != nil && r.active.independentPCR.inputPID == pmt.pcrPID {
				id = r.active.independentPCR
			}
			if id == nil {
				outPID, err := r.allocateOutputPID()
				if err != nil {
					return r.fail(err)
				}
				id = &programIdentity{
					inputPID:       pmt.pcrPID,
					outputPID:      outPID,
					independentPCR: true,
				}
			}
			next.independentPCR = id
			next.byInputPID[pmt.pcrPID] = id
			next.outputPCRPID = id.outputPID
		}
	}

	if err := r.emitProgramTables(next); err != nil {
		return err
	}

	r.active = next
	v := inputVersion
	r.pmtVer = &v
	return nil
}

func (r *ProgramRemuxer) emitProgramTables(topo *programTopology) error {
	signature := append(r.buildPATSection(0), r.buildPMTSection(0, topo)...)
	if r.published && string(signature) == string(r.lastSig) {
		return nil
	}

	outputVersion := uint8(0)
	if r.published {
		outputVersion = (r.outVersion + 1) & 0x1f
	}
	patSection := r.buildPATSection(outputVersion)
	pmtSection := r.buildPMTSection(outputVersion, topo)

	// Packetize before writing. PID exhaustion/allocation has already
	// happened, so no half table can be published on those errors.
	patPackets := packetizeProgramSection(0x0000, patSection, r.counter(0x0000), true)
	pmtPackets := packetizeProgramSection(r.cfg.OutputPMTPID, pmtSection, r.counter(r.cfg.OutputPMTPID), true)

	for _, packet := range patPackets {
		if err := r.writePacket(packet); err != nil {
			return err
		}
	}
	for _, packet := range pmtPackets {
		if err := r.writePacket(packet); err != nil {
			return err
		}
	}

	r.outVersion = outputVersion
	r.published = true
	r.lastSig = signature
	return nil
}

func appendUint16(b []byte, v uint16) []byte {
	return append(b, byte(v>>8), byte(v))
}

func finishProgramSection(section []byte) []byte {
	crc := computeCRC32(section)
	return append(section, byte(crc>>24), byte(crc>>16), byte(crc>>8), byte(crc))
}

func (r *ProgramRemuxer) buildPATSection(version uint8) []byte {
	inner := []byte{}
	inner = appendUint16(inner, r.tsID)
	inner = append(inner, 0xc1|((version&0x1f)<<1), 0x00, 0x00)
	inner = appendUint16(inner, r.cfg.ProgramNumber)
	inner = appendUint16(inner, 0xe000|r.cfg.OutputPMTPID)

	sectionLength := len(inner) + 4
	section := []byte{0x00, 0xb0 | byte(sectionLength>>8), byte(sectionLength)}
	section = append(section, inner...)
	return finishProgramSection(section)
}

func (r *ProgramRemuxer) buildPMTSection(version uint8, topo *programTopology) []byte {
	inner := []byte{}
	inner = appendUint16(inner, r.cfg.ProgramNumber)
	inner = append(inner, 0xc1|((version&0x1f)<<1), 0x00, 0x00)
	inner = appendUint16(inner, 0xe000|topo.outputPCRPID)
	inner = appendUint16(inner, 0xf000|uint16(len(topo.programDescriptors)))
	inner = append(inner, topo.programDescriptors...)

	for _, es := range topo.streams {
		inner = append(inner, es.streamType)
		inner = appendUint16(inner, 0xe000|es.identity.outputPID)
		inner = appendUint16(inner, 0xf000|uint16(len(es.descriptors)))
		inner = append(inner, es.descriptors...)
	}

	sectionLength := len(inner) + 4
	section := []byte{0x02, 0xb0 | byte(sectionLength>>8), byte(sectionLength)}
	section = append(section, inner...)
	return finishProgramSection(section)
}

func packetizeProgramSection(pid uint16, section []byte, cc *programCC, pointer bool) [][]byte {
	var packets [][]byte
	sectionOffset := 0
	first := true

	for first || sectionOffset < len(section) {
		packet := make([]byte, MpegTsPacketSize)
		packet[0] = syncByte
		if first && pointer {
			packet[1] = 0x40 | byte(pid>>8)
		} else {
			packet[1] = byte(pid >> 8)
		}
		packet[2] = byte(pid)
		packet[3] = 0x10 | cc.payload()

		payload := packet[4:]
		for i := range payload {
			payload[i] = 0xff
		}

		payloadOffset := 0
		if first && pointer {
			payload[0] = 0x00
			payloadOffset = 1
		}

		available := len(payload) - payloadOffset
		take := len(section) - sectionOffset
		if take > available {
			take = available
		}
		copy(payload[payloadOffset:], section[sectionOffset:sectionOffset+take])
		sectionOffset += take

		packets = append(packets, packet)
		first = false
	}
	return packets
}
