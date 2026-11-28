package mp4

import (
	"errors"
	"fmt"
	"io"
	"math"

	"github.com/abema/go-mp4/internal/oggopus"
)

// Opus-in-MP4/OGG export constraints.
const (
	opusSampleRate   = 48000
	opusMaxTOCFrames = 5760
)

// opusFrameDurations maps an RFC 6716 TOC configuration to the duration of
// one frame in 48 kHz units. Configurations 0..11 use the 2.5/5/10/20 ms
// family, 12..15 the 2.5/5 ms family and 16..31 the sub-2.5 ms family.
var opusFrameDurations = [32]uint32{
	480, 960, 1920, 2880, // 0..3
	480, 960, 1920, 2880, // 4..7
	480, 960, 1920, 2880, // 8..11
	480, 960, 480, 960, // 12..15
	120, 240, 480, 960, // 16..19
	120, 240, 480, 960, // 20..23
	120, 240, 480, 960, // 24..27
	120, 240, 480, 960, // 28..31
}

var (
	errExportFragmented  = errors.New("mp4: fragmented MP4 is not supported by Opus export")
	errExportEncrypted   = errors.New("mp4: encrypted content is not supported by Opus export")
	errExportUnsupported = errors.New("mp4: unsupported box for Opus export")
)

// ExportOpusOgg exports the Opus audio track selected by trackID from a
// non-fragmented, self-contained, unencrypted MP4 and writes an Ogg Opus
// stream (RFC 7845) to w. Compressed Opus packets are copied byte-for-byte;
// only the container changes.
//
// The single rate-1 edit list entry defines the audible half-open window
// [S,E) in the track's 48 kHz media timeline. Packets are kept in decode
// order from the first packet up to and including the first packet covering
// E, so decoder history is preserved. The OpusHead pre_skip field is set to
// S; dOps.PreSkip is informative only and never added to S.
//
// All structural, table, range, TOC-duration and window checks run before
// the first byte is written to w: invalid input never yields a partial Ogg
// stream. Read/write errors during the copying phase are returned as-is and
// already written pages are not rolled back. r is always seeked back to its
// position on entry, whether the export succeeds or fails.
func ExportOpusOgg(r io.ReadSeeker, w io.Writer, trackID, serial uint32) (err error) {
	origin, serr := r.Seek(0, io.SeekCurrent)
	if serr != nil {
		return serr
	}
	defer func() {
		if _, e := r.Seek(origin, io.SeekStart); e != nil && err == nil {
			err = e
		}
	}()

	plan, err := buildOpusExportPlan(r, trackID)
	if err != nil {
		return err
	}
	return plan.write(r, w, serial)
}

// opusExportPlan holds everything needed to emit the Ogg stream. The audio
// payload itself is never accumulated: only table-sized metadata is kept.
type opusExportPlan struct {
	head      oggopus.Head
	chunks    []opusChunkPlan
	sizes     []uint32 // stsz-derived packet size per sample, decode order
	endSample uint32   // index (0-based) of the first packet covering E
	endPoint  uint64   // E in 48 kHz media units
}

type opusChunkPlan struct {
	dataOffset      uint64
	samplesPerChunk uint32
}

type mdatRange struct {
	start uint64 // first payload byte
	end   uint64 // one past the last payload byte
}

type opusTrackBoxes struct {
	tkhd     *Tkhd
	mdhd     *Mdhd
	hdlr     *Hdlr
	elst     *Elst
	stsd     *Stsd
	opus     *AudioSampleEntry
	dops     *DOps
	dref     *Dref
	stts     *Stts
	stsc     *Stsc
	stsz     *Stsz
	stco     *Stco
	co64     *Co64
	opusSeen int
	dopsSeen int
}

