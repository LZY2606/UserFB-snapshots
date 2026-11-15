package astits

import (
	"bytes"
	"errors"
	"fmt"
	"io"

	"github.com/asticode/go-astikit"
)

// Program remuxer configuration bounds.
const (
	programRemuxMinPID  uint16 = 0x0020
	programRemuxMaxPID  uint16 = 0x1ffe
	programRemuxNullPID uint16 = 0x1fff
	psiMaxSectionLength        = 1021
	psiSyntaxHeaderSize        = 5
	psiCRC32Size               = 4
	descriptorTagCA     uint8  = 0x09 // conditional access descriptor, carries a CA PID
)

// Errors returned by ProgramRemuxer.
var (
	ErrProgramNumberZero        = errors.New("astits: program number must not be zero")
	ErrRemuxPIDOutOfRange       = errors.New("astits: output PID must be between 0x20 and 0x1ffe")
	ErrRemuxMediaPIDCollision   = errors.New("astits: first media PID must not be the PMT PID")
	ErrRemuxPIDExhausted        = errors.New("astits: no free output PID left")
	ErrRemuxUnsupportedInput    = errors.New("astits: scrambled streams, CA descriptors and descriptors referencing PIDs are not supported")
	ErrRemuxPartialTSData       = errors.New("astits: input ended in the middle of a TS packet")
	ErrRemuxPartialPSISection   = errors.New("astits: input ended in the middle of a PSI section")
	ErrRemuxPSIPacketTooSmall   = errors.New("astits: PSI packet payload too small to contain pointer field")
	ErrRemuxInvalidPSISection   = errors.New("astits: malformed PSI section")
	ErrRemuxUnsupportedPMTState = errors.New("astits: PMT section must have section_number and last_section_number equal to 0")
)

// ProgramRemuxConfig configures a ProgramRemuxer.
type ProgramRemuxConfig struct {
	ProgramNumber uint16 // Program number to extract, must not be zero
	OutputPMTPID  uint16 // PID carrying the generated PMT in the output stream
	FirstMediaPID uint16 // First PID usable for remapped elementary and standalone PCR streams
}

// remuxIdentity describes one remapped input PID inside a committed topology.
type remuxIdentity struct {
	inputPID   uint16
	outputPID  uint16
	streamType StreamType // zero for a standalone PCR identity
	started    bool       // a PUSI payload (ES) or any packet (standalone PCR) has been emitted
}

// remuxTopology is an immutable snapshot produced by one committed PMT version.
type remuxTopology struct {
	programNumber uint16
	pmtPID        uint16
	pcrPID        uint16
	// Input PID -> identity, covering every ES PID and, when distinct, the PCR PID.
	identities map[uint16]*remuxIdentity
	// ES identities in PMT declaration order.
	esOrder []*remuxIdentity
	// Standalone PCR identity (PCR PID not used by any ES), nil otherwise.
	pcrIdentity *remuxIdentity
	// Raw PMT payload (PCR PID, descriptors and ES entries) with output PIDs applied.
	pmt *PMTData
}

// psiStreamState holds the in-progress section bytes of one input PID.
type psiStreamState struct {
	pending     []byte
	pendingLeft int
}

// psiSectionState tracks the sections making up one table version on one PID.
type psiSectionState struct {
	version     uint8
	tableIDExt  uint16
	lastSection uint8
	currentNext bool
	sections    map[uint8][]byte // section_number -> raw section bytes (table id through CRC)
}

// psiAssembler reassembles PSI sections out of arbitrary TS packet payload
// boundaries. State is kept per PID: packets of other PIDs may interleave a
// section that spans several packets.
type psiAssembler struct {
	streams  map[uint16]*psiStreamState
	progress map[uint16]map[uint8]*psiSectionState // pid -> version -> in-progress table
	complete map[uint16]*psiSectionState           // last accepted completed table per pid
}

