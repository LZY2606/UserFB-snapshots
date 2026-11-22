package fmp4

import (
	"fmt"
	"io"

	amp4 "github.com/abema/go-mp4"
)

const (
	tfhdBaseDataOffsetPresent         = 0x000001
	tfhdSampleDescriptionIndexPresent = 0x000002
	tfhdDefaultSampleDurationPresent  = 0x000008
	tfhdDefaultSampleSizePresent      = 0x000010
	tfhdDefaultSampleFlagsPresent     = 0x000020
	tfhdDefaultBaseIsMoof             = 0x020000

	trunFlagFirstSampleFlagsPresent = 0x000004
)

// parsedTfhd is the decoded content of a tfhd box.
type parsedTfhd struct {
	trackID               uint32
	flags                 uint32
	hasDefaultDuration    bool
	defaultSampleDuration uint32
	hasDefaultSize        bool
	defaultSampleSize     uint32
	hasDefaultFlags       bool
	defaultSampleFlags    uint32
}

// parsedSample is a sample decoded from a trun box, without its payload.
// Missing trun fields are already filled with the tfhd defaults.
type parsedSample struct {
	Duration        uint32
	Size            uint32
	IsNonSyncSample bool
	PTSOffset       int32
}

// trunEntry is the raw content of a trun sample entry.
type trunEntry struct {
	duration  uint32
	size      uint32
	flags     uint32
	ptsOffset int32

	hasDuration bool
	hasSize     bool
	hasFlags    bool
}

// parsedTrun is the decoded content of a trun box.
type parsedTrun struct {
	version       uint8
	flags         uint32
	hasDataOffset bool
	dataOffset    int32
	entries       []trunEntry
}

// parsedTraf is the decoded content of a single traf box.
type parsedTraf struct {
	trackID    uint32
	baseTime   uint64
	dataOffset int32
	samples    []parsedSample
}

// parsedFragment is the decoded content of a moof box.
type parsedFragment struct {
	sequence uint32
	trafs    []*parsedTraf
}

// readFullBoxHeader reads the version and flags of a full box, positioned at
// the start of its payload.
func readFullBoxHeader(r io.Reader) (uint8, uint32, error) {
	var buf [4]byte
	_, err := io.ReadFull(r, buf[:])
	if err != nil {
		return 0, 0, err
	}

	flags := uint32(buf[1])<<16 | uint32(buf[2])<<8 | uint32(buf[3])
	return buf[0], flags, nil
}

// trunSampleCount extracts the sample count from a raw trun box header.
func trunSampleCount(raw []byte) (uint32, bool) {
	if len(raw) < 16 {
		return 0, false
	}
	return uint32(raw[12])<<24 | uint32(raw[13])<<16 | uint32(raw[14])<<8 | uint32(raw[15]), true
}

func readUint32(r io.Reader) (uint32, error) {
	var buf [4]byte
	_, err := io.ReadFull(r, buf[:])
	if err != nil {
		return 0, err
	}
	return uint32(buf[0])<<24 | uint32(buf[1])<<16 |
		uint32(buf[2])<<8 | uint32(buf[3]), nil
}

// parseTfhd decodes a tfhd box. The reader is positioned at the box payload.
func parseTfhd(r io.Reader, payloadSize uint64) (*parsedTfhd, error) {
	_, flags, err := readFullBoxHeader(r)
	if err != nil {
		return nil, err
	}

	out := &parsedTfhd{
		flags: flags,
	}

	out.trackID, err = readUint32(r)
	if err != nil {
		return nil, err
	}

	expected := uint64(8)

	if flags&tfhdBaseDataOffsetPresent != 0 {
		var buf [8]byte
		_, err = io.ReadFull(r, buf[:])
		if err != nil {
			return nil, err
		}
		expected += 8
	}

	if flags&tfhdSampleDescriptionIndexPresent != 0 {
		_, err = readUint32(r)
		if err != nil {
			return nil, err
		}
		expected += 4
	}

	if flags&tfhdDefaultSampleDurationPresent != 0 {
		out.defaultSampleDuration, err = readUint32(r)
		if err != nil {
			return nil, err
		}
		out.hasDefaultDuration = true
		expected += 4
	}

	if flags&tfhdDefaultSampleSizePresent != 0 {
		out.defaultSampleSize, err = readUint32(r)
		if err != nil {
			return nil, err
		}
		out.hasDefaultSize = true
		expected += 4
	}

	if flags&tfhdDefaultSampleFlagsPresent != 0 {
		out.defaultSampleFlags, err = readUint32(r)
		if err != nil {
			return nil, err
		}
		out.hasDefaultFlags = true
		expected += 4
	}

	if payloadSize < expected {
		return nil, fmt.Errorf("tfhd: payload too small")
	}

	return out, nil
}