func buildOpusExportPlan(r io.ReadSeeker, trackID uint32) (*opusExportPlan, error) {
	// Phase 1: scan top-level boxes. Fragmented files and unknown layouts
	// are rejected up front.
	var moovBI *BoxInfo
	var mdats []mdatRange
	handler := func(h *ReadHandle) (interface{}, error) {
		switch h.BoxInfo.Type {
		case BoxTypeFtyp(), BoxTypeFree(), BoxTypeSkip(), BoxTypeUdta():
			return nil, nil
		case BoxTypeMoov():
			moovBI = &h.BoxInfo
			return nil, nil
		case BoxTypeMdat():
			if err := checkPayloadSize(r, &h.BoxInfo); err != nil {
				return nil, err
			}
			mdats = append(mdats, mdatRange{
				start: h.BoxInfo.Offset + h.BoxInfo.HeaderSize,
				end:   h.BoxInfo.Offset + h.BoxInfo.Size,
			})
			return nil, nil
		case BoxTypeMoof(), BoxTypeStyp():
			return nil, errExportFragmented
		case BoxTypeMvex():
			return nil, errExportFragmented
		default:
			return nil, fmt.Errorf("%w: top-level box %s", errExportUnsupported, h.BoxInfo.Type.String())
		}
	}
	if _, err := ReadBoxStructure(r, handler); err != nil {
		return nil, err
	}
	if moovBI == nil {
		return nil, errors.New("mp4: moov box not found")
	}
	if len(mdats) == 0 {
		return nil, errors.New("mp4: mdat box not found")
	}

	// Phase 2: inspect moov children. mvex marks a fragmented movie.
	var mvhd *Mvhd
	var trakBIs []*BoxInfo
	moovHandler := func(h *ReadHandle) (interface{}, error) {
		switch h.BoxInfo.Type {
		case BoxTypeMoov():
			return h.Expand()
		case BoxTypeMvhd():
			box, _, err := h.ReadPayload()
			if err != nil {
				return nil, err
			}
			mvhd = box.(*Mvhd)
			return nil, nil
		case BoxTypeTrak():
			trakBIs = append(trakBIs, &h.BoxInfo)
			return nil, nil
		case BoxTypeUdta():
			return nil, nil // opaque, ordinary user data
		case BoxTypeMvex():
			return nil, errExportFragmented
		case BoxTypeMoof():
			return nil, errExportFragmented
		default:
			return nil, fmt.Errorf("%w: moov child box %s", errExportUnsupported, h.BoxInfo.Type.String())
		}
	}
	if _, err := ReadBoxStructureFromInternal(r, moovBI, moovHandler); err != nil {
		return nil, err
	}
	if mvhd == nil {
		return nil, errors.New("mp4: mvhd box not found")
	}
	if len(trakBIs) == 0 {
		return nil, errors.New("mp4: trak box not found")
	}

	// Phase 3: find the unique track with the requested track ID.
	var selected *BoxInfo
	matches := 0
	for _, trakBI := range trakBIs {
		bips, err := ExtractBoxesWithPayload(r, trakBI, []BoxPath{
			{BoxTypeTkhd()},
		})
		if err != nil {
			return nil, err
		}
		if len(bips) != 1 {
			return nil, errors.New("mp4: invalid tkhd box count")
		}
		if bips[0].Payload.(*Tkhd).TrackID == trackID {
			selected = trakBI
			matches++
		}
	}
	if matches == 0 {
		return nil, fmt.Errorf("mp4: trackID %d not found", trackID)
	}
	if matches > 1 {
		return nil, fmt.Errorf("mp4: trackID %d is not unique", trackID)
	}

	boxes, err := scanOpusTrack(r, selected)
	if err != nil {
		return nil, err
	}
	return validateOpusTrack(r, boxes, mvhd, mdats)
}