func newPSIAssembler() *psiAssembler {
	return &psiAssembler{
		streams:  make(map[uint16]*psiStreamState),
		progress: make(map[uint16]map[uint8]*psiSectionState),
		complete: make(map[uint16]*psiSectionState),
	}
}

// hasPendingSection reports whether a section is still being collected.
func (a *psiAssembler) hasPendingSection() bool {
	for _, s := range a.streams {
		if s.pendingLeft > 0 {
			return true
		}
	}
	return false
}

// feed consumes one TS packet payload on pid and returns every freshly completed
// table version (its raw sections, ordered by section number). Replayed versions
// are never returned twice.
func (a *psiAssembler) feed(pid uint16, pusi bool, payload []byte) ([][]byte, error) {
	st := a.streams[pid]
	if st == nil {
		st = &psiStreamState{}
		a.streams[pid] = st
	}

	var done [][]byte
	if pusi {
		if len(payload) < 1 {
			return nil, ErrRemuxPSIPacketTooSmall
		}
		pointer := int(payload[0])
		payload = payload[1:]
		if pointer > len(payload) {
			return nil, fmt.Errorf("%w: pointer field %d exceeds %d payload bytes", ErrRemuxInvalidPSISection, pointer, len(payload))
		}
		// A pointer field first completes the section carried by previous packets.
		if st.pendingLeft > 0 {
			if pointer < st.pendingLeft {
				return nil, fmt.Errorf("%w: pointer field ends before the previous section is complete", ErrRemuxInvalidPSISection)
			}
			st.pending = append(st.pending, payload[:st.pendingLeft]...)
			payload = payload[st.pendingLeft:]
			pointer -= st.pendingLeft
			st.pendingLeft = 0
			section := st.pending
			st.pending = nil
			s, err := a.storeSection(pid, section)
			if err != nil {
				return nil, err
			}
			done = append(done, s...)
		}
		// Remaining pointer filler bytes belong to the previous section.
		payload = payload[pointer:]
	}

	for len(payload) > 0 {
		if st.pendingLeft == 0 {
			// 0xff marks the end of sections in this payload.
			if payload[0] == 0xff {
				return done, nil
			}
			if len(payload) < 3 {
				return nil, fmt.Errorf("%w: truncated section header", ErrRemuxInvalidPSISection)
			}
			sectionLength := int(uint16(payload[1]&0x0f)<<8 | uint16(payload[2]))
			// A syntax section always contains the 5-byte syntax header and the
			// 4-byte CRC32; the field itself is bounded by 1021.
			if sectionLength < psiSyntaxHeaderSize+psiCRC32Size || sectionLength > psiMaxSectionLength {
				return nil, fmt.Errorf("%w: section length %d out of range", ErrRemuxInvalidPSISection, sectionLength)
			}
			st.pendingLeft = 3 + sectionLength
			st.pending = st.pending[:0]
		}
		n := st.pendingLeft
		if n > len(payload) {
			n = len(payload)
		}
		st.pending = append(st.pending, payload[:n]...)
		payload = payload[n:]
		st.pendingLeft -= n
		if st.pendingLeft == 0 {
			section := st.pending
			st.pending = nil
			s, err := a.storeSection(pid, section)
			if err != nil {
				return nil, err
			}
			done = append(done, s...)
		}
	}
	return done, nil
}

