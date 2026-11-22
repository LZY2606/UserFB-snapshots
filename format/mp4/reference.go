package mp4

import (
	"bytes"
	"fmt"
	"io"

	"github.com/nareix/joy4/format/mp4/mp4io"
)

// ReferenceResolver opens the external media identified by location.
// The location is handed over exactly as stored in the dref url entry,
// including any zero terminator handling. Implementations decide what
// the string means; the mp4 package never opens file paths or network
// resources itself.
type ReferenceResolver func(location string) (io.ReadSeeker, error)

// dataReferenceFlagSelfContained marks a url entry whose samples live in
// the main file (ISO/IEC 14496-12, url 'self-contained' flag).
const dataReferenceFlagSelfContained = 0x000001

type dataReference struct {
	selfContained bool
	// location is the raw url entry payload for external entries.
	location string
}

type streamReferences struct {
	// refs[i] is the data reference selected by stsd description i
	// (1-based stsd indices, hence the leading nil entry).
	refs []dataReference
}

// probeReferences validates the dref/stsd/stsc/stco tables of one track
// and builds the description -> data reference mapping. It performs no
// resolution and never touches external media.
func (self *Demuxer) probeReferences(stream *Stream) (err error) {
	sample := stream.sample
	refs := &streamReferences{}
	stream.refs = refs

	track := stream.trackAtom
	if track == nil || track.Media == nil || track.Media.Info == nil ||
		track.Media.Info.Data == nil || track.Media.Info.Data.Refer == nil {
		return fmt.Errorf("mp4: stream[%d]: dref table not found", stream.idx)
	}
	dref := track.Media.Info.Data.Refer
	if dref.Version != 0 {
		return fmt.Errorf("mp4: stream[%d]: dref version %d is not supported", stream.idx, dref.Version)
	}
	if dref.EntryCount < 0 || int(dref.EntryCount) != len(dref.Urls) {
		return fmt.Errorf("mp4: stream[%d]: dref entry count mismatch", stream.idx)
	}
	if len(dref.Urls) == 0 {
		return fmt.Errorf("mp4: stream[%d]: empty dref table", stream.idx)
	}

	drefs := make([]dataReference, len(dref.Urls)+1)
	for i, url := range dref.Urls {
		if url.Version != 0 {
			return fmt.Errorf("mp4: stream[%d]: url version %d is not supported", stream.idx, url.Version)
		}
		entry := dataReference{}
		if url.Flags&dataReferenceFlagSelfContained != 0 {
			entry.selfContained = true
			if len(url.Location) != 0 {
				return fmt.Errorf("mp4: stream[%d]: self-contained url entry must carry no location", stream.idx)
			}
		} else {
			location := url.Location
			if len(location) == 0 {
				return fmt.Errorf("mp4: stream[%d]: external url entry has an empty location", stream.idx)
			}
			if location[len(location)-1] != 0 {
				return fmt.Errorf("mp4: stream[%d]: url location must end with a zero byte", stream.idx)
			}
			entry.location = string(location)
		}
		drefs[i+1] = entry
	}

	if sample.SampleDesc == nil || sample.SampleDesc.EntryCount <= 0 ||
		int(sample.SampleDesc.EntryCount) != len(sample.SampleDesc.Descs) {
		return fmt.Errorf("mp4: stream[%d]: stsd table missing or entry count mismatch", stream.idx)
	}

	// One entry per stsd description; validate data_reference_index and
	// require identical codec configuration across descriptions.
	var configKey []byte
	var firstType mp4io.Tag
	for i, atom := range sample.SampleDesc.Descs {
		var dataRefIdx int16
		switch desc := atom.(type) {
		case *mp4io.AVC1Desc:
			dataRefIdx = desc.DataRefIdx
			if firstType == 0 {
				firstType = mp4io.AVC1
				if desc.Conf == nil {
					return fmt.Errorf("mp4: stream[%d]: avc1 description %d has no avcC", stream.idx, i+1)
				}
				configKey = desc.Conf.Data
			} else if firstType != mp4io.AVC1 {
				return fmt.Errorf("mp4: stream[%d]: stsd mixes codec types", stream.idx)
			} else if desc.Conf == nil || !bytes.Equal(desc.Conf.Data, configKey) {
				return fmt.Errorf("mp4: stream[%d]: avc1 descriptions differ in codec configuration", stream.idx)
			}
		case *mp4io.MP4ADesc:
			dataRefIdx = desc.DataRefIdx
			if firstType == 0 {
				firstType = mp4io.MP4A
				if desc.Conf == nil {
					return fmt.Errorf("mp4: stream[%d]: mp4a description %d has no esds", stream.idx, i+1)
				}
				configKey = desc.Conf.DecConfig
			} else if firstType != mp4io.MP4A {
				return fmt.Errorf("mp4: stream[%d]: stsd mixes codec types", stream.idx)
			} else if desc.Conf == nil || !bytes.Equal(desc.Conf.DecConfig, configKey) {
				return fmt.Errorf("mp4: stream[%d]: mp4a descriptions differ in codec configuration", stream.idx)
			}
		default:
			return fmt.Errorf("mp4: stream[%d]: unsupported sample description tag", stream.idx)
		}
		if dataRefIdx < 1 || int(dataRefIdx) >= len(drefs) {
			return fmt.Errorf("mp4: stream[%d]: data_reference_index %d out of range", stream.idx, dataRefIdx)
		}
		refs.refs = append(refs.refs, drefs[dataRefIdx])
	}

	if err = self.validateSampleTables(stream, refs); err != nil {
		return err
	}
	return nil
}

