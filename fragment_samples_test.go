package mp4_test

import (
	"bytes"
	"io"
	"math"
	"testing"

	mp4 "github.com/abema/go-mp4"
	"github.com/orcaman/writerseeker"
	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"
)

// fragWriter is a small helper around mp4.Writer to encode real box
// structures into an in-memory file.
type fragWriter struct {
	t *testing.T
	w *mp4.Writer
}

func newFragWriter(t *testing.T) (*fragWriter, *writerseeker.WriterSeeker) {
	t.Helper()
	ws := &writerseeker.WriterSeeker{}
	return &fragWriter{t: t, w: mp4.NewWriter(ws)}, ws
}

func (fw *fragWriter) start(typ mp4.BoxType) *mp4.BoxInfo {
	fw.t.Helper()
	bi, err := fw.w.StartBox(&mp4.BoxInfo{Type: typ})
	require.NoError(fw.t, err)
	return bi
}

func (fw *fragWriter) end() {
	fw.t.Helper()
	_, err := fw.w.EndBox()
	require.NoError(fw.t, err)
}

func (fw *fragWriter) payload(box mp4.IImmutableBox) {
	fw.t.Helper()
	_, err := mp4.Marshal(fw.w, box, mp4.Context{})
	require.NoError(fw.t, err)
}

func (fw *fragWriter) raw(p []byte) {
	fw.t.Helper()
	_, err := fw.w.Write(p)
	require.NoError(fw.t, err)
}

func (fw *fragWriter) trex(trex *mp4.Trex) {
	fw.start(mp4.BoxTypeTrex())
	fw.payload(trex)
	fw.end()
}

func (fw *fragWriter) tfhd(tfhd *mp4.Tfhd) {
	fw.start(mp4.BoxTypeTfhd())
	fw.payload(tfhd)
	fw.end()
}

func (fw *fragWriter) tfdt(tfdt *mp4.Tfdt) {
	fw.start(mp4.BoxTypeTfdt())
	fw.payload(tfdt)
	fw.end()
}

func (fw *fragWriter) trun(trun *mp4.Trun) {
	fw.start(mp4.BoxTypeTrun())
	fw.payload(trun)
	fw.end()
}

func (fw *fragWriter) free(payload []byte) {
	fw.start(mp4.BoxTypeFree())
	fw.raw(payload)
	fw.end()
}

func (fw *fragWriter) mdat(payload []byte) (payloadOffset uint64) {
	bi := fw.start(mp4.BoxTypeMdat())
	fw.raw(payload)
	fw.end()
	return bi.Offset + bi.HeaderSize
}

func readAll(t *testing.T, ws *writerseeker.WriterSeeker) []byte {
	t.Helper()
	data, err := io.ReadAll(ws.Reader())
	require.NoError(t, err)
	return data
}