// storeSection validates one section and, when a whole current table version is
// collected and newer than the accepted one, returns its sections once.
func (a *psiAssembler) storeSection(pid uint16, raw []byte) ([][]byte, error) {
	if len(raw) < 8 {
		return nil, fmt.Errorf("%w: section shorter than 8 bytes", ErrRemuxInvalidPSISection)
	}
	tableID := PSITableID(raw[0])
	if !tableID.hasCRC32() {
		return nil, fmt.Errorf("%w: table id %d has no CRC32", ErrRemuxInvalidPSISection, tableID)
	}
	crcOffset := len(raw) - psiCRC32Size
	gotCRC := uint32(raw[crcOffset])<<24 | uint32(raw[crcOffset+1])<<16 | uint32(raw[crcOffset+2])<<8 | uint32(raw[crcOffset+3])
	if computeCRC32(raw[:crcOffset]) != gotCRC {
		return nil, ErrCRC32Mismatch
	}

	tableIDExt := uint16(raw[3])<<8 | uint16(raw[4])
	version := (raw[5] & 0x3f) >> 1
	currentNext := raw[5]&0x01 == 1
	sectionNumber := raw[6]
	lastSectionNumber := raw[7]
	if sectionNumber > lastSectionNumber {
		return nil, fmt.Errorf("%w: section_number %d greater than last_section_number %d", ErrRemuxInvalidPSISection, sectionNumber, lastSectionNumber)
	}

	// Versions may interleave, so in-progress tables are keyed by version.
	versions := a.progress[pid]
	if versions == nil {
		versions = make(map[uint8]*psiSectionState)
		a.progress[pid] = versions
	}
	st := versions[version]
	if st == nil || st.tableIDExt != tableIDExt || st.lastSection != lastSectionNumber || st.currentNext != currentNext {
		st = &psiSectionState{
			version:     version,
			tableIDExt:  tableIDExt,
			lastSection: lastSectionNumber,
			currentNext: currentNext,
			sections:    make(map[uint8][]byte),
		}
		versions[version] = st
	}
	st.sections[sectionNumber] = append([]byte(nil), raw...)

	// Only current tables with every section collected are usable.
	if !currentNext || len(st.sections) != int(lastSectionNumber)+1 {
		return nil, nil
	}

	// Modulo-32 forward distance: 1..15 updates, 16 refused, 17..31 are old,
	// 0 is a replay of the accepted version. None of them roll state backwards.
	if prev := a.complete[pid]; prev != nil {
		distance := (int(version) - int(prev.version) + 32) % 32
		if distance < 1 || distance > 15 {
			return nil, nil
		}
	}
	a.complete[pid] = st

	out := make([][]byte, 0, len(st.sections))
	for i := uint8(0); i <= lastSectionNumber; i++ {
		out = append(out, st.sections[i])
	}
	return out, nil
}

// remuxCC tracks the continuity counter of one output media PID.
type remuxCC struct {
	lastPayload uint8 // CC of the last payload packet
	seen        bool  // whether any packet was emitted
}

// ProgramRemuxer extracts one program out of a multi-program MPEG-TS and writes
// a standalone, single-program transport stream. Compressed audio and video is
// never decoded: admitted packets are copied verbatim, only their TS header PID
// and continuity counter are rewritten. PCR, PTS and DTS are preserved.
type ProgramRemuxer struct {
	w      io.Writer
	cfg    ProgramRemuxConfig
	err    error
	psi    *psiAssembler
	tsid   uint16 // transport_stream_id from the committed PAT
	active *remuxTopology
	// PMT PID candidate advertised by the latest PAT while its PMT is pending.
	candidatePMTPID uint16
	hasCandidate    bool
	// Output table bookkeeping.
	tableVersion uint8
	tablesSent   bool
	patCC        uint8
	pmtCC        uint8
	// Output PID allocation.
	nextMediaPID uint16
	usedOutputs  map[uint16]bool
	cc           map[uint16]*remuxCC
	// Input framing.
	pendingTS []byte
}

// NewProgramRemuxer creates a ProgramRemuxer writing into w.
func NewProgramRemuxer(w io.Writer, cfg ProgramRemuxConfig) (*ProgramRemuxer, error) {
	if cfg.ProgramNumber == 0 {
		return nil, ErrProgramNumberZero
	}
	if !validRemuxPID(cfg.OutputPMTPID) || !validRemuxPID(cfg.FirstMediaPID) {
		return nil, ErrRemuxPIDOutOfRange
	}
	if cfg.FirstMediaPID == cfg.OutputPMTPID {
		return nil, ErrRemuxMediaPIDCollision
	}
	return &ProgramRemuxer{
		w:            w,
		cfg:          cfg,
		psi:          newPSIAssembler(),
		nextMediaPID: cfg.FirstMediaPID,
		usedOutputs:  map[uint16]bool{cfg.OutputPMTPID: true},
		cc:           make(map[uint16]*remuxCC),
	}, nil
}