// scanOpusTrack walks the selected trak in full, collecting payloads of the
// boxes the exporter needs and rejecting fragmentation, encryption and
// unsupported structures explicitly.
func scanOpusTrack(r io.ReadSeeker, trakBI *BoxInfo) (*opusTrackBoxes, error) {
	boxes := &opusTrackBoxes{}
	expand := map[BoxType]bool{
		BoxTypeTrak(): true,
		BoxTypeEdts(): true,
		BoxTypeMdia(): true,
		BoxTypeMinf(): true,
		BoxTypeStbl(): true,
		BoxTypeStsd(): true,
		BoxTypeDinf(): true,
		BoxTypeDref(): true,
		BoxTypeOpus(): true,
	}
	forbidden := map[BoxType]struct {
		err error
		msg string
	}{
		BoxTypeCtts(): {errExportUnsupported, "ctts box is not supported"},
		BoxTypeSinf(): {errExportEncrypted, "encrypted sample entries are not supported"},
		BoxTypeSaiz(): {errExportEncrypted, "encrypted sample entries are not supported"},
		BoxTypeSaio(): {errExportEncrypted, "encrypted sample entries are not supported"},
		BoxTypeEnca(): {errExportEncrypted, "encrypted sample entries are not supported"},
		BoxTypeEncv(): {errExportEncrypted, "encrypted sample entries are not supported"},
		BoxTypeMoof(): {errExportFragmented, "fragmented tracks are not supported"},
		BoxTypeMvex(): {errExportFragmented, "fragmented tracks are not supported"},
	}
	handler := func(h *ReadHandle) (interface{}, error) {
		t := h.BoxInfo.Type
		if spec, ok := forbidden[t]; ok {
			return nil, fmt.Errorf("%w: %s", spec.err, spec.msg)
		}
		if t == BoxTypeUdta() {
			return nil, nil // opaque user data, do not descend
		}

		read := func() (IBox, error) {
			box, _, err := h.ReadPayload()
			return box, err
		}
		switch t {
		case BoxTypeTkhd():
			box, err := read()
			if err != nil {
				return nil, err
			}
			boxes.tkhd = box.(*Tkhd)
		case BoxTypeMdhd():
			box, err := read()
			if err != nil {
				return nil, err
			}
			boxes.mdhd = box.(*Mdhd)
		case BoxTypeHdlr():
			box, err := read()
			if err != nil {
				return nil, err
			}
			boxes.hdlr = box.(*Hdlr)
		case BoxTypeSmhd():
			// expected for an audio track; payload is not needed
		case BoxTypeElst():
			box, err := read()
			if err != nil {
				return nil, err
			}
			boxes.elst = box.(*Elst)
		case BoxTypeStsd():
			box, err := read()
			if err != nil {
				return nil, err
			}
			boxes.stsd = box.(*Stsd)
		case BoxTypeOpus():
			box, err := read()
			if err != nil {
				return nil, err
			}
			boxes.opus = box.(*AudioSampleEntry)
			boxes.opusSeen++
		case BoxTypeDOps():
			box, err := read()
			if err != nil {
				return nil, err
			}
			boxes.dops = box.(*DOps)
			boxes.dopsSeen++
		case BoxTypeDref():
			box, err := read()
			if err != nil {
				return nil, err
			}
			boxes.dref = box.(*Dref)
		case BoxTypeUrl():
			box, err := read()
			if err != nil {
				return nil, err
			}
			url := box.(*Url)
			if !url.CheckFlag(UrlSelfContained) || url.Location != "" {
				return nil, errors.New("mp4: non-local data references are not supported")
			}
		case BoxTypeUrn():
			return nil, errors.New("mp4: non-local data references are not supported")
		case BoxTypeStts():
			box, err := read()
			if err != nil {
				return nil, err
			}
			boxes.stts = box.(*Stts)
		case BoxTypeStsc():
			box, err := read()
			if err != nil {
				return nil, err
			}
			boxes.stsc = box.(*Stsc)
		case BoxTypeStsz():
			box, err := read()
			if err != nil {
				return nil, err
			}
			boxes.stsz = box.(*Stsz)
		case BoxTypeStco():
			box, err := read()
			if err != nil {
				return nil, err
			}
			boxes.stco = box.(*Stco)
		case BoxTypeCo64():
			box, err := read()
			if err != nil {
				return nil, err
			}
			boxes.co64 = box.(*Co64)
		default:
			if expand[t] {
				return h.Expand()
			}
			return nil, fmt.Errorf("%w: trak box %s", errExportUnsupported, t.String())
		}
		if expand[t] {
			return h.Expand()
		}
		return nil, nil
	}
	if _, err := ReadBoxStructureFromInternal(r, trakBI, handler); err != nil {
		return nil, err
	}
	return boxes, nil
}