// buildMainFile encodes a fragmented MP4 with two tracks, interleaved trafs
// and two runs in the first traf. trun1DataOffset and traf2BaseDataOffset are
// patched in by the caller (two-pass build) so that the test does not rely on
// mdat immediately following moof.
//
//	layout: ftyp moov(trex1,trex2) moof(traf1[trun,trun] traf2[trun]) free mdat1 free mdat2
func buildMainFile(t *testing.T, trun1DataOffset int32, traf2BaseDataOffset uint64) (data []byte, moofOffset, mdat1Payload, mdat2Payload uint64) {
	t.Helper()
	fw, ws := newFragWriter(t)

	fw.start(mp4.BoxTypeFtyp())
	fw.payload(&mp4.Ftyp{
		MajorBrand:   [4]byte{'i', 's', 'o', '6'},
		MinorVersion: 1,
		CompatibleBrands: []mp4.CompatibleBrandElem{
			{CompatibleBrand: [4]byte{'i', 's', 'o', '6'}},
		},
	})
	fw.end()

	fw.start(mp4.BoxTypeMoov())
	fw.start(mp4.BoxTypeMvex())
	fw.trex(&mp4.Trex{
		TrackID:                       1,
		DefaultSampleDescriptionIndex: 1,
		DefaultSampleDuration:         999,
		DefaultSampleSize:             7,
		DefaultSampleFlags:            0x01010000,
	})
	fw.trex(&mp4.Trex{
		TrackID:                       2,
		DefaultSampleDescriptionIndex: 1,
		DefaultSampleDuration:         1000,
		DefaultSampleSize:             2,
		DefaultSampleFlags:            0x02000000,
	})
	fw.end() // mvex
	fw.end() // moov

	moofOffset = fw.start(mp4.BoxTypeMoof()).Offset

	// traf #0: track 1, 64-bit tfdt, two runs
	fw.start(mp4.BoxTypeTraf())
	tfhd1 := &mp4.Tfhd{TrackID: 1}
	tfhd1.SetFlags(mp4.TfhdDefaultBaseIsMoof)
	fw.tfhd(tfhd1)
	tfdt1 := &mp4.Tfdt{BaseMediaDecodeTimeV1: 100}
	tfdt1.SetVersion(1)
	fw.tfdt(tfdt1)
	// trun #0: per-sample duration/size/flags/cto, version 1 (signed cto)
	trun1 := &mp4.Trun{
		SampleCount: 2,
		DataOffset:  trun1DataOffset,
		Entries: []mp4.TrunEntry{
			{SampleDuration: 10, SampleSize: 4, SampleFlags: 0x02000000, SampleCompositionTimeOffsetV1: 0},
			{SampleDuration: 10, SampleSize: 4, SampleFlags: 0x01010000, SampleCompositionTimeOffsetV1: -5},
		},
	}
	trun1.SetVersion(1)
	trun1.SetFlags(0x000001 | 0x000100 | 0x000200 | 0x000400 | 0x000800)
	fw.trun(trun1)
	// trun #1: no data_offset (continues after the previous run),
	// first_sample_flags plus per-sample duration/size; the second sample
	// has an explicit zero size
	trun2 := &mp4.Trun{
		SampleCount:      2,
		FirstSampleFlags: 0x02000000,
		Entries: []mp4.TrunEntry{
			{SampleDuration: 10, SampleSize: 3},
			{SampleDuration: 10, SampleSize: 0},
		},
	}
	trun2.SetFlags(0x000004 | 0x000100 | 0x000200)
	fw.trun(trun2)
	fw.end() // traf

	// traf #1: track 2, absolute base_data_offset, tfhd defaults overriding
	// trex (duration 20 over 1000, explicit zero flags over 0x02000000);
	// the sample size falls back to the trex default
	fw.start(mp4.BoxTypeTraf())
	tfhd2 := &mp4.Tfhd{
		TrackID:               2,
		BaseDataOffset:        traf2BaseDataOffset,
		DefaultSampleDuration: 20,
		DefaultSampleFlags:    0,
	}
	tfhd2.SetFlags(mp4.TfhdBaseDataOffsetPresent | mp4.TfhdDefaultSampleDurationPresent | mp4.TfhdDefaultSampleFlagsPresent)
	fw.tfhd(tfhd2)
	fw.tfdt(&mp4.Tfdt{BaseMediaDecodeTimeV0: 7})
	trunB := &mp4.Trun{SampleCount: 2, DataOffset: 0, Entries: make([]mp4.TrunEntry, 2)}
	trunB.SetFlags(0x000001)
	fw.trun(trunB)
	fw.end() // traf

	fw.end() // moof

	fw.free([]byte{0xde, 0xad, 0xbe, 0xef})
	mdat1Payload = fw.mdat([]byte("AAAABBBBCCC"))
	fw.free([]byte{0x01, 0x02, 0x03})
	mdat2Payload = fw.mdat([]byte("q1q2"))

	return readAll(t, ws), moofOffset, mdat1Payload, mdat2Payload
}

func makeMainFile(t *testing.T) (data []byte, moofOffset, mdat1Payload, mdat2Payload uint64) {
	t.Helper()
	// first pass: measure the layout with placeholder offsets
	_, moofOffset, mdat1Payload, mdat2Payload = buildMainFile(t, 0, 0)
	// second pass: patch in the real offsets (sizes do not change)
	data, moof2, mdat1b, mdat2b := buildMainFile(t, int32(mdat1Payload-moofOffset), mdat2Payload)
	require.Equal(t, moofOffset, moof2)
	require.Equal(t, mdat1Payload, mdat1b)
	require.Equal(t, mdat2Payload, mdat2b)
	return data, moofOffset, mdat1Payload, mdat2Payload
}