func validRemuxPID(pid uint16) bool {
	return pid >= programRemuxMinPID && pid <= programRemuxMaxPID
}

// Write feeds arbitrary bytes of the input transport stream. It blocks until all
// complete 188-byte packets have been processed. Short writes of the underlying
// writer are transparently retried; once a write fails the error is sticky.
func (r *ProgramRemuxer) Write(p []byte) (int, error) {
	n := len(p)
	if r.err != nil {
		return 0, r.err
	}
	r.pendingTS = append(r.pendingTS, p...)
	for len(r.pendingTS) >= MpegTsPacketSize {
		if err := r.processPacket(r.pendingTS[:MpegTsPacketSize]); err != nil {
			r.err = err
			return 0, err
		}
		r.pendingTS = r.pendingTS[MpegTsPacketSize:]
	}
	return n, nil
}

// Flush finalizes the output. Ending inside PES payload data is allowed, but a
// partial TS packet or an unfinished PSI section is an error. A version whose
// sections were all received but which never formed a complete table is dropped.
func (r *ProgramRemuxer) Flush() error {
	if r.err != nil {
		return r.err
	}
	if len(r.pendingTS) > 0 {
		r.err = ErrRemuxPartialTSData
		return r.err
	}
	if r.psi.hasPendingSection() {
		r.err = ErrRemuxPartialPSISection
		return r.err
	}
	return nil
}

// writeAll retries short writes until b is fully consumed.
func (r *ProgramRemuxer) writeAll(b []byte) error {
	for len(b) > 0 {
		n, err := r.w.Write(b)
		if err != nil {
			return err
		}
		if n <= 0 {
			return io.ErrShortWrite
		}
		if n > len(b) {
			n = len(b)
		}
		b = b[n:]
	}
	return nil
}

// processPacket handles one full input TS packet.
func (r *ProgramRemuxer) processPacket(raw []byte) error {
	if raw[0] != syncByte {
		return ErrPacketMustStartWithASyncByte
	}
	p, err := parsePacket(astikit.NewBytesIterator(raw), nil)
	if err != nil {
		return err
	}
	if p.Header.TransportErrorIndicator {
		// A packet flagged with a transport error carries no usable content.
		return nil
	}
	if p.Header.TransportScramblingControl != ScramblingControlNotScrambled {
		return ErrRemuxUnsupportedInput
	}

	switch p.Header.PID {
	case PIDPAT:
		sections, err := r.psi.feed(p.Header.PID, p.Header.PayloadUnitStartIndicator, p.Payload)
		if err != nil {
			return err
		}
		if len(sections) > 0 {
			if err := r.handlePAT(sections); err != nil {
				return err
			}
		}
		return nil
	case PIDNull:
		return nil
	}

	// PMT packets belong either to the active topology or to a migration candidate.
	if (r.active != nil && p.Header.PID == r.active.pmtPID) ||
		(r.hasCandidate && p.Header.PID == r.candidatePMTPID) {
		sections, err := r.psi.feed(p.Header.PID, p.Header.PayloadUnitStartIndicator, p.Payload)
		if err != nil {
			return err
		}
		for _, section := range sections {
			if err := r.handlePMT(p.Header.PID, section); err != nil {
				return err
			}
		}
		return nil
	}

	// Everything else is media, admitted only through a committed topology.
	if r.active != nil {
		return r.copyMedia(p, raw)
	}
	return nil
}

