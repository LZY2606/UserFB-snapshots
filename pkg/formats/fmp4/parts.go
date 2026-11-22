package fmp4

import (
	"bytes"
	"fmt"
	"io"

	amp4 "github.com/abema/go-mp4"
)

const (
	maxSamplesPerTrun = 120 * 160 // 120fps * 60 seconds
)

// Parts is a sequence of fMP4 parts.
type Parts []*Part

// Unmarshal decodes one or more fMP4 parts.
func (ps *Parts) Unmarshal(byts []byte) error {
	type readState int

	const (
		waitingMoof readState = iota
		waitingMfhd
		waitingTraf
		waitingTfdtTfhdTrun
	)

	state := waitingMoof
	var curPart *Part
	var moofOffset uint64
	var curTrack *PartTrack
	var curTfhd *parsedTfhd
	var haveTfdt bool

	_, err := amp4.ReadBoxStructure(bytes.NewReader(byts), func(h *amp4.ReadHandle) (any, error) {
		if h.BoxInfo.IsSupportedType() {
			switch h.BoxInfo.Type.String() {
			case "moof":
				if state != waitingMoof {
					return nil, fmt.Errorf("unexpected moof")
				}

				curPart = &Part{}
				*ps = append(*ps, curPart)
				moofOffset = h.BoxInfo.Offset
				state = waitingMfhd
				return h.Expand()

			case "mfhd":
				if state != waitingMfhd {
					return nil, fmt.Errorf("unexpected mfhd")
				}

				box, _, err := h.ReadPayload()
				if err != nil {
					return nil, err
				}
				mfhd := box.(*amp4.Mfhd)

				curPart.SequenceNumber = mfhd.SequenceNumber
				state = waitingTraf

			case "traf":
				if state != waitingTraf && state != waitingTfdtTfhdTrun {
					return nil, fmt.Errorf("unexpected traf")
				}

				if curTrack != nil {
					if !haveTfdt || curTfhd == nil {
						return nil, fmt.Errorf("parse error")
					}
				}

				curTrack = &PartTrack{}
				curPart.Tracks = append(curPart.Tracks, curTrack)
				curTfhd = nil
				haveTfdt = false
				state = waitingTfdtTfhdTrun
				return h.Expand()

			case "tfhd":
				if state != waitingTfdtTfhdTrun || curTfhd != nil {
					return nil, fmt.Errorf("unexpected tfhd")
				}

				parsed, err := parseTfhd(
					bytes.NewReader(byts[h.BoxInfo.Offset+h.BoxInfo.HeaderSize:]),
					h.BoxInfo.Size-h.BoxInfo.HeaderSize,
				)
				if err != nil {
					return nil, err
				}

				curTfhd = parsed
				curTrack.ID = int(parsed.trackID)

			case "tfdt":
				if state != waitingTfdtTfhdTrun || haveTfdt {
					return nil, fmt.Errorf("unexpected tfdt")
				}

				_, baseTime, err := parseTfdt(
					bytes.NewReader(byts[h.BoxInfo.Offset+h.BoxInfo.HeaderSize:]),
					h.BoxInfo.Size-h.BoxInfo.HeaderSize,
				)
				if err != nil {
					return nil, err
				}

				curTrack.BaseTime = baseTime
				haveTfdt = true

			case "trun":
				if state != waitingTfdtTfhdTrun || curTfhd == nil {
					return nil, fmt.Errorf("unexpected trun")
				}

				// prevent RAM exhaustion due to unlimited Trun unmarshaling
				rawBox := byts[h.BoxInfo.Offset:]
				if sampleCount, ok := trunSampleCount(rawBox); ok && sampleCount > maxSamplesPerTrun {
					return nil, fmt.Errorf("sample count (%d) exceeds maximum (%d)", sampleCount, maxSamplesPerTrun)
				}

				parsed, err := parseTrun(
					bytes.NewReader(byts[h.BoxInfo.Offset+h.BoxInfo.HeaderSize:]),
					h.BoxInfo.Size-h.BoxInfo.HeaderSize,
				)
				if err != nil {
					return nil, err
				}

				if len(parsed.entries) != 0 {
					if !parsed.hasDataOffset {
						return nil, fmt.Errorf("unsupported flags")
					}

					existing := len(curTrack.Samples)
					tmp := make([]*Sample, existing+len(parsed.entries))
					copy(tmp, curTrack.Samples)
					curTrack.Samples = tmp

					pos := uint64(parsed.dataOffset) + moofOffset
					if uint64(len(byts)) < pos {
						return nil, fmt.Errorf("invalid data_offset / moof_offset")
					}

					ptr := byts[pos:]

					for i, e := range parsed.entries {
						s := &Sample{}
						resolved := resolveSample(e, curTfhd)

						s.Duration = resolved.Duration
						s.PTSOffset = resolved.PTSOffset
						s.IsNonSyncSample = resolved.IsNonSyncSample

						size := resolved.Size
						if size == 0 || len(ptr) < int(size) {
							return nil, fmt.Errorf("invalid sample size")
						}

						s.Payload = ptr[:size]
						ptr = ptr[size:]

						curTrack.Samples[existing+i] = s
					}
				}

			case "mdat":
				if state != waitingTraf && state != waitingTfdtTfhdTrun {
					return nil, fmt.Errorf("unexpected mdat")
				}

				if curTrack != nil {
					if !haveTfdt || curTfhd == nil {
						return nil, fmt.Errorf("parse error")
					}
				}

				state = waitingMoof
			}
		}

		return nil, nil
	})
	if err != nil {
		return err
	}

	if state != waitingMoof {
		return fmt.Errorf("decode error")
	}

	return nil
}

// Marshal encodes a one or more fMP4 part.
func (ps Parts) Marshal(w io.WriteSeeker) error {
	for _, p := range ps {
		err := p.Marshal(w)
		if err != nil {
			return err
		}
	}
	return nil
}