// validateSampleTables checks every table the scheduler indexes into,
// including stsc description indices, without falling back to defaults.
func (self *Demuxer) validateSampleTables(stream *Stream, refs *streamReferences) (err error) {
	sample := stream.sample
	prefix := fmt.Sprintf("mp4: stream[%d]:", stream.idx)

	if sample.SampleToChunk == nil || len(sample.SampleToChunk.Entries) == 0 {
		return fmt.Errorf("%s stsc table missing", prefix)
	}
	if sample.TimeToSample == nil || len(sample.TimeToSample.Entries) == 0 {
		return fmt.Errorf("%s stts table missing", prefix)
	}
	if sample.ChunkOffset == nil || len(sample.ChunkOffset.Entries) == 0 {
		return fmt.Errorf("%s stco table missing", prefix)
	}
	if sample.SampleSize == nil {
		return fmt.Errorf("%s stsz table missing", prefix)
	}

	if sample.SampleToChunk.Entries[0].FirstChunk != 1 {
		return fmt.Errorf("%s first stsc entry must start at chunk 1", prefix)
	}
	descCount := len(refs.refs)
	for i, entry := range sample.SampleToChunk.Entries {
		if entry.SamplesPerChunk == 0 {
			return fmt.Errorf("%s stsc entry %d has zero samples per chunk", prefix, i)
		}
		if entry.SampleDescId < 1 || int(entry.SampleDescId) > descCount {
			return fmt.Errorf("%s stsc sample_description_index %d out of range", prefix, entry.SampleDescId)
		}
		if i > 0 && entry.FirstChunk <= sample.SampleToChunk.Entries[i-1].FirstChunk {
			return fmt.Errorf("%s stsc first chunk values must increase", prefix)
		}
	}

	if sample.SampleSize.SampleSize == 0 {
		// Total sample count must match the sizes table; the scheduler
		// indexes SampleSize.Entries by sample index.
		count := 0
		group := 0
		for chunkIndex := range sample.ChunkOffset.Entries {
			n := int(sample.SampleToChunk.Entries[group].SamplesPerChunk)
			count += n
			if group+1 < len(sample.SampleToChunk.Entries) &&
				uint32(chunkIndex+1) == sample.SampleToChunk.Entries[group+1].FirstChunk {
				group++
			}
		}
		if count != len(sample.SampleSize.Entries) {
			return fmt.Errorf("%s stsz entry count %d does not match sample count %d", prefix, len(sample.SampleSize.Entries), count)
		}
	}

	total := 0
	group := 0
	for chunkIndex := range sample.ChunkOffset.Entries {
		n := int(sample.SampleToChunk.Entries[group].SamplesPerChunk)
		total += n
		if group+1 < len(sample.SampleToChunk.Entries) &&
			uint32(chunkIndex+1) == sample.SampleToChunk.Entries[group+1].FirstChunk {
			group++
		}
	}
	sttsCount := uint64(0)
	for _, entry := range sample.TimeToSample.Entries {
		sttsCount += uint64(entry.Count)
	}
	if uint64(total) != sttsCount {
		return fmt.Errorf("%s stts sample count %d does not match chunk count %d", prefix, sttsCount, total)
	}

	if sample.SyncSample != nil {
		for i, idx := range sample.SyncSample.Entries {
			if idx < 1 || int(idx) > total {
				return fmt.Errorf("%s stss entry %d out of range", prefix, i+1)
			}
			if i > 0 && idx <= sample.SyncSample.Entries[i-1] {
				return fmt.Errorf("%s stss entries must increase", prefix)
			}
		}
	}
	if sample.CompositionOffset != nil && len(sample.CompositionOffset.Entries) > 0 {
		cttsCount := uint64(0)
		for _, entry := range sample.CompositionOffset.Entries {
			cttsCount += uint64(entry.Count)
		}
		if cttsCount != uint64(total) {
			return fmt.Errorf("%s ctts sample count %d does not match chunk count %d", prefix, cttsCount, total)
		}
	}
	return nil
}

// resolveResource returns the cached reader for location, resolving it
// on first use. Failures are not cached so the caller can retry.
func (self *Demuxer) resolveResource(location string) (r io.ReadSeeker, err error) {
	if r, ok := self.resources[location]; ok {
		return r, nil
	}
	if self.resolve == nil {
		err = fmt.Errorf("mp4: external sample at %q requires NewDemuxerWithResolver", location)
		return
	}
	if r, err = self.resolve(location); err != nil {
		err = fmt.Errorf("mp4: resolve location %q failed: %w", location, err)
		return
	}
	if r == nil {
		err = fmt.Errorf("mp4: resolver returned nil reader for %q", location)
		return
	}
	self.resources[location] = r
	return r, nil
}

// readReferenceSample reads one sample, seeking to the resource's
// absolute position before every read so it never depends on a cursor
// left by a previous read.
func (self *Demuxer) readReferenceSample(ref dataReference, offset, length int64) (data []byte, err error) {
	data = make([]byte, length)
	if ref.selfContained {
		err = self.readat(offset, data)
		return
	}
	var r io.ReadSeeker
	if r, err = self.resolveResource(ref.location); err != nil {
		return
	}
	if _, err = r.Seek(offset, io.SeekStart); err != nil {
		err = fmt.Errorf("mp4: seek external %q offset %d failed: %w", ref.location, offset, err)
		return
	}
	if _, err = io.ReadFull(r, data); err != nil {
		err = fmt.Errorf("mp4: read external %q offset %d failed: %w", ref.location, offset, err)
		return
	}
	return
}