// handlePAT consumes the ordered sections of one complete PAT table version.
func (r *ProgramRemuxer) handlePAT(sections [][]byte) error {
	programs := make(map[uint16]uint16) // program number -> PMT PID
	var tsid uint16
	for _, raw := range sections {
		tsid = uint16(raw[3])<<8 | uint16(raw[4])
		i := astikit.NewBytesIterator(raw)
		i.Seek(3 + psiSyntaxHeaderSize)
		d, err := parsePATSection(i, len(raw)-psiCRC32Size, tsid)
		if err != nil {
			return err
		}
		for _, pgm := range d.Programs {
			programs[pgm.ProgramNumber] = pgm.ProgramMapID
		}
	}
	r.tsid = tsid

	pmtPID, found := programs[r.cfg.ProgramNumber]
	if !found {
		// The selected program was removed from a complete PAT: media admission
		// stops immediately until a later PAT and PMT rebuild the topology.
		r.active = nil
		r.hasCandidate = false
		return nil
	}

	if r.active != nil && pmtPID == r.active.pmtPID {
		// Unchanged binding: PMT updates keep arriving on the same PID.
		r.hasCandidate = false
		return nil
	}
	if r.hasCandidate && pmtPID == r.candidatePMTPID {
		return nil
	}
	// New PMT PID: build a candidate only; the old topology keeps working until
	// a complete PMT for the selected program arrives on the new PID.
	r.candidatePMTPID = pmtPID
	r.hasCandidate = true
	return nil
}

// handlePMT consumes one complete PMT section arriving on pid.
func (r *ProgramRemuxer) handlePMT(pid uint16, raw []byte) error {
	// PMT input is limited to a single section.
	if raw[6] != 0 || raw[7] != 0 {
		return ErrRemuxUnsupportedPMTState
	}
	tableIDExt := uint16(raw[3])<<8 | uint16(raw[4])
	if tableIDExt != r.cfg.ProgramNumber {
		// A PMT for another program can never complete our candidate.
		return nil
	}

	i := astikit.NewBytesIterator(raw)
	i.Seek(3 + psiSyntaxHeaderSize)
	d, err := parsePMTSection(i, len(raw)-psiCRC32Size, tableIDExt)
	if err != nil {
		return err
	}
	if err := validatePMTForRemux(d); err != nil {
		return err
	}

	if r.hasCandidate {
		// While a migration candidate is pending, only a PMT arriving on the
		// candidate PID can complete it. A later PMT on the old PID must keep
		// the old topology untouched and can never complete the candidate.
		if pid != r.candidatePMTPID {
			return nil
		}
		r.hasCandidate = false
		return r.commitTopology(pid, d)
	}
	if r.active != nil && pid == r.active.pmtPID {
		// PMT refresh on the unchanged binding: whole-table commit only.
		return r.commitTopology(pid, d)
	}
	return nil
}

// validatePMTForRemux rejects CA and descriptors that are not opaque copies.
func validatePMTForRemux(d *PMTData) error {
	for _, desc := range d.ProgramDescriptors {
		if !descriptorIsOpaque(desc) {
			return ErrRemuxUnsupportedInput
		}
	}
	for _, es := range d.ElementaryStreams {
		for _, desc := range es.ElementaryStreamDescriptors {
			if !descriptorIsOpaque(desc) {
				return ErrRemuxUnsupportedInput
			}
		}
	}
	return nil
}

// descriptorIsOpaque reports whether desc can be copied byte for byte without
// interpreting conditional-access or embedded-PID semantics. Registration,
// ISO 639, user-defined and unknown opaque descriptors are allowed.
func descriptorIsOpaque(d *Descriptor) bool {
	// The CA descriptor embeds a CA PID and is never allowed.
	if d.Tag == descriptorTagCA {
		return false
	}
	switch d.Tag {
	case DescriptorTagRegistration, DescriptorTagISO639LanguageAndAudioType:
		return true
	}
	if d.Tag >= 0x80 && d.Tag <= 0xfe {
		return true
	}
	// Structured descriptors other than the explicitly allowed tags may carry
	// semantics (including PID references); only unknown opaque ones are copied.
	return d.Unknown != nil
}

