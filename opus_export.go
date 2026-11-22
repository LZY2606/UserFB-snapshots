package mp4

import (
	"errors"
	"fmt"
	"io"
	"math"

	"github.com/abema/go-mp4/internal/oggopus"
)

// opusExportRate is the only supported media timescale and Ogg granule rate.
const opusExportRate uint32 = 48000

// ErrOpusExport is returned for every input rejected by ExportOpusOgg.
var ErrOpusExport = errors.New("mp4: opus export rejected")

func opusExportError(format string, args ...interface{}) error {
	return fmt.Errorf("%w: %s", ErrOpusExport, fmt.Sprintf(format, args...))
}

// opusTrackScan accumulates the boxes of one trak subtree while scanning.
type opusTrackScan struct {
	trackID uint32
	tkhd    *Tkhd
	mdhd    *Mdhd
	elst    *Elst
	stsd    *Stsd
	ase     *AudioSampleEntry
	dops    *DOps
	stts    *Stts
	stsc    *Stsc
	stsz    *Stsz
	stco    *Stco
	co64    *Co64
	dref    *Dref

	opusEntryCount int
	urlEntryCount  int
	urnEntryCount  int
	badURLCount    int

	seen map[BoxType]bool
}

func newOpusTrackScan() *opusTrackScan {
	return &opusTrackScan{seen: make(map[BoxType]bool)}
}

// opusScan is the mutable state of the single ReadBoxStructure pass.
type opusScan struct {
	mvhd       *Mvhd
	mdatBoxes  []*BoxInfo
	fragmented bool
	encrypted  bool
	tracks     []*opusTrackScan
	current    *opusTrackScan
}

func pathHas(path BoxPath, t BoxType) bool {
	for _, p := range path {
		if p == t {
			return true
		}
	}
	return false
}

// ExportOpusOgg exports the Opus audio track identified by trackID from a
// non-fragmented, self-contained, unencrypted MP4 and writes an Ogg Opus
// stream (RFC 7845) to w. The compressed Opus packets are copied verbatim;
// only the container changes and the audible start/end samples do not move.
//
// Parsing always starts at the beginning of r. Whatever the outcome, r is
// seeked back to the position it had when ExportOpusOgg was called. All
// validation finishes before the first byte is written to w, so rejected
// input never produces a partial stream.
func ExportOpusOgg(r io.ReadSeeker, w io.Writer, trackID, serial uint32) (retErr error) {
	origin, err := r.Seek(0, io.SeekCurrent)
	if err != nil {
		return err
	}
	defer func() {
		if _, seekErr := r.Seek(origin, io.SeekStart); seekErr != nil && retErr == nil {
			retErr = seekErr
		}
	}()

	scan := &opusScan{}
	// ReadBoxStructure always starts parsing at offset 0 of r.
	if _, err := ReadBoxStructure(r, func(h *ReadHandle) (interface{}, error) {
		return nil, scan.handle(h)
	}); err != nil {
		return err
	}
	if scan.fragmented {
		return opusExportError("fragmented MP4 is not supported")
	}
	if scan.encrypted {
		return opusExportError("encrypted MP4 is not supported")
	}
	if len(scan.mdatBoxes) == 0 {
		return opusExportError("no mdat box found")
	}
	if scan.mvhd == nil {
		return opusExportError("mvhd box not found")
	}
	if scan.mvhd.Timescale == 0 {
		return opusExportError("movie timescale is zero")
	}

	var track *opusTrackScan
	for _, t := range scan.tracks {
		if t.tkhd != nil && t.tkhd.TrackID == trackID {
			if track != nil {
				return opusExportError("trackID %d is not unique", trackID)
			}
			track = t
		}
	}
	if track == nil {
		return opusExportError("trackID %d not found", trackID)
	}

	plan, head, endGranule, err := track.buildPlan(scan.mvhd, scan.mdatBoxes, r)
	if err != nil {
		return err
	}

	// Everything below this point writes to w. Validation above must have
	// rejected every malformed input before reaching it.
	ow, err := oggopus.NewWriter(w, serial, head)
	if err != nil {
		return err
	}
	if err := ow.WriteTags("go-mp4"); err != nil {
		return err
	}

	// A single fixed-capacity packet workspace; the whole track is never
	// buffered as audio bytes.
	buf := make([]byte, oggopus.MaxPacketSize)
	var granule uint64
	for i, pkt := range plan {
		if _, err := r.Seek(int64(pkt.fileOffset), io.SeekStart); err != nil {
			return err
		}
		if _, err := io.ReadFull(r, buf[:pkt.size]); err != nil {
			return err
		}
		granule += uint64(pkt.tocFrames)
		if granule > math.MaxInt64 {
			return opusExportError("granule position overflow")
		}
		g := int64(granule)
		eos := i == len(plan)-1
		if eos {
			// Only the EOS page is trimmed to the audible edit end.
			g = int64(endGranule)
		}
		if err := ow.WriteAudioPacket(buf[:pkt.size], g, eos); err != nil {
			return err
		}
	}
	return nil
}