func validateOpusTrack(r io.ReadSeeker, b *opusTrackBoxes, mvhd *Mvhd, mdats []mdatRange) (*opusExportPlan, error) {
	if b.tkhd == nil || b.mdhd == nil || b.hdlr == nil || b.stsd == nil ||
		b.stts == nil || b.stsc == nil || b.stsz == nil {
		return nil, errors.New("mp4: required Opus track boxes are missing")
	}
	if b.hdlr.HandlerType != [4]byte{'s', 'o', 'u', 'n'} {
		return nil, errors.New("mp4: selected track is not an audio track")
	}
	if b.opusSeen != 1 || b.opus == nil || b.dopsSeen != 1 || b.dops == nil {
		return nil, errors.New("mp4: exactly one Opus sample description with dOps is required")
	}
	if b.stsd.EntryCount != 1 {
		return nil, errors.New("mp4: multiple sample descriptions are not supported")
	}
	if b.stco == nil && b.co64 == nil {
		return nil, errors.New("mp4: stco/co64 box not found")
	}
	if b.stco != nil && b.co64 != nil {
		return nil, errors.New("mp4: both stco and co64 boxes exist")
	}
	// A missing dref implies the implicit self-contained local reference;
	// explicit url/urn entries were validated while scanning the track.
	if b.dref != nil && b.dref.EntryCount != 1 {
		return nil, errors.New("mp4: exactly one self-contained data reference is supported")
	}

	// dOps and sample entry constraints.
	dops := b.dops
	if dops.Version != 0 {
		return nil, fmt.Errorf("mp4: unsupported dOps version %d", dops.Version)
	}
	if dops.ChannelMappingFamily != 0 {
		return nil, fmt.Errorf("mp4: unsupported Opus channel mapping family %d", dops.ChannelMappingFamily)
	}
	if dops.OutputChannelCount != 1 && dops.OutputChannelCount != 2 {
		return nil, fmt.Errorf("mp4: unsupported Opus channel count %d", dops.OutputChannelCount)
	}
	if len(dops.ChannelMapping) != 0 || dops.StreamCount != 0 || dops.CoupledCount != 0 {
		return nil, errors.New("mp4: mapping family 0 must not carry channel mapping data")
	}
	if uint16(dops.OutputChannelCount) != b.opus.ChannelCount {
		return nil, errors.New("mp4: sample entry channel count does not match dOps")
	}
	if b.opus.SampleRate != opusSampleRate<<16 {
		return nil, fmt.Errorf("mp4: Opus sample entry rate must be %d fixed-point", opusSampleRate)
	}
	if b.opus.SampleEntry.DataReferenceIndex != 1 {
		return nil, errors.New("mp4: unsupported data reference index")
	}

	// Media timescale and edit list.
	if b.mdhd.Timescale != opusSampleRate {
		return nil, fmt.Errorf("mp4: Opus track mdhd timescale must be %d, got %d", opusSampleRate, b.mdhd.Timescale)
	}
	if b.elst == nil {
		return nil, errors.New("mp4: exactly one edit list entry is required")
	}
	if b.elst.EntryCount != 1 || len(b.elst.Entries) != 1 {
		return nil, errors.New("mp4: exactly one edit list entry is required")
	}
	entry := b.elst.Entries[0]
	mediaTime := b.elst.GetMediaTime(0)
	if mediaTime < 0 {
		return nil, errors.New("mp4: empty edit (negative media_time) is not supported")
	}
	segmentDuration := b.elst.GetSegmentDuration(0)
	if segmentDuration == 0 {
		return nil, errors.New("mp4: edit segment_duration must be positive")
	}
	if entry.MediaRateInteger != 1 || entry.MediaRateFraction != 0 {
		return nil, errors.New("mp4: edit media rate must be 1.0")
	}
	if mvhd.Timescale == 0 {
		return nil, errors.New("mp4: invalid movie timescale")
	}
	// segment_duration lives in the movie timescale; convert to 48 kHz
	// media units with exact integer arithmetic.
	movDur := segmentDuration
	if movDur > math.MaxUint64/opusSampleRate {
		return nil, errors.New("mp4: edit duration overflow")
	}
	scaled := movDur * opusSampleRate
	if scaled%uint64(mvhd.Timescale) != 0 {
		return nil, errors.New("mp4: edit segment_duration is not an exact number of 48 kHz frames")
	}
	windowLen := scaled / uint64(mvhd.Timescale)
	if windowLen == 0 {
		return nil, errors.New("mp4: edit window must contain at least one 48 kHz frame")
	}
	S := uint64(mediaTime)
	if S > math.MaxUint16 {
		return nil, errors.New("mp4: edit media_time does not fit the OpusHead pre_skip field")
	}
	E, overflow := addU64(S, windowLen)
	if overflow {
		return nil, errors.New("mp4: edit window end overflow")
	}

	// stts: expand and check mdhd duration and per-packet TOC durations.
	sampleCount, sttsTotal, deltas, err := expandStts(b.stts)
	if err != nil {
		return nil, err
	}
	if b.mdhd.GetDuration() != sttsTotal {
		return nil, fmt.Errorf("mp4: mdhd duration %d does not match stts total %d", b.mdhd.GetDuration(), sttsTotal)
	}
	if E > sttsTotal {
		return nil, errors.New("mp4: edit end exceeds media duration")
	}

	// stsz sizes.
	sizes, err := expandStsz(b.stsz, sampleCount)
	if err != nil {
		return nil, err
	}

	// stsc expansion into per-chunk sample counts and description indices.
	chunks, err := expandStsc(b.stsc, b.stco, b.co64)
	if err != nil {
		return nil, err
	}

	// Sample count consistency across stsc, stts and stsz.
	var chunkSamples uint64
	for _, c := range chunks {
		n, overflow := addU64(chunkSamples, uint64(c.samplesPerChunk))
		if overflow {
			return nil, errors.New("mp4: sample count overflow")
		}
		chunkSamples = n
	}
	if chunkSamples != uint64(sampleCount) {
		return nil, fmt.Errorf("mp4: stsc covers %d samples but stts/stsz have %d", chunkSamples, sampleCount)
	}

	// mdat range checks for every chunk's contiguous byte span.
	if err := checkChunksInMdats(chunks, sizes, mdats); err != nil {
		return nil, err
	}

	// TOC duration checks and window coverage. Samples are visited in
	// decode order (table order), never sorted by file offset.
	endSample, err := validateOpusPackets(r, chunks, sizes, deltas, S, E)
	if err != nil {
		return nil, err
	}

	return &opusExportPlan{
		head: oggopus.Head{
			Channels:        dops.OutputChannelCount,
			PreSkip:         uint16(S),
			InputSampleRate: dops.InputSampleRate,
			OutputGain:      dops.OutputGain,
		},
		chunks:    chunks,
		sizes:     sizes,
		endSample: endSample,
		endPoint:  E,
	}, nil
}