// parseTfdt decodes a tfdt box. The reader is positioned at the box payload.
func parseTfdt(r io.Reader, payloadSize uint64) (uint8, uint64, error) {
	version, _, err := readFullBoxHeader(r)
	if err != nil {
		return 0, 0, err
	}

	switch version {
	case 0:
		if payloadSize < 8 {
			return 0, 0, fmt.Errorf("tfdt: payload too small")
		}
		v, err := readUint32(r)
		return 0, uint64(v), err

	case 1:
		if payloadSize < 12 {
			return 0, 0, fmt.Errorf("tfdt: payload too small")
		}
		var buf [8]byte
		_, err = io.ReadFull(r, buf[:])
		if err != nil {
			return 0, 0, err
		}
		return 1, uint64(buf[0])<<56 | uint64(buf[1])<<48 |
			uint64(buf[2])<<40 | uint64(buf[3])<<32 |
			uint64(buf[4])<<24 | uint64(buf[5])<<16 |
			uint64(buf[6])<<8 | uint64(buf[7]), nil

	default:
		return 0, 0, fmt.Errorf("tfdt: unsupported version %d", version)
	}
}

// parseTrun decodes a trun box without touching sample payloads. The reader is
// positioned at the box payload.
func parseTrun(r io.Reader, payloadSize uint64) (*parsedTrun, error) {
	version, flags, err := readFullBoxHeader(r)
	if err != nil {
		return nil, err
	}

	if version > 1 {
		return nil, fmt.Errorf("trun: unsupported version %d", version)
	}

	out := &parsedTrun{
		version: version,
		flags:   flags,
	}

	count, err := readUint32(r)
	if err != nil {
		return nil, err
	}

	if count > maxSamplesPerTrun {
		return nil, fmt.Errorf("sample count (%d) exceeds maximum (%d)", count, maxSamplesPerTrun)
	}

	if flags&trunFlagDataOffsetPreset != 0 {
		v, err := readUint32(r)
		if err != nil {
			return nil, err
		}
		out.hasDataOffset = true
		out.dataOffset = int32(v)
	}

	if flags&trunFlagFirstSampleFlagsPresent != 0 {
		_, err = readUint32(r)
		if err != nil {
			return nil, err
		}
	}

	entrySize := 0
	if flags&trunFlagSampleDurationPresent != 0 {
		entrySize += 4
	}
	if flags&trunFlagSampleSizePresent != 0 {
		entrySize += 4
	}
	if flags&trunFlagSampleFlagsPresent != 0 {
		entrySize += 4
	}
	if flags&trunFlagSampleCompositionTimeOffsetPresentOrV1 != 0 {
		entrySize += 4
	}

	headerSize := uint64(8)
	if out.hasDataOffset {
		headerSize += 4
	}
	if flags&trunFlagFirstSampleFlagsPresent != 0 {
		headerSize += 4
	}

	if payloadSize < headerSize+uint64(entrySize)*uint64(count) {
		return nil, fmt.Errorf("trun: payload too small")
	}

	out.entries = make([]trunEntry, count)
	for i := range out.entries {
		if flags&trunFlagSampleDurationPresent != 0 {
			v, err := readUint32(r)
			if err != nil {
				return nil, err
			}
			out.entries[i].duration = v
			out.entries[i].hasDuration = true
		}

		if flags&trunFlagSampleSizePresent != 0 {
			v, err := readUint32(r)
			if err != nil {
				return nil, err
			}
			out.entries[i].size = v
			out.entries[i].hasSize = true
		}

		if flags&trunFlagSampleFlagsPresent != 0 {
			v, err := readUint32(r)
			if err != nil {
				return nil, err
			}
			out.entries[i].flags = v
			out.entries[i].hasFlags = true
		}

		if flags&trunFlagSampleCompositionTimeOffsetPresentOrV1 != 0 {
			v, err := readUint32(r)
			if err != nil {
				return nil, err
			}
			out.entries[i].ptsOffset = int32(v) // signed in both v0 and v1
		}
	}

	return out, nil
}

// resolveSample applies tfhd defaults and signed-CTS semantics to a trun entry.
func resolveSample(e trunEntry, tfhd *parsedTfhd) parsedSample {
	s := parsedSample{PTSOffset: e.ptsOffset}

	if e.hasDuration {
		s.Duration = e.duration
	} else {
		s.Duration = tfhd.defaultSampleDuration
	}

	if e.hasSize {
		s.Size = e.size
	} else {
		s.Size = tfhd.defaultSampleSize
	}

	var flags uint32
	if e.hasFlags {
		flags = e.flags
	} else {
		flags = tfhd.defaultSampleFlags
	}
	s.IsNonSyncSample = (flags & sampleFlagIsNonSyncSample) != 0

	return s
}