func TestFragmentSamples(t *testing.T) {
	t.Run("IndexAndBytes", func(t *testing.T) {
		data, moofOffset, mdat1Payload, mdat2Payload := makeMainFile(t)

		r := bytes.NewReader(data)
		samples, err := mp4.ProbeFragmentSamples(r)
		require.NoError(t, err)

		// the reader position must be restored
		pos, err := r.Seek(0, io.SeekCurrent)
		require.NoError(t, err)
		assert.Equal(t, int64(0), pos)

		require.Len(t, samples, 6)

		type expectation struct {
			trackID     uint32
			trafIndex   uint32
			trunIndex   uint32
			sampleIndex uint32
			offset      uint64
			size        uint32
			dts         uint64
			pts         int64
			duration    uint32
			flags       uint32
			marker      string
		}
		expected := []expectation{
			// traf 0 (track 1), trun 0: DTS starts at the 64-bit tfdt value 100
			{1, 0, 0, 0, mdat1Payload + 0, 4, 100, 100, 10, 0x02000000, "AAAA"},
			// composition offset -5 => PTS = 110 - 5 = 105
			{1, 0, 0, 1, mdat1Payload + 4, 4, 110, 105, 10, 0x01010000, "BBBB"},
			// traf 0, trun 1: no data_offset, continues at the end of trun 0;
			// first sample takes first_sample_flags
			{1, 0, 1, 0, mdat1Payload + 8, 3, 120, 120, 10, 0x02000000, "CCC"},
			// zero-size sample at the right endpoint of the mdat payload;
			// flags fall back to the trex default (first_sample_flags only
			// replaces the flags of the first sample of the run)
			{1, 0, 1, 1, mdat1Payload + 11, 0, 130, 130, 10, 0x01010000, ""},
			// traf 1 (track 2): absolute base_data_offset, tfhd default
			// duration 20 overrides trex 1000, explicit zero tfhd flags
			// override trex 0x02000000, size 2 falls back to trex
			{2, 1, 0, 0, mdat2Payload + 0, 2, 7, 7, 20, 0x00000000, "q1"},
			{2, 1, 0, 1, mdat2Payload + 2, 2, 27, 27, 20, 0x00000000, "q2"},
		}
		for i, e := range expected {
			s := samples[i]
			assert.Equal(t, e.trackID, s.TrackID, "sample %d TrackID", i)
			assert.Equal(t, moofOffset, s.MoofOffset, "sample %d MoofOffset", i)
			assert.Equal(t, e.trafIndex, s.TrafIndex, "sample %d TrafIndex", i)
			assert.Equal(t, e.trunIndex, s.TrunIndex, "sample %d TrunIndex", i)
			assert.Equal(t, e.sampleIndex, s.SampleIndex, "sample %d SampleIndex", i)
			assert.Equal(t, e.offset, s.Offset, "sample %d Offset", i)
			assert.Equal(t, e.size, s.Size, "sample %d Size", i)
			assert.Equal(t, e.dts, s.DTS, "sample %d DTS", i)
			assert.Equal(t, e.pts, s.PTS, "sample %d PTS", i)
			assert.Equal(t, e.duration, s.Duration, "sample %d Duration", i)
			assert.Equal(t, e.flags, s.SampleFlags, "sample %d SampleFlags", i)

			// slicing the original file at the indexed range must yield the
			// marker bytes
			end := s.Offset + uint64(s.Size)
			require.LessOrEqual(t, end, uint64(len(data)))
			assert.Equal(t, e.marker, string(data[s.Offset:end]), "sample %d bytes", i)
		}
	})

	t.Run("NegativeDataOffsetAndLargeTfdt", func(t *testing.T) {
		// the mdat box precedes the moof box, so the trun data_offset
		// relative to the moof (default-base-is-moof) is negative
		const tfdtValue = uint64(1)<<32 + 50 // exercises the 64-bit tfdt field
		build := func(dataOffset int32) (data []byte, moofOffset, mdatPayload uint64) {
			fw, ws := newFragWriter(t)

			fw.start(mp4.BoxTypeMoov())
			fw.start(mp4.BoxTypeMvex())
			fw.trex(&mp4.Trex{
				TrackID:               1,
				DefaultSampleDuration: 4,
				DefaultSampleSize:     4,
				DefaultSampleFlags:    0x01000000,
			})
			fw.end()
			fw.end()

			mdatPayload = fw.mdat([]byte("ZYXW"))

			moofOffset = fw.start(mp4.BoxTypeMoof()).Offset
			fw.start(mp4.BoxTypeTraf())
			tfhd := &mp4.Tfhd{TrackID: 1}
			tfhd.SetFlags(mp4.TfhdDefaultBaseIsMoof)
			fw.tfhd(tfhd)
			tfdt := &mp4.Tfdt{BaseMediaDecodeTimeV1: tfdtValue}
			tfdt.SetVersion(1)
			fw.tfdt(tfdt)
			trun := &mp4.Trun{
				SampleCount: 1,
				DataOffset:  dataOffset,
				Entries:     []mp4.TrunEntry{{SampleDuration: 4, SampleSize: 4}},
			}
			trun.SetFlags(0x000001 | 0x000100 | 0x000200)
			fw.trun(trun)
			fw.end()
			fw.end()

			return readAll(t, ws), moofOffset, mdatPayload
		}

		_, moofOffset, mdatPayload := build(0)
		dataOffset := int64(mdatPayload) - int64(moofOffset)
		require.True(t, dataOffset < 0, "data_offset must be negative, got %d", dataOffset)
		data, moof2, mdat2 := build(int32(dataOffset))
		require.Equal(t, moofOffset, moof2)
		require.Equal(t, mdatPayload, mdat2)

		samples, err := mp4.ProbeFragmentSamples(bytes.NewReader(data))
		require.NoError(t, err)
		require.Len(t, samples, 1)
		s := samples[0]
		assert.Equal(t, mdatPayload, s.Offset)
		assert.Equal(t, uint32(4), s.Size)
		assert.Equal(t, tfdtValue, s.DTS)
		assert.Equal(t, int64(tfdtValue), s.PTS)
		assert.Equal(t, uint32(4), s.Duration)
		assert.Equal(t, uint32(0x01000000), s.SampleFlags) // trex fallback
		assert.Equal(t, "ZYXW", string(data[s.Offset:s.Offset+uint64(s.Size)]))
	})

	t.Run("Errors", func(t *testing.T) {
		// minimalFile writes moov?(trex) moof(traf(tfhd, tfdt?, trun)) mdat.
		// A nil tfdt is skipped. When patchOffset is true, the file is built
		// twice and the trun data_offset is set so that the run starts at the
		// mdat payload (tfhd is assumed to use default-base-is-moof).
		minimalFile := func(t *testing.T, withTrex bool, tfhd *mp4.Tfhd, tfdt *mp4.Tfdt, trun *mp4.Trun, mdatPayload []byte, patchOffset bool) []byte {
			build := func() (data []byte, moofOffset, mdatPayloadOffset uint64) {
				fw, ws := newFragWriter(t)
				if withTrex {
					fw.start(mp4.BoxTypeMoov())
					fw.start(mp4.BoxTypeMvex())
					fw.trex(&mp4.Trex{
						TrackID:               1,
						DefaultSampleDuration: 10,
						DefaultSampleSize:     4,
						DefaultSampleFlags:    0x01000000,
					})
					fw.end()
					fw.end()
				}
				moofOffset = fw.start(mp4.BoxTypeMoof()).Offset
				fw.start(mp4.BoxTypeTraf())
				fw.tfhd(tfhd)
				if tfdt != nil {
					fw.tfdt(tfdt)
				}
				fw.trun(trun)
				fw.end()
				fw.end()
				mdatPayloadOffset = fw.mdat(mdatPayload)
				return readAll(t, ws), moofOffset, mdatPayloadOffset
			}
			data, moofOffset, mdatPayloadOffset := build()
			if !patchOffset {
				return data
			}
			trun.DataOffset = int32(mdatPayloadOffset - moofOffset)
			data, moof2, mdat2 := build()
			require.Equal(t, moofOffset, moof2)
			require.Equal(t, mdatPayloadOffset, mdat2)
			return data
		}

		tfhdMoofBase := func() *mp4.Tfhd {
			tfhd := &mp4.Tfhd{TrackID: 1}
			tfhd.SetFlags(mp4.TfhdDefaultBaseIsMoof)
			return tfhd
		}
		simpleTrun := func() *mp4.Trun {
			trun := &mp4.Trun{
				SampleCount: 1,
				DataOffset:  0,
				Entries:     []mp4.TrunEntry{{SampleDuration: 10, SampleSize: 4}},
			}
			trun.SetFlags(0x000001 | 0x000100 | 0x000200)
			return trun
		}

		testCases := []struct {
			name    string
			build   func(t *testing.T) []byte
			wantErr string
		}{
			{
				name: "missing tfdt",
				build: func(t *testing.T) []byte {
					return minimalFile(t, true, tfhdMoofBase(), nil, simpleTrun(), []byte("ABCD"), false)
				},
				wantErr: "tfdt box not found",
			},
			{
				name: "missing required default",
				build: func(t *testing.T) []byte {
					// no trex for track 1 and tfhd carries no default size;
					// the trun carries no per-sample size either
					tfhd := tfhdMoofBase()
					tfhd.DefaultSampleDuration = 10
					tfhd.AddFlag(mp4.TfhdDefaultSampleDurationPresent)
					trun := &mp4.Trun{
						SampleCount: 1,
						DataOffset:  0,
						Entries:     []mp4.TrunEntry{{SampleDuration: 10}},
					}
					trun.SetFlags(0x000001 | 0x000100)
					return minimalFile(t, false, tfhd, &mp4.Tfdt{}, trun, []byte("ABCD"), false)
				},
				wantErr: "missing default sample size",
			},
			{
				name: "duplicate trex track ID",
				build: func(t *testing.T) []byte {
					fw, ws := newFragWriter(t)
					fw.start(mp4.BoxTypeMoov())
					fw.start(mp4.BoxTypeMvex())
					fw.trex(&mp4.Trex{TrackID: 1, DefaultSampleDuration: 10, DefaultSampleSize: 4})
					fw.trex(&mp4.Trex{TrackID: 1, DefaultSampleDuration: 20, DefaultSampleSize: 8})
					fw.end()
					fw.end()
					return readAll(t, ws)
				},
				wantErr: "duplicate trex box for track ID 1",
			},
			{
				name: "first_sample_flags with per-sample flags",
				build: func(t *testing.T) []byte {
					trun := &mp4.Trun{
						SampleCount:      1,
						DataOffset:       0,
						FirstSampleFlags: 0x02000000,
						Entries:          []mp4.TrunEntry{{SampleDuration: 10, SampleSize: 4, SampleFlags: 0x01000000}},
					}
					trun.SetFlags(0x000001 | 0x000004 | 0x000100 | 0x000200 | 0x000400)
					return minimalFile(t, true, tfhdMoofBase(), &mp4.Tfdt{}, trun, []byte("ABCD"), false)
				},
				wantErr: "first_sample_flags and per-sample flags",
			},
			{
				name: "implicit base data offset",
				build: func(t *testing.T) []byte {
					// neither base_data_offset nor default-base-is-moof
					return minimalFile(t, true, &mp4.Tfhd{TrackID: 1}, &mp4.Tfdt{}, simpleTrun(), []byte("ABCD"), false)
				},
				wantErr: "implicit base data offset is not supported",
			},
			{
				name: "sample beyond mdat payload",
				build: func(t *testing.T) []byte {
					trun := &mp4.Trun{
						SampleCount: 1,
						DataOffset:  0,
						Entries:     []mp4.TrunEntry{{SampleDuration: 10, SampleSize: 100}},
					}
					trun.SetFlags(0x000001 | 0x000100 | 0x000200)
					return minimalFile(t, true, tfhdMoofBase(), &mp4.Tfdt{}, trun, []byte("ABCD"), true)
				},
				wantErr: "not contained in the payload",
			},
			{
				name: "sample on mdat box header",
				build: func(t *testing.T) []byte {
					// data_offset points at the mdat box header instead of
					// its payload
					build := func(dataOffset int32) (data []byte, moofOffset, mdatOffset uint64) {
						fw, ws := newFragWriter(t)
						fw.start(mp4.BoxTypeMoov())
						fw.start(mp4.BoxTypeMvex())
						fw.trex(&mp4.Trex{TrackID: 1, DefaultSampleDuration: 10, DefaultSampleSize: 4, DefaultSampleFlags: 1})
						fw.end()
						fw.end()
						moofOffset = fw.start(mp4.BoxTypeMoof()).Offset
						fw.start(mp4.BoxTypeTraf())
						fw.tfhd(tfhdMoofBase())
						fw.tfdt(&mp4.Tfdt{})
						trun := &mp4.Trun{
							SampleCount: 1,
							DataOffset:  dataOffset,
							Entries:     []mp4.TrunEntry{{SampleDuration: 10, SampleSize: 4}},
						}
						trun.SetFlags(0x000001 | 0x000100 | 0x000200)
						fw.trun(trun)
						fw.end()
						fw.end()
						bi := fw.start(mp4.BoxTypeMdat())
						fw.raw([]byte("ABCD"))
						fw.end()
						return readAll(t, ws), moofOffset, bi.Offset
					}
					_, moofOffset, mdatOffset := build(0)
					data, moof2, _ := build(int32(mdatOffset - moofOffset))
					require.Equal(t, moofOffset, moof2)
					return data
				},
				wantErr: "not contained in the payload",
			},
			{
				name: "sample spanning two mdats",
				build: func(t *testing.T) []byte {
					// a 5-byte sample starting at the first mdat payload
					// would end inside the second mdat box
					build := func(dataOffset int32) (data []byte, moofOffset, mdat1Payload uint64) {
						fw, ws := newFragWriter(t)
						fw.start(mp4.BoxTypeMoov())
						fw.start(mp4.BoxTypeMvex())
						fw.trex(&mp4.Trex{TrackID: 1, DefaultSampleDuration: 10, DefaultSampleSize: 4, DefaultSampleFlags: 1})
						fw.end()
						fw.end()
						moofOffset = fw.start(mp4.BoxTypeMoof()).Offset
						fw.start(mp4.BoxTypeTraf())
						tfhd := &mp4.Tfhd{TrackID: 1}
						tfhd.SetFlags(mp4.TfhdDefaultBaseIsMoof)
						fw.tfhd(tfhd)
						fw.tfdt(&mp4.Tfdt{})
						trun := &mp4.Trun{
							SampleCount: 1,
							DataOffset:  dataOffset,
							Entries:     []mp4.TrunEntry{{SampleDuration: 10, SampleSize: 5}},
						}
						trun.SetFlags(0x000001 | 0x000100 | 0x000200)
						fw.trun(trun)
						fw.end()
						fw.end()
						mdat1Payload = fw.mdat([]byte("AB"))
						fw.mdat([]byte("CD"))
						return readAll(t, ws), moofOffset, mdat1Payload
					}
					_, moofOffset, mdat1Payload := build(0)
					data, moof2, _ := build(int32(mdat1Payload - moofOffset))
					require.Equal(t, moofOffset, moof2)
					return data
				},
				wantErr: "not contained in the payload",
			},
			{
				name: "data offset overflow",
				build: func(t *testing.T) []byte {
					tfhd := &mp4.Tfhd{TrackID: 1, BaseDataOffset: math.MaxUint64 - 1}
					tfhd.SetFlags(mp4.TfhdBaseDataOffsetPresent)
					trun := &mp4.Trun{
						SampleCount: 1,
						DataOffset:  4,
						Entries:     []mp4.TrunEntry{{SampleDuration: 10, SampleSize: 4}},
					}
					trun.SetFlags(0x000001 | 0x000100 | 0x000200)
					return minimalFile(t, true, tfhd, &mp4.Tfdt{}, trun, []byte("ABCD"), false)
				},
				wantErr: "overflows",
			},
			{
				name: "negative data address",
				build: func(t *testing.T) []byte {
					trun := &mp4.Trun{
						SampleCount: 1,
						DataOffset:  math.MinInt32,
						Entries:     []mp4.TrunEntry{{SampleDuration: 10, SampleSize: 4}},
					}
					trun.SetFlags(0x000001 | 0x000100 | 0x000200)
					return minimalFile(t, true, tfhdMoofBase(), &mp4.Tfdt{}, trun, []byte("ABCD"), false)
				},
				wantErr: "is negative",
			},
			{
				name: "dts exceeds int64 range",
				build: func(t *testing.T) []byte {
					tfdt := &mp4.Tfdt{BaseMediaDecodeTimeV1: math.MaxUint64}
					tfdt.SetVersion(1)
					trun := &mp4.Trun{
						SampleCount: 2,
						DataOffset:  0,
						Entries: []mp4.TrunEntry{
							{SampleDuration: 10, SampleSize: 2},
							{SampleDuration: 10, SampleSize: 2},
						},
					}
					trun.SetFlags(0x000001 | 0x000100 | 0x000200)
					return minimalFile(t, true, tfhdMoofBase(), tfdt, trun, []byte("ABCD"), true)
				},
				wantErr: "exceeds int64 range",
			},
		}

		for _, tc := range testCases {
			t.Run(tc.name, func(t *testing.T) {
				data := tc.build(t)
				r := bytes.NewReader(data)
				samples, err := mp4.ProbeFragmentSamples(r)
				require.Error(t, err)
				assert.Contains(t, err.Error(), tc.wantErr)
				// no partial index may be delivered
				assert.Nil(t, samples)
				// the reader position must be restored even on failure
				pos, serr := r.Seek(0, io.SeekCurrent)
				require.NoError(t, serr)
				assert.Equal(t, int64(0), pos)
			})
		}
	})
}