func (s *opusScan) handle(h *ReadHandle) error {
	t := h.BoxInfo.Type
	atRoot := len(h.Path) == 1

	// Structural rejection independent of where the box appears.
	switch t {
	case BoxTypeMoof(), BoxTypeStyp(), BoxTypeMvex(), BoxTypeTrex():
		s.fragmented = true
	case BoxTypePssh(), BoxTypeTenc(), BoxTypeSinf(), BoxTypeFrma(),
		BoxTypeSaiz(), BoxTypeSaio(), BoxTypeEnca(), BoxTypeEncv():
		s.encrypted = true
	}
	if atRoot && t == BoxTypeMdat() {
		s.mdatBoxes = append(s.mdatBoxes, &h.BoxInfo)
	}
	if t == BoxTypeMvhd() && pathHas(h.Path, BoxTypeMoov()) {
		box, _, err := h.ReadPayload()
		if err != nil {
			return err
		}
		s.mvhd = box.(*Mvhd)
		return nil
	}
	if t == BoxTypeTrak() && pathHas(h.Path, BoxTypeMoov()) {
		s.current = newOpusTrackScan()
		s.tracks = append(s.tracks, s.current)
		if _, err := h.Expand(); err != nil {
			return err
		}
		s.current = nil
		return nil
	}

	if s.current == nil {
		// Only moov needs recursive scanning outside of a trak subtree.
		if atRoot && t == BoxTypeMoov() {
			_, err := h.Expand()
			return err
		}
		return nil
	}

	cur := s.current
	switch t {
	case BoxTypeTkhd(), BoxTypeMdhd(), BoxTypeElst(), BoxTypeDOps():
		if cur.seen[t] {
			return opusExportError("duplicate %q box in track %d", t.String(), cur.trackID)
		}
		cur.seen[t] = true
		box, _, err := h.ReadPayload()
		if err != nil {
			return err
		}
		switch b := box.(type) {
		case *Tkhd:
			cur.tkhd = b
		case *Mdhd:
			cur.mdhd = b
		case *Elst:
			cur.elst = b
		case *DOps:
			cur.dops = b
		}
		return nil
	case BoxTypeStts(), BoxTypeStsc(), BoxTypeStsz(), BoxTypeStco(), BoxTypeCo64():
		if cur.seen[t] {
			return opusExportError("duplicate %q box in track %d", t.String(), cur.trackID)
		}
		cur.seen[t] = true
		box, _, err := h.ReadPayload()
		if err != nil {
			return err
		}
		switch b := box.(type) {
		case *Stts:
			cur.stts = b
		case *Stsc:
			cur.stsc = b
		case *Stsz:
			cur.stsz = b
		case *Stco:
			cur.stco = b
		case *Co64:
			cur.co64 = b
		}
		return nil
	case BoxTypeDref():
		if cur.seen[t] {
			return opusExportError("duplicate %q box in track %d", t.String(), cur.trackID)
		}
		cur.seen[t] = true
		box, _, err := h.ReadPayload()
		if err != nil {
			return err
		}
		cur.dref = box.(*Dref)
		if _, err := h.Expand(); err != nil {
			return err
		}
		return nil
	case BoxTypeCtts():
		return opusExportError("ctts box is not supported")
	case BoxTypeStsd():
		if cur.seen[t] {
			return opusExportError("duplicate stsd box in track %d", cur.trackID)
		}
		cur.seen[t] = true
		box, _, err := h.ReadPayload()
		if err != nil {
			return err
		}
		cur.stsd = box.(*Stsd)
		if _, err := h.Expand(); err != nil {
			return err
		}
		return nil
	case BoxTypeOpus():
		box, _, err := h.ReadPayload()
		if err != nil {
			return err
		}
		cur.opusEntryCount++
		cur.ase = box.(*AudioSampleEntry)
		if _, err := h.Expand(); err != nil {
			return err
		}
		return nil
	case BoxTypeUrl():
		box, _, err := h.ReadPayload()
		if err != nil {
			return err
		}
		cur.urlEntryCount++
		if !box.(*Url).CheckFlag(UrlSelfContained) {
			cur.badURLCount++
		}
		return nil
	case BoxTypeUrn():
		if _, _, err := h.ReadPayload(); err != nil {
			return err
		}
		cur.urnEntryCount++
		return nil
	case BoxTypeMdia(), BoxTypeMinf(), BoxTypeStbl(), BoxTypeEdts(), BoxTypeDinf():
		_, err := h.Expand()
		return err
	default:
		return nil
	}
}