// commitTopology builds the new identity set, regenerates the output tables and
// only then admits the new topology's media.
func (r *ProgramRemuxer) commitTopology(pmtPID uint16, d *PMTData) error {
	topo, err := r.buildTopology(pmtPID, d)
	if err != nil {
		return err
	}

	// A retransmitted table with identical topology produces no output tables.
	if r.active != nil && topologiesEqual(r.active, topo) {
		r.active = topo
		return nil
	}

	if err := r.emitTables(topo); err != nil {
		return err
	}
	r.active = topo
	r.tablesSent = true
	return nil
}

// buildTopology allocates output PIDs, preserving PIDs of surviving ES identities
// whose stream type did not change and of a surviving standalone PCR PID.
func (r *ProgramRemuxer) buildTopology(pmtPID uint16, d *PMTData) (*remuxTopology, error) {
	topo := &remuxTopology{
		programNumber: d.ProgramNumber,
		pmtPID:        pmtPID,
		pcrPID:        d.PCRPID,
		identities:    make(map[uint16]*remuxIdentity),
	}

	// Plan allocations locally first: PID exhaustion must not publish a half
	// table, so the remuxer state is only updated once the full plan succeeds.
	used := make(map[uint16]bool, len(r.usedOutputs))
	for pid := range r.usedOutputs {
		used[pid] = true
	}
	nextPID := r.nextMediaPID
	allocate := func() (uint16, error) {
		for pid := nextPID; pid <= programRemuxMaxPID; pid++ {
			if used[pid] {
				continue
			}
			used[pid] = true
			nextPID = pid + 1
			return pid, nil
		}
		return 0, ErrRemuxPIDExhausted
	}

	// Survivors: same input ES PID and same stream type keep their output PID.
	prevES := map[uint16]*remuxIdentity{}
	if r.active != nil {
		for _, id := range r.active.esOrder {
			prevES[id.inputPID] = id
		}
	}

	for _, es := range d.ElementaryStreams {
		id := &remuxIdentity{
			inputPID:   es.ElementaryPID,
			streamType: es.StreamType,
		}
		if old := prevES[es.ElementaryPID]; old != nil && old.streamType == es.StreamType {
			id.outputPID = old.outputPID
			id.started = old.started
		} else {
			pid, err := allocate()
			if err != nil {
				return nil, err
			}
			id.outputPID = pid
		}
		topo.identities[es.ElementaryPID] = id
		topo.esOrder = append(topo.esOrder, id)
	}

	// Standalone PCR PID is allocated after the ES streams.
	if d.PCRPID != programRemuxNullPID {
		if _, isES := topo.identities[d.PCRPID]; !isES {
			id := &remuxIdentity{inputPID: d.PCRPID}
			if r.active != nil && r.active.pcrIdentity != nil &&
				r.active.pcrIdentity.inputPID == d.PCRPID {
				// A surviving standalone clock keeps its output PID and state.
				id.outputPID = r.active.pcrIdentity.outputPID
				id.started = r.active.pcrIdentity.started
			} else {
				pid, err := allocate()
				if err != nil {
					return nil, err
				}
				id.outputPID = pid
			}
			topo.identities[d.PCRPID] = id
			topo.pcrIdentity = id
		}
	}

	// Build the output PMT, applying output PIDs; descriptors keep their bytes.
	outPMT := &PMTData{
		ProgramNumber:      d.ProgramNumber,
		ProgramDescriptors: cloneDescriptors(d.ProgramDescriptors),
		PCRPID:             programRemuxNullPID,
	}
	if d.PCRPID != programRemuxNullPID {
		outPMT.PCRPID = topo.identities[d.PCRPID].outputPID
	}
	for idx, es := range d.ElementaryStreams {
		outPMT.ElementaryStreams = append(outPMT.ElementaryStreams, &PMTElementaryStream{
			StreamType:                  es.StreamType,
			ElementaryPID:               topo.esOrder[idx].outputPID,
			ElementaryStreamDescriptors: cloneDescriptors(es.ElementaryStreamDescriptors),
		})
	}
	topo.pmt = outPMT

	// Commit the allocation plan.
	r.usedOutputs = used
	r.nextMediaPID = nextPID
	return topo, nil
}