func expandStts(stts *Stts) (count uint32, total uint64, deltas []uint32, err error) {
	if stts.EntryCount != uint32(len(stts.Entries)) {
		return 0, 0, nil, errors.New("mp4: invalid stts entry count")
	}
	deltas = make([]uint32, 0)
	for _, e := range stts.Entries {
		if e.SampleCount == 0 {
			return 0, 0, nil, errors.New("mp4: stts sample_count must be positive")
		}
		if e.SampleDelta == 0 {
			return 0, 0, nil, errors.New("mp4: stts sample_delta must be positive")
		}
		if e.SampleCount > math.MaxUint32-uint32(len(deltas)) {
			return 0, 0, nil, errors.New("mp4: stts sample count overflow")
		}
		for i := uint32(0); i < e.SampleCount; i++ {
			deltas = append(deltas, e.SampleDelta)
		}
		add, overflow := mulU64(uint64(e.SampleCount), uint64(e.SampleDelta))
		if overflow {
			return 0, 0, nil, errors.New("mp4: stts duration overflow")
		}
		total, overflow = addU64(total, add)
		if overflow {
			return 0, 0, nil, errors.New("mp4: stts duration overflow")
		}
	}
	return uint32(len(deltas)), total, deltas, nil
}

func expandStsz(stsz *Stsz, sampleCount uint32) ([]uint32, error) {
	if stsz.SampleCount != sampleCount {
		return nil, fmt.Errorf("mp4: stsz sample_count %d does not match stts count %d", stsz.SampleCount, sampleCount)
	}
	sizes := make([]uint32, sampleCount)
	if stsz.SampleSize != 0 {
		if len(stsz.EntrySize) != 0 {
			return nil, errors.New("mp4: uniform stsz must not carry entry sizes")
		}
		for i := range sizes {
			sizes[i] = stsz.SampleSize
		}
	} else {
		if uint32(len(stsz.EntrySize)) != sampleCount {
			return nil, errors.New("mp4: stsz entry count mismatch")
		}
		copy(sizes, stsz.EntrySize)
	}
	for _, size := range sizes {
		if size == 0 {
			return nil, errors.New("mp4: Opus packet size must be at least 1 byte")
		}
		if size > oggopus.MaxPacketSize {
			return nil, fmt.Errorf("mp4: Opus packet size %d exceeds Ogg limit %d", size, oggopus.MaxPacketSize)
		}
	}
	return sizes, nil
}