// opusPacketPlan is the validated per-sample information needed in the
// streaming copy phase. mediaStart is the packet's position in 48 kHz media
// time (used for edit-window selection); fileOffset is its byte position in
// the MP4 (used to copy the packet verbatim).
type opusPacketPlan struct {
	mediaStart uint64
	fileOffset uint64
	size       uint32
	tocFrames  uint32
}

// buildPlan runs every table, timing, range and TOC check. It performs no
// writes and returns the full packet plan plus the Ogg header parameters.
func (ts *opusTrackScan) buildPlan(mvhd *Mvhd, mdats []*BoxInfo, r io.ReadSeeker) ([]*opusPacketPlan, *oggopus.Head, uint64, error) {
	if ts.tkhd == nil {
		return nil, nil, 0, opusExportError("tkhd box not found")
	}
	ts.trackID = ts.tkhd.TrackID
	if ts.mdhd == nil {
		return nil, nil, 0, opusExportError("track %d: mdhd box not found", ts.trackID)
	}
	if ts.mdhd.Timescale != opusExportRate {
		return nil, nil, 0, opusExportError("track %d: mdhd timescale must be %d, got %d", ts.trackID, opusExportRate, ts.mdhd.Timescale)
	}
	if ts.stsd == nil || ts.stsd.EntryCount != 1 || ts.opusEntryCount != 1 || ts.ase == nil {
		return nil, nil, 0, opusExportError("track %d: exactly one Opus sample description is required", ts.trackID)
	}
	if ts.dops == nil {
		return nil, nil, 0, opusExportError("track %d: dOps box not found", ts.trackID)
	}
	if err := ts.validateDOps(); err != nil {
		return nil, nil, 0, err
	}
	if ts.dref == nil {
		return nil, nil, 0, opusExportError("track %d: dref box not found", ts.trackID)
	}
	if ts.urnEntryCount != 0 || ts.badURLCount != 0 ||
		ts.dref.EntryCount != uint32(ts.urlEntryCount) || ts.dref.EntryCount == 0 {
		return nil, nil, 0, opusExportError("track %d: only self-contained data references are supported", ts.trackID)
	}
	if ts.ase.DataReferenceIndex < 1 || uint32(ts.ase.DataReferenceIndex) > ts.dref.EntryCount {
		return nil, nil, 0, opusExportError("track %d: invalid data reference index %d", ts.trackID, ts.ase.DataReferenceIndex)
	}
	if ts.ase.ChannelCount != uint16(ts.dops.OutputChannelCount) {
		return nil, nil, 0, opusExportError("track %d: sample entry channel count %d != dOps channel count %d",
			ts.trackID, ts.ase.ChannelCount, ts.dops.OutputChannelCount)
	}
	if ts.ase.SampleRate != opusExportRate<<16 {
		return nil, nil, 0, opusExportError("track %d: sample entry rate must be %d fixed-point", ts.trackID, opusExportRate)
	}
	if ts.stts == nil || ts.stsc == nil || ts.stsz == nil {
		return nil, nil, 0, opusExportError("track %d: stts/stsc/stsz boxes are required", ts.trackID)
	}
	if (ts.stco == nil) == (ts.co64 == nil) {
		return nil, nil, 0, opusExportError("track %d: exactly one of stco/co64 is required", ts.trackID)
	}

	S, E, err := ts.editWindow(mvhd)
	if err != nil {
		return nil, nil, 0, err
	}

	chunkCount, chunkOffsets, err := ts.chunkOffsets()
	if err != nil {
		return nil, nil, 0, err
	}
	samplesPerChunk, err := expandSampleToChunk(ts.stsc, chunkCount, ts.trackID)
	if err != nil {
		return nil, nil, 0, err
	}
	sampleCount, err := sumSamplesPerChunk(samplesPerChunk)
	if err != nil {
		return nil, nil, 0, err
	}
	if sampleCount == 0 {
		return nil, nil, 0, opusExportError("track %d: no samples", ts.trackID)
	}

	sizes, err := ts.sampleSizes(sampleCount)
	if err != nil {
		return nil, nil, 0, err
	}
	deltas, sttsTotal, err := expandStts(ts.stts, sampleCount, ts.trackID)
	if err != nil {
		return nil, nil, 0, err
	}
	if mdhdDur := ts.mdhd.GetDuration(); mdhdDur != sttsTotal {
		return nil, nil, 0, opusExportError("track %d: mdhd duration %d != stts total %d", ts.trackID, mdhdDur, sttsTotal)
	}
	if E > sttsTotal {
		return nil, nil, 0, opusExportError("track %d: edit end %d exceeds media duration %d", ts.trackID, E, sttsTotal)
	}
	if E > math.MaxInt64 {
		return nil, nil, 0, opusExportError("track %d: edit end %d overflows the Ogg granule field", ts.trackID, E)
	}

	plan, err := ts.buildPacketPlan(sampleCount, samplesPerChunk, chunkOffsets, sizes, deltas, mdats, r)
	if err != nil {
		return nil, nil, 0, err
	}

	// Keep packets from the first one up to and including the first packet
	// whose decode span covers E. Earlier packets preserve decoder history;
	// whole packets starting at or after E are dropped.
	last := -1
	var tocTotal uint64
	for i, pkt := range plan {
		if pkt.mediaStart >= E {
			break
		}
		last = i
		tocTotal += uint64(pkt.tocFrames)
	}
	if last < 0 {
		return nil, nil, 0, opusExportError("track %d: edit window does not intersect any packet", ts.trackID)
	}
	if tocTotal < E {
		return nil, nil, 0, opusExportError("track %d: packet stream ends before edit end %d", ts.trackID, E)
	}

	head := &oggopus.Head{
		Channels:        ts.dops.OutputChannelCount,
		PreSkip:         uint16(S),
		InputSampleRate: ts.dops.InputSampleRate,
		OutputGain:      ts.dops.OutputGain,
	}
	return plan[:last+1], head, E, nil
}