// topologiesEqual compares the committed identity layout of two topologies.
func topologiesEqual(a, b *remuxTopology) bool {
	if a.pmtPID != b.pmtPID || a.pcrPID != b.pcrPID || len(a.identities) != len(b.identities) {
		return false
	}
	for pid, idA := range a.identities {
		idB, ok := b.identities[pid]
		if !ok || idA.outputPID != idB.outputPID || idA.streamType != idB.streamType {
			return false
		}
	}
	return pmtDataEqual(a.pmt, b.pmt)
}

// pmtDataEqual compares the PMT content that matters for output generation.
func pmtDataEqual(a, b *PMTData) bool {
	if a.ProgramNumber != b.ProgramNumber || a.PCRPID != b.PCRPID ||
		len(a.ElementaryStreams) != len(b.ElementaryStreams) ||
		!descriptorsEqual(a.ProgramDescriptors, b.ProgramDescriptors) {
		return false
	}
	for idx := range a.ElementaryStreams {
		ea, eb := a.ElementaryStreams[idx], b.ElementaryStreams[idx]
		if ea.StreamType != eb.StreamType || ea.ElementaryPID != eb.ElementaryPID ||
			!descriptorsEqual(ea.ElementaryStreamDescriptors, eb.ElementaryStreamDescriptors) {
			return false
		}
	}
	return true
}

func descriptorsEqual(a, b []*Descriptor) bool {
	return bytes.Equal(serializeDescriptors(a), serializeDescriptors(b))
}

func cloneDescriptors(ds []*Descriptor) []*Descriptor {
	if len(ds) == 0 {
		return nil
	}
	out := make([]*Descriptor, len(ds))
	for i, d := range ds {
		cp := *d
		out[i] = &cp
	}
	return out
}

// emitTables writes a complete PAT then PMT pair, bumping the shared output
// version on every real topology change. Media of the new topology is only
// admitted after both tables have been fully written.
func (r *ProgramRemuxer) emitTables(topo *remuxTopology) error {
	if r.tablesSent {
		r.tableVersion = (r.tableVersion + 1) & 0x1f
	}
	pat := &PATData{
		TransportStreamID: r.tsid,
		Programs: []*PATProgram{{
			ProgramNumber: r.cfg.ProgramNumber,
			ProgramMapID:  r.cfg.OutputPMTPID,
		}},
	}
	patSection := buildSectionBytes(PSITableIDPAT, r.tsid, r.tableVersion, pat)
	patPackets, err := packetizeSection(PIDPAT, &r.patCC, patSection)
	if err != nil {
		return err
	}
	pmtSection := buildSectionBytes(PSITableIDPMT, r.cfg.ProgramNumber, r.tableVersion, topo.pmt)
	pmtPackets, err := packetizeSection(r.cfg.OutputPMTPID, &r.pmtCC, pmtSection)
	if err != nil {
		return err
	}
	if err := r.writeAll(patPackets); err != nil {
		return err
	}
	return r.writeAll(pmtPackets)
}

// buildSectionBytes serializes a PAT or PMT section with its real section length
// and CRC32, without any pointer field.
func buildSectionBytes(tableID PSITableID, tableIDExt uint16, version uint8, d interface{}) []byte {
	var body bytes.Buffer
	w := astikit.NewBitsWriter(astikit.BitsWriterOptions{Writer: &body})
	switch tableID {
	case PSITableIDPAT:
		writePATSection(w, d.(*PATData))
	case PSITableIDPMT:
		writePMTSection(w, d.(*PMTData))
	}
	sectionLength := body.Len() + psiSyntaxHeaderSize + psiCRC32Size

	var out bytes.Buffer
	bw := astikit.NewBitsWriter(astikit.BitsWriterOptions{Writer: &out})
	b := astikit.NewBitsWriterBatch(bw)
	b.Write(uint8(tableID))
	b.Write(true)  // section_syntax_indicator
	b.Write(false) // private bit
	b.WriteN(uint8(0xff), 2)
	b.WriteN(uint16(sectionLength), 12)
	b.Write(tableIDExt)
	b.WriteN(uint8(0xff), 2) // reserved
	b.WriteN(version, 5)
	b.Write(true) // current_next_indicator
	b.Write(uint8(0))
	b.Write(uint8(0))
	if err := b.Err(); err != nil {
		panic(err) // only programming errors reach this point
	}
	out.Write(body.Bytes())
	b.Write(computeCRC32(out.Bytes()))
	return out.Bytes()
}