func expandStsc(stsc *Stsc, stco *Stco, co64 *Co64) ([]opusChunkPlan, error) {
	if stsc.EntryCount != uint32(len(stsc.Entries)) || stsc.EntryCount == 0 {
		return nil, errors.New("mp4: invalid stsc entry count")
	}
	var chunkCount uint32
	var offsets []uint64
	if stco != nil {
		if stco.EntryCount != uint32(len(stco.ChunkOffset)) {
			return nil, errors.New("mp4: invalid stco entry count")
		}
		chunkCount = stco.EntryCount
		offsets = make([]uint64, chunkCount)
		for i, o := range stco.ChunkOffset {
			offsets[i] = uint64(o)
		}
	} else {
		if co64.EntryCount != uint32(len(co64.ChunkOffset)) {
			return nil, errors.New("mp4: invalid co64 entry count")
		}
		chunkCount = co64.EntryCount
		offsets = append(offsets, co64.ChunkOffset...)
	}
	if chunkCount == 0 {
		return nil, errors.New("mp4: track has no chunks")
	}

	chunks := make([]opusChunkPlan, chunkCount)
	for i, off := range offsets {
		chunks[i].dataOffset = off
	}
	for i, e := range stsc.Entries {
		if e.FirstChunk == 0 || e.SamplesPerChunk == 0 {
			return nil, errors.New("mp4: invalid stsc entry")
		}
		if e.SampleDescriptionIndex != 1 {
			return nil, errors.New("mp4: all samples must use the single Opus description")
		}
		if i > 0 && e.FirstChunk <= stsc.Entries[i-1].FirstChunk {
			return nil, errors.New("mp4: stsc first_chunk values must be strictly increasing")
		}
		var endChunk uint32 // 1-based inclusive end of the run
		if i == len(stsc.Entries)-1 {
			endChunk = chunkCount
		} else {
			endChunk = stsc.Entries[i+1].FirstChunk - 1
		}
		if i == 0 && e.FirstChunk != 1 {
			return nil, errors.New("mp4: stsc must start at first chunk")
		}
		if endChunk > chunkCount || e.FirstChunk > endChunk {
			return nil, errors.New("mp4: stsc run exceeds chunk count")
		}
		for cn := e.FirstChunk; cn <= endChunk; cn++ {
			chunks[cn-1].samplesPerChunk = e.SamplesPerChunk
		}
	}
	for i, c := range chunks {
		if c.samplesPerChunk == 0 {
			return nil, fmt.Errorf("mp4: chunk %d has no stsc entry", i+1)
		}
	}
	return chunks, nil
}

func checkChunksInMdats(chunks []opusChunkPlan, sizes []uint32, mdats []mdatRange) error {
	sample := uint32(0)
	for ci, chunk := range chunks {
		n := chunk.samplesPerChunk
		var span uint64
		for i := uint32(0); i < n; i++ {
			add, overflow := addU64(span, uint64(sizes[sample]))
			if overflow {
				return fmt.Errorf("mp4: chunk %d byte span overflow", ci+1)
			}
			span = add
			sample++
		}
		start := chunk.dataOffset
		end, overflow := addU64(start, span)
		if overflow {
			return fmt.Errorf("mp4: chunk %d end offset overflow", ci+1)
		}
		inside := false
		for _, m := range mdats {
			if start >= m.start && start < m.end && end <= m.end {
				inside = true
				break
			}
		}
		if !inside {
			return fmt.Errorf("mp4: chunk %d bytes are not contained in a single mdat", ci+1)
		}
	}
	return nil
}