func (ts *opusTrackScan) validateDOps() error {
	d := ts.dops
	if d.Version != 0 {
		return opusExportError("track %d: dOps version must be 0, got %d", ts.trackID, d.Version)
	}
	if d.ChannelMappingFamily != 0 {
		return opusExportError("track %d: channel mapping family %d is not supported", ts.trackID, d.ChannelMappingFamily)
	}
	if d.OutputChannelCount != 1 && d.OutputChannelCount != 2 {
		return opusExportError("track %d: unsupported channel count %d", ts.trackID, d.OutputChannelCount)
	}
	if len(d.ChannelMapping) != 0 || d.StreamCount != 0 || d.CoupledCount != 0 {
		return opusExportError("track %d: mapping family 0 must not carry channel mapping fields", ts.trackID)
	}
	return nil
}

// editWindow resolves the single rate-1 edit list entry to the half-open
// 48 kHz media window [S, E). dOps.PreSkip is never added to S.
func (ts *opusTrackScan) editWindow(mvhd *Mvhd) (uint64, uint64, error) {
	if ts.elst == nil || ts.elst.EntryCount != 1 || len(ts.elst.Entries) != 1 {
		return 0, 0, opusExportError("track %d: exactly one edit list entry is required", ts.trackID)
	}
	entry := ts.elst.Entries[0]
	if entry.MediaRateInteger != 1 || entry.MediaRateFraction != 0 {
		return 0, 0, opusExportError("track %d: edit rate must be 1.0", ts.trackID)
	}
	mediaTime := ts.elst.GetMediaTime(0)
	if mediaTime < 0 {
		return 0, 0, opusExportError("track %d: empty edit (negative media_time) is not supported", ts.trackID)
	}
	segmentDuration := ts.elst.GetSegmentDuration(0)
	if segmentDuration == 0 {
		return 0, 0, opusExportError("track %d: edit segment_duration must be positive", ts.trackID)
	}
	// segment_duration is expressed in the movie timescale, which may differ
	// from 48 kHz. Convert with exact integer arithmetic, never with floats.
	const rate = uint64(opusExportRate)
	if segmentDuration > math.MaxUint64/rate {
		return 0, 0, opusExportError("track %d: segment_duration overflow", ts.trackID)
	}
	numerator := segmentDuration * rate
	timescale := uint64(mvhd.Timescale)
	if numerator%timescale != 0 {
		return 0, 0, opusExportError("track %d: segment_duration %d does not map exactly to 48k frames", ts.trackID, segmentDuration)
	}
	windowLen := numerator / timescale
	if windowLen == 0 {
		return 0, 0, opusExportError("track %d: edit window is empty at 48 kHz", ts.trackID)
	}
	S := uint64(mediaTime)
	if S > 0xffff {
		return 0, 0, opusExportError("track %d: edit start %d does not fit in OpusHead pre_skip", ts.trackID, S)
	}
	if S > math.MaxUint64-windowLen {
		return 0, 0, opusExportError("track %d: edit end overflow", ts.trackID)
	}
	return S, S + windowLen, nil
}