// parseMoof decodes a complete moof box. The reader must be positioned at the
// moof start. On success the reader is positioned at the first byte after the
// moof. Only the Recorder layout is accepted: one mfhd and, per traf, one tfhd,
// one tfdt and a single trun, with default-base-is-moof set and every sample
// field carried by the trun.
func parseMoof(r io.ReadSeeker, moofInfo *amp4.BoxInfo) (*parsedFragment, error) {
	frag := &parsedFragment{}

	var curTraf *parsedTraf
	var curTfhd *parsedTfhd
	haveMfhd := false
	haveTfdt := false
	trunCount := 0

	_, err := amp4.ReadBoxStructureFromInternal(r, moofInfo, func(h *amp4.ReadHandle) (any, error) {
		switch h.BoxInfo.Type.String() {
		case "moof":
			return h.Expand()

		case "mfhd":
			if curTraf != nil || haveMfhd {
				return nil, fmt.Errorf("unexpected mfhd")
			}

			box, _, err := h.ReadPayload()
			if err != nil {
				return nil, err
			}
			frag.sequence = box.(*amp4.Mfhd).SequenceNumber
			haveMfhd = true

		case "traf":
			if !haveMfhd {
				return nil, fmt.Errorf("unexpected traf")
			}
			if curTraf != nil {
				if curTfhd == nil || !haveTfdt || trunCount == 0 {
					return nil, fmt.Errorf("traf is missing required boxes")
				}
			}

			curTraf = &parsedTraf{}
			frag.trafs = append(frag.trafs, curTraf)
			curTfhd = nil
			haveTfdt = false
			trunCount = 0
			return h.Expand()

		case "tfhd":
			if curTraf == nil || curTfhd != nil {
				return nil, fmt.Errorf("unexpected tfhd")
			}

			if _, err := h.BoxInfo.SeekToPayload(r); err != nil {
				return nil, err
			}
			tfhd, err := parseTfhd(r, h.BoxInfo.Size-h.BoxInfo.HeaderSize)
			if err != nil {
				return nil, err
			}
			// the Recorder only writes default-base-is-moof, with no other flags
			if tfhd.flags != tfhdDefaultBaseIsMoof {
				return nil, fmt.Errorf("tfhd: only the default-base-is-moof layout is supported")
			}
			curTfhd = tfhd
			curTraf.trackID = tfhd.trackID

		case "tfdt":
			if curTraf == nil || curTfhd == nil || haveTfdt {
				return nil, fmt.Errorf("unexpected tfdt")
			}

			if _, err := h.BoxInfo.SeekToPayload(r); err != nil {
				return nil, err
			}
			version, baseTime, err := parseTfdt(r, h.BoxInfo.Size-h.BoxInfo.HeaderSize)
			if err != nil {
				return nil, err
			}
			if version != 1 {
				return nil, fmt.Errorf("tfdt: only version 1 is supported")
			}
			curTraf.baseTime = baseTime
			haveTfdt = true

		case "trun":
			if curTraf == nil || curTfhd == nil || !haveTfdt {
				return nil, fmt.Errorf("unexpected trun")
			}
			trunCount++
			if trunCount > 1 {
				return nil, fmt.Errorf("only one trun per traf is supported")
			}

			if _, err := h.BoxInfo.SeekToPayload(r); err != nil {
				return nil, err
			}
			trun, err := parseTrun(r, h.BoxInfo.Size-h.BoxInfo.HeaderSize)
			if err != nil {
				return nil, err
			}
			if !trun.hasDataOffset {
				return nil, fmt.Errorf("trun: data offset is required")
			}
			if trun.flags&(trunFlagFirstSampleFlagsPresent) != 0 {
				return nil, fmt.Errorf("trun: first sample flags are unsupported")
			}
			if trun.flags&^(trunFlagDataOffsetPreset|trunFlagSampleDurationPresent|
				trunFlagSampleSizePresent|trunFlagSampleFlagsPresent|
				trunFlagSampleCompositionTimeOffsetPresentOrV1) != 0 {
				return nil, fmt.Errorf("trun: unsupported flags")
			}
			if trun.flags&(trunFlagSampleDurationPresent|trunFlagSampleSizePresent) !=
				(trunFlagSampleDurationPresent | trunFlagSampleSizePresent) {
				return nil, fmt.Errorf("trun: sample duration and size are required")
			}
			if trun.version != 1 {
				return nil, fmt.Errorf("trun: only version 1 is supported")
			}

			curTraf.dataOffset = trun.dataOffset
			curTraf.samples = make([]parsedSample, len(trun.entries))
			for i, e := range trun.entries {
				curTraf.samples[i] = resolveSample(e, curTfhd)
			}

		default:
			// unknown boxes inside moof/traf are not part of the Recorder layout
			return nil, fmt.Errorf("unexpected box '%v'", h.BoxInfo.Type)
		}

		return nil, nil
	})
	if err != nil {
		return nil, err
	}

	if !haveMfhd || len(frag.trafs) == 0 {
		return nil, fmt.Errorf("moof is missing required boxes")
	}

	if curTfhd == nil || !haveTfdt || trunCount == 0 {
		return nil, fmt.Errorf("traf is missing required boxes")
	}

	return frag, nil
}