// validateOpusPackets reads every packet's TOC byte(s), compares the RFC 6716
// duration with stts independently for each packet and locates the first
// packet covering E. Packets after E are still validated: the whole source
// table must be well formed before anything is written.
func validateOpusPackets(r io.ReadSeeker, chunks []opusChunkPlan, sizes, deltas []uint32, S, E uint64) (uint32, error) {
	var (
		sample     uint32
		sttsCursor uint64 // cumulative stts media time before the current packet
		endSample  uint32
		endFound   bool
	)
	tocBuf := make([]byte, 2)
	for _, chunk := range chunks {
		offset := chunk.dataOffset
		for i := uint32(0); i < chunk.samplesPerChunk; i++ {
			size := sizes[sample]
			if _, err := r.Seek(int64(offset), io.SeekStart); err != nil {
				return 0, err
			}
			readN := 1
			if size >= 2 {
				readN = 2
			}
			if _, err := io.ReadFull(r, tocBuf[:readN]); err != nil {
				return 0, err
			}
			tocFrames, err := opusTOCFrames(tocBuf[:readN])
			if err != nil {
				return 0, fmt.Errorf("mp4: packet %d: %w", sample, err)
			}
			delta := uint64(deltas[sample])
			isLast := int(sample) == len(deltas)-1
			if !isLast {
				if delta != uint64(tocFrames) {
					return 0, fmt.Errorf("mp4: packet %d stts delta %d differs from TOC duration %d", sample, delta, tocFrames)
				}
			} else if delta == 0 || delta > uint64(tocFrames) {
				return 0, fmt.Errorf("mp4: final packet has invalid effective duration %d for TOC duration %d", delta, tocFrames)
			}
			nextStts, overflow := addU64(sttsCursor, delta)
			if overflow {
				return 0, errors.New("mp4: stts timeline overflow")
			}
			if !endFound && nextStts >= E {
				endFound = true
				endSample = sample
			}
			sttsCursor = nextStts
			offset, overflow = addU64(offset, uint64(size))
			if overflow {
				return 0, errors.New("mp4: sample offset overflow")
			}
			sample++
		}
	}
	if !endFound {
		return 0, errors.New("mp4: edit window end is not covered by any packet")
	}
	// Defensive: the audible window must start before the media timeline end.
	if S >= sttsCursor {
		return 0, errors.New("mp4: edit start lies beyond the media timeline")
	}
	return endSample, nil
}

// opusTOCFrames returns the packet duration in 48 kHz frames derived purely
// from the TOC configuration and frame-count code (RFC 6716 section 3).
func opusTOCFrames(head []byte) (uint32, error) {
	toc := head[0]
	frameDur := opusFrameDurations[toc>>3]
	var frames uint32
	switch toc & 3 {
	case 0:
		frames = 1
	case 1, 2:
		frames = 2
	case 3:
		if len(head) < 2 {
			return 0, errors.New("code 3 packet lacks its frame-count byte")
		}
		frames = uint32(head[1] & 0x3f)
		if frames == 0 {
			return 0, errors.New("code 3 packet declares zero frames")
		}
	}
	total := frames * frameDur
	if total > opusMaxTOCFrames {
		return 0, fmt.Errorf("packet TOC duration %d exceeds %d frames", total, opusMaxTOCFrames)
	}
	return total, nil
}

func addU64(a, b uint64) (uint64, bool) {
	if a > math.MaxUint64-b {
		return 0, true
	}
	return a + b, false
}

func mulU64(a, b uint64) (uint64, bool) {
	if b != 0 && a > math.MaxUint64/b {
		return 0, true
	}
	return a * b, false
}

// write streams the planned packets. Only one fixed-size packet buffer is
// used; the track payload is never fully held in memory.
func (p *opusExportPlan) write(r io.ReadSeeker, w io.Writer, serial uint32) error {
	ow := oggopus.NewWriter(w, serial)
	if err := ow.WriteHead(p.head); err != nil {
		return err
	}
	if err := ow.WriteTags("go-mp4"); err != nil {
		return err
	}

	buf := make([]byte, oggopus.MaxPacketSize)
	var sample uint32
	var granule uint64
	for _, chunk := range p.chunks {
		offset := chunk.dataOffset
		for i := uint32(0); i < chunk.samplesPerChunk; i++ {
			size := p.sizes[sample]
			if _, err := r.Seek(int64(offset), io.SeekStart); err != nil {
				return err
			}
			packet := buf[:size]
			if _, err := io.ReadFull(r, packet); err != nil {
				return err
			}
			tocFrames, err := opusTOCFrames(packet)
			if err != nil {
				return err
			}
			granule, err = addU64Checked(granule, uint64(tocFrames))
			if err != nil {
				return err
			}
			if sample == p.endSample {
				// Only the EOS page carries the trimmed granule E, so
				// the audible length is exactly E-S.
				return ow.Close(p.endPoint, packet)
			}
			if err := ow.WriteAudio(granule, packet); err != nil {
				return err
			}
			offset, err = addU64Checked(offset, uint64(size))
			if err != nil {
				return err
			}
			sample++
		}
	}
	return errors.New("mp4: internal export plan ended before the window end")
}

func addU64Checked(a, b uint64) (uint64, error) {
	v, overflow := addU64(a, b)
	if overflow {
		return 0, errors.New("mp4: integer overflow during export")
	}
	return v, nil
}