func (ts *opusTrackScan) chunkOffsets() (uint32, []uint64, error) {
	switch {
	case ts.stco != nil:
		if ts.stco.EntryCount != uint32(len(ts.stco.ChunkOffset)) {
			return 0, nil, opusExportError("track %d: stco entry count mismatch", ts.trackID)
		}
		offsets := make([]uint64, len(ts.stco.ChunkOffset))
		for i, off := range ts.stco.ChunkOffset {
			offsets[i] = uint64(off)
		}
		return ts.stco.EntryCount, offsets, nil
	default:
		if ts.co64.EntryCount != uint32(len(ts.co64.ChunkOffset)) {
			return 0, nil, opusExportError("track %d: co64 entry count mismatch", ts.trackID)
		}
		return ts.co64.EntryCount, append([]uint64(nil), ts.co64.ChunkOffset...), nil
	}
}

// expandSampleToChunk resolves stsc into a samples-per-chunk table. Decode
// order follows chunk order; physical file addresses never reorder it.
func expandSampleToChunk(stsc *Stsc, chunkCount uint32, trackID uint32) ([]uint32, error) {
	if stsc.EntryCount == 0 || uint32(len(stsc.Entries)) != stsc.EntryCount {
		return nil, opusExportError("track %d: empty or corrupt stsc", trackID)
	}
	if chunkCount == 0 {
		return nil, opusExportError("track %d: no chunks", trackID)
	}
	out := make([]uint32, chunkCount)
	for i, e := range stsc.Entries {
		if e.FirstChunk == 0 || e.FirstChunk > chunkCount {
			return nil, opusExportError("track %d: stsc first_chunk %d out of range", trackID, e.FirstChunk)
		}
		if e.SamplesPerChunk == 0 {
			return nil, opusExportError("track %d: stsc samples_per_chunk must be positive", trackID)
		}
		if e.SampleDescriptionIndex != 1 {
			return nil, opusExportError("track %d: unsupported sample description index %d", trackID, e.SampleDescriptionIndex)
		}
		if i > 0 && e.FirstChunk <= stsc.Entries[i-1].FirstChunk {
			return nil, opusExportError("track %d: stsc first_chunk values must increase", trackID)
		}
		end := chunkCount
		if i+1 < len(stsc.Entries) {
			next := stsc.Entries[i+1].FirstChunk
			if next <= e.FirstChunk {
				return nil, opusExportError("track %d: stsc first_chunk values must increase", trackID)
			}
			end = next - 1
		}
		for c := e.FirstChunk - 1; c < end; c++ {
			out[c] = e.SamplesPerChunk
		}
	}
	for c, n := range out {
		if n == 0 {
			return nil, opusExportError("track %d: stsc does not cover chunk %d", trackID, c+1)
		}
	}
	return out, nil
}