// packetizeSection splits one section into 188-byte TS packets: the first carries
// pointer_field 0 and PUSI, continuation packets carry no pointer field. The
// continuity counter of pid is advanced once per emitted packet.
func packetizeSection(pid uint16, cc *uint8, section []byte) ([]byte, error) {
	var out bytes.Buffer
	first := true
	offset := 0
	for first || offset < len(section) {
		payload := []byte{}
		if first {
			payload = append(payload, 0) // pointer_field
		}
		// 188 bytes hold a 4-byte TS header (sync byte + 3 header bytes);
		// the first packet additionally spends one byte on the pointer field.
		payloadCap := MpegTsPacketSize - 4 - len(payload)
		end := offset + payloadCap
		if end > len(section) {
			end = len(section)
		}
		payload = append(payload, section[offset:end]...)
		offset = end

		pkt := &Packet{
			Header: PacketHeader{
				HasPayload:                true,
				PayloadUnitStartIndicator: first,
				PID:                       pid,
				ContinuityCounter:         *cc,
			},
			Payload: payload,
		}
		pw := astikit.NewBitsWriter(astikit.BitsWriterOptions{Writer: &out})
		if _, err := writePacket(pw, pkt, MpegTsPacketSize); err != nil {
			return nil, err
		}
		*cc = (*cc + 1) & 0x0f
		first = false
	}
	return out.Bytes(), nil
}

// copyMedia emits an admitted media packet, rewriting only PID and CC.
func (r *ProgramRemuxer) copyMedia(p *Packet, raw []byte) error {
	id, ok := r.active.identities[p.Header.PID]
	if !ok {
		// Other programs, unknown PIDs and the old input tables are dropped.
		return nil
	}

	standalonePCR := r.active.pcrIdentity == id
	if !standalonePCR {
		// New ES identities wait for a PUSI payload; earlier continuations are lost.
		if !id.started {
			if !p.Header.PayloadUnitStartIndicator || !p.Header.HasPayload {
				return nil
			}
			id.started = true
		}
	} else if !id.started {
		// Adaptation-only PCR packets are valid clock output from the first packet.
		id.started = true
	}

	out := make([]byte, MpegTsPacketSize)
	copy(out, raw)
	// Rewrite the 13-bit PID in header bytes 1-2, preserving PUSI in byte 1.
	out[1] = (out[1] & 0xe0) | byte(id.outputPID>>8)
	out[2] = byte(id.outputPID)
	cc := r.cc[id.outputPID]
	if cc == nil {
		cc = &remuxCC{}
		r.cc[id.outputPID] = cc
	}
	if p.Header.HasPayload {
		// The first payload packet starts at 0; each later payload adds one.
		if cc.seen {
			cc.lastPayload = (cc.lastPayload + 1) & 0x0f
		}
		cc.seen = true
		out[3] = (out[3] & 0xf0) | cc.lastPayload
	} else {
		// Adaptation-only packets reuse the previous payload CC (0 before any).
		out[3] = (out[3] & 0xf0) | cc.lastPayload
	}
	return r.writeAll(out)
}

// serializeDescriptors renders descriptors back to their wire bytes.
func serializeDescriptors(ds []*Descriptor) []byte {
	var buf bytes.Buffer
	w := astikit.NewBitsWriter(astikit.BitsWriterOptions{Writer: &buf})
	writeDescriptors(w, ds)
	return buf.Bytes()
}