func sumSamplesPerChunk(samplesPerChunk []uint32) (uint32, error) {
	var total uint64
	for _, n := range samplesPerChunk {
		total += uint64(n)
		if total > math.MaxUint32 {
			return 0, opusExportError("sample count overflow")
		}
	}
	return uint32(total), nil
}

func (ts *opusTrackScan) sampleSizes(sampleCount uint32) ([]uint32, error) {
	sizes := make([]uint32, sampleCount)
	switch {
	case ts.stsz.SampleSize != 0:
		if ts.stsz.SampleCount != sampleCount || len(ts.stsz.EntrySize) != 0 {
			return nil, opusExportError("track %d: stsz sample count mismatch", ts.trackID)
		}
		for i := range sizes {
			sizes[i] = ts.stsz.SampleSize
		}
	default:
		if ts.stsz.SampleCount != sampleCount || uint32(len(ts.stsz.EntrySize)) != sampleCount {
			return nil, opusExportError("track %d: stsz sample count mismatch", ts.trackID)
		}
		copy(sizes, ts.stsz.EntrySize)
	}
	for i, size := range sizes {
		if size < 1 || size > oggopus.MaxPacketSize {
			return nil, opusExportError("track %d: sample %d size %d out of range [1,%d]", ts.trackID, i, size, oggopus.MaxPacketSize)
		}
	}
	return sizes, nil
}

func expandStts(stts *Stts, sampleCount uint32, trackID uint32) ([]uint32, uint64, error) {
	deltas := make([]uint32, sampleCount)
	var idx uint32
	var total uint64
	for _, e := range stts.Entries {
		if e.SampleCount == 0 || e.SampleDelta == 0 {
			return nil, 0, opusExportError("track %d: stts entries must be positive", trackID)
		}
		if uint64(idx)+uint64(e.SampleCount) > uint64(sampleCount) {
			return nil, 0, opusExportError("track %d: stts sample count mismatch", trackID)
		}
		for k := uint32(0); k < e.SampleCount; k++ {
			deltas[idx] = e.SampleDelta
			idx++
		}
		if uint64(e.SampleCount) > math.MaxUint64/uint64(e.SampleDelta) ||
			total > math.MaxUint64-uint64(e.SampleCount)*uint64(e.SampleDelta) {
			return nil, 0, opusExportError("track %d: stts duration overflow", trackID)
		}
		total += uint64(e.SampleCount) * uint64(e.SampleDelta)
	}
	if idx != sampleCount || stts.EntryCount != uint32(len(stts.Entries)) {
		return nil, 0, opusExportError("track %d: stts sample count mismatch", trackID)
	}
	return deltas, total, nil
}

// buildPacketPlan walks chunks in decode order, verifies every sample byte
// range against the mdat boxes and independently checks each TOC duration
// against stts. mediaStart records the media-time (48k sample) coordinate
// where the packet begins, which is what the edit window is expressed in.
func (ts *opusTrackScan) buildPacketPlan(sampleCount uint32, samplesPerChunk []uint32,
	chunkOffsets []uint64, sizes, deltas []uint32, mdats []*BoxInfo, r io.ReadSeeker,
) ([]*opusPacketPlan, error) {
	plan := make([]*opusPacketPlan, 0, sampleCount)
	var sampleIdx uint32
	var mediaStart uint64
	for chunkIdx, n := range samplesPerChunk {
		cursor := chunkOffsets[chunkIdx]
		for k := uint32(0); k < n; k++ {
			size := sizes[sampleIdx]
			fileOffset := cursor
			end, overflow := addOverflow(fileOffset, uint64(size))
			if overflow {
				return nil, opusExportError("track %d: sample %d offset overflow", ts.trackID, sampleIdx)
			}
			if err := checkMdatRange(fileOffset, end, mdats, ts.trackID, sampleIdx); err != nil {
				return nil, err
			}
			tocFrames, err := readTocFrames(r, fileOffset, size, ts.trackID, sampleIdx)
			if err != nil {
				return nil, err
			}
			if err := checkDelta(deltas[sampleIdx], tocFrames, sampleIdx == sampleCount-1, ts.trackID, sampleIdx); err != nil {
				return nil, err
			}
			plan = append(plan, &opusPacketPlan{
				mediaStart: mediaStart,
				fileOffset: fileOffset,
				size:       size,
				tocFrames:  tocFrames,
			})
			if mediaStart > math.MaxUint64-uint64(tocFrames) {
				return nil, opusExportError("track %d: media time overflow", ts.trackID)
			}
			mediaStart += uint64(tocFrames)
			cursor = end
			sampleIdx++
		}
	}
	return plan, nil
}

func addOverflow(a, b uint64) (uint64, bool) {
	if a > math.MaxUint64-b {
		return 0, true
	}
	return a + b, false
}

func checkMdatRange(offset, end uint64, mdats []*BoxInfo, trackID, sampleIdx uint32) error {
	for _, m := range mdats {
		start, overflow := addOverflow(m.Offset, m.HeaderSize)
		if overflow {
			continue
		}
		stop, overflow := addOverflow(m.Offset, m.Size)
		if overflow {
			continue
		}
		if offset >= start && end <= stop {
			return nil
		}
	}
	return opusExportError("track %d: sample %d bytes [%d,%d) are outside every mdat", trackID, sampleIdx, offset, end)
}

func checkDelta(delta, tocFrames uint32, isLast bool, trackID, sampleIdx uint32) error {
	switch {
	case isLast && delta > 0 && delta <= tocFrames:
		return nil
	case delta == tocFrames:
		return nil
	default:
		return opusExportError("track %d: sample %d stts delta %d != TOC duration %d", trackID, sampleIdx, delta, tocFrames)
	}
}

// tocFrameDuration returns the per-frame duration in 48 kHz samples for a
// TOC configuration, per RFC 6716 section 3.
func tocFrameDuration(config int) uint32 {
	switch {
	case config < 12:
		return [4]uint32{480, 960, 1920, 2880}[config%4]
	case config < 16:
		return [2]uint32{480, 960}[config%2]
	default:
		return [4]uint32{120, 240, 480, 960}[config%4]
	}
}

// readTocFrames parses just the TOC bytes of an Opus packet (RFC 6716) and
// returns the total number of 48 kHz frames. The compressed payload itself
// is neither decoded nor rewritten.
func readTocFrames(r io.ReadSeeker, offset uint64, size, trackID, sampleIdx uint32) (uint32, error) {
	var hdr [2]byte
	if _, err := r.Seek(int64(offset), io.SeekStart); err != nil {
		return 0, err
	}
	if size >= 2 {
		if _, err := io.ReadFull(r, hdr[:]); err != nil {
			return 0, err
		}
	} else {
		if _, err := io.ReadFull(r, hdr[:1]); err != nil {
			return 0, err
		}
	}
	config := int(hdr[0] >> 3)
	code := hdr[0] & 0x03
	frameDur := tocFrameDuration(config)

	var frameCount uint32
	switch code {
	case 0:
		frameCount = 1
	case 1, 2:
		frameCount = 2
	case 3:
		if size < 2 {
			return 0, opusExportError("track %d: sample %d TOC code 3 requires a frame-count byte", trackID, sampleIdx)
		}
		frameCount = uint32(hdr[1] & 0x3f)
		if frameCount == 0 {
			return 0, opusExportError("track %d: sample %d TOC code 3 frame count is zero", trackID, sampleIdx)
		}
	}
	total := frameCount * frameDur
	if total > 5760 {
		return 0, opusExportError("track %d: sample %d TOC duration %d exceeds 5760 frames", trackID, sampleIdx, total)
	}
	return total, nil
}
