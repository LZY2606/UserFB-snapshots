package astits

import (
	"bytes"
	"context"
	"errors"
	"testing"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"
)

// This test exercises SCTE-35 support end to end: hand-written splice_info_section
// payloads, an independent MPEG-2 CRC32 implementation, real 188-byte TS packets
// and the public Demuxer with PMT-based PID recognition.
//
// Wire formats follow ANSI/SCTE 35 2023r1 section 9:
// https://www.scte.org/standards/library/catalog/scte-35-digital-program-insertion-cueing-message-and-api/

const (
	scte35TestPMTPID   uint16 = 0x1000
	scte35TestCuePIDA  uint16 = 0x0135
	scte35TestCuePIDB  uint16 = 0x0136
	scte35TestVideoPID uint16 = 0x0140
	scte35TestOtherPID uint16 = 0x0150
)

// scte35TestCRC32 is an independent, table-free MPEG-2 CRC32 implementation used
// only to build the test sections. It must not share code with computeCRC32.
func scte35TestCRC32(b []byte) uint32 {
	crc := uint32(0xffffffff)
	for _, v := range b {
		crc ^= uint32(v) << 24
		for bit := 0; bit < 8; bit++ {
			if crc&0x80000000 != 0 {
				crc = (crc << 1) ^ 0x04c11db7
			} else {
				crc <<= 1
			}
		}
	}
	return crc
}

// scte35PSISection builds a generic CRC-protected PSI section with a syntax header.
func scte35PSISection(tableID byte, tableIDExtension, version uint16, body []byte) []byte {
	sectionLength := 5 + len(body) + 4 // syntax header + body + CRC_32
	out := []byte{
		tableID,
		0xb0 | byte(sectionLength>>8),
		byte(sectionLength),
		byte(tableIDExtension >> 8),
		byte(tableIDExtension),
		0xc1 | byte(version<<1), // current/next indicator set
		0x00,                    // section_number
		0x00,                    // last_section_number
	}
	out = append(out, body...)
	crc := scte35TestCRC32(out)
	return append(out, byte(crc>>24), byte(crc>>16), byte(crc>>8), byte(crc))
}

func scte35TestPAT() []byte {
	body := []byte{
		0x00, 0x01, // program_number 1
		0xe0 | byte(scte35TestPMTPID>>8), byte(scte35TestPMTPID & 0xff), // reserved + PMT PID
	}
	return scte35PSISection(0x00, 0x0001, 0, body)
}

type scte35TestES struct {
	streamType StreamType
	pid        uint16
}

func scte35TestPMT(streams []scte35TestES) []byte {
	body := []byte{
		0xe0 | byte(scte35TestVideoPID>>8), byte(scte35TestVideoPID & 0xff), // PCR_PID
		0xf0, 0x00, // program_info_length = 0
	}
	for _, es := range streams {
		body = append(body,
			byte(es.streamType),
			byte(es.pid>>8)&0x1f,
			byte(es.pid),
			0xf0, 0x00, // es_info_length = 0
		)
	}
	return scte35PSISection(0x02, 0x0001, 0, body)
}

// scte35SectionOptions controls the splice_info_section wrapper.
type scte35SectionOptions struct {
	ptsAdjustment uint64
	tier          uint16
	encrypted     bool
	protocol      byte
	unbounded     bool // force splice_command_length to 0xFFF
}

// scte35BuildSection assembles one complete splice_info_section and appends a
// valid CRC_32 over the exact wire bytes.
func scte35BuildSection(opts scte35SectionOptions, command, descriptors []byte) []byte {
	body := []byte{opts.protocol}

	enc0 := byte(opts.ptsAdjustment>>32) & 0x01
	if opts.encrypted {
		enc0 |= 0x80 // encrypted_packet
	}
	body = append(body, enc0,
		byte(opts.ptsAdjustment>>24),
		byte(opts.ptsAdjustment>>16),
		byte(opts.ptsAdjustment>>8),
		byte(opts.ptsAdjustment),
		0x00, // cw_index
	)

	commandLength := len(command)
	cmdHigh := byte(commandLength >> 8)
	cmdLow := byte(commandLength)
	if opts.unbounded {
		cmdHigh, cmdLow = 0x0f, 0xff
	}
	body = append(body,
		0xf0|(byte(opts.tier>>8)&0x0f), byte(opts.tier),
		0xf0|(cmdHigh&0x0f), cmdLow,
	)
	body = append(body, command...)
	body = append(body, byte(len(descriptors)>>8), byte(len(descriptors)))
	body = append(body, descriptors...)

	sectionLength := len(body) + 4
	out := []byte{0xfc, byte(sectionLength>>8) & 0x0f, byte(sectionLength)}
	out = append(out, body...)
	crc := scte35TestCRC32(out)
	return append(out, byte(crc>>24), byte(crc>>16), byte(crc>>8), byte(crc))
}

func scte35PutSpliceTime(b *[]byte, pts uint64, specified bool) {
	if !specified {
		*b = append(*b, 0x7f) // time_specified_flag = 0, reserved bits set
		return
	}
	*b = append(*b,
		0xfe|(byte(pts>>32)&0x01),
		byte(pts>>24), byte(pts>>16), byte(pts>>8), byte(pts),
	)
}

func scte35NullCommand() []byte {
	return []byte{SCTE35SpliceCommandTypeNull}
}

func scte35TimeSignalCommand(pts uint64, specified bool) []byte {
	cmd := []byte{SCTE35SpliceCommandTypeTimeSignal}
	scte35PutSpliceTime(&cmd, pts, specified)
	return cmd
}

func scte35InsertProgramCommand(eventID uint32, outOfNetwork, immediate bool, pts uint64, specified bool, duration *SCTE35BreakDuration, upid uint16, availNum, availsExpected uint8) []byte {
	cmd := []byte{
		SCTE35SpliceCommandTypeInsert,
		byte(eventID >> 24), byte(eventID >> 16), byte(eventID >> 8), byte(eventID),
		0x7f, // not canceled, reserved bits set
	}
	flags := byte(0x0f) // four reserved bits
	if outOfNetwork {
		flags |= 0x80
	}
	flags |= 0x40 // program_splice_flag
	hasDuration := duration != nil
	if hasDuration {
		flags |= 0x20
	}
	if immediate {
		flags |= 0x10
	}
	cmd = append(cmd, flags)
	if !immediate {
		scte35PutSpliceTime(&cmd, pts, specified)
	}
	if hasDuration {
		df := byte(0x7e)
		if duration.AutoReturn {
			df |= 0x80
		}
		df |= byte(duration.Duration>>32) & 0x01
		cmd = append(cmd, df,
			byte(duration.Duration>>24), byte(duration.Duration>>16),
			byte(duration.Duration>>8), byte(duration.Duration),
		)
	}
	cmd = append(cmd, byte(upid>>8), byte(upid), availNum, availsExpected)
	return cmd
}

func scte35InsertCancelCommand(eventID uint32) []byte {
	return []byte{
		SCTE35SpliceCommandTypeInsert,
		byte(eventID >> 24), byte(eventID >> 16), byte(eventID >> 8), byte(eventID),
		0xff, // splice_event_cancel_indicator set
	}
}

func scte35InsertComponentCommand(eventID uint32, components []struct {
	tag       uint8
	pts       uint64
	specified bool
}, upid uint16) []byte {
	cmd := []byte{
		SCTE35SpliceCommandTypeInsert,
		byte(eventID >> 24), byte(eventID >> 16), byte(eventID >> 8), byte(eventID),
		0x7f, // not canceled, reserved bits set
		0x0f, // component splice, no duration, not immediate, reserved bits set
		byte(len(components)),
	}
	for _, c := range components {
		cmd = append(cmd, c.tag)
		scte35PutSpliceTime(&cmd, c.pts, c.specified)
	}
	cmd = append(cmd, byte(upid>>8), byte(upid), 0x00, 0x00)
	return cmd
}

func scte35Descriptor(tag uint8, data []byte) []byte {
	return append([]byte{tag, byte(len(data))}, data...)
}

// scte35TSPackets splits a payload into 188-byte TS packets with per-PID
// continuity counters and 0xFF packet stuffing.
func scte35TSPackets(pid uint16, pusi bool, payload []byte, startCC uint8) [][]byte {
	var packets [][]byte
	cc := startCC
	for offset := 0; offset < len(payload); {
		p := make([]byte, MpegTsPacketSize)
		p[0] = syncByte
		p[1] = byte(pid>>8) & 0x1f
		if pusi && offset == 0 {
			p[1] |= 0x40 // payload_unit_start_indicator
		}
		p[2] = byte(pid)
		p[3] = 0x10 | (cc & 0x0f) // payload-only adaptation field control + continuity counter
		cc++

		n := copy(p[4:], payload[offset:])
		offset += n
		for k := 4 + n; k < MpegTsPacketSize; k++ {
			p[k] = 0xff
		}
		packets = append(packets, p)
	}
	return packets
}

type scte35TSBuffer struct {
	buf bytes.Buffer
	cc  map[uint16]uint8
}

func newSCTE35TSBuffer() *scte35TSBuffer {
	return &scte35TSBuffer{cc: make(map[uint16]uint8)}
}

func (b *scte35TSBuffer) write(pid uint16, pusi bool, payload []byte) {
	packets := scte35TSPackets(pid, pusi, payload, b.cc[pid])
	b.cc[pid] = (b.cc[pid] + uint8(len(packets))) & 0x0f
	for _, p := range packets {
		b.buf.Write(p)
	}
}

func (b *scte35TSBuffer) writeRaw(packets ...[]byte) {
	for _, p := range packets {
		b.buf.Write(p)
	}
}

func (b *scte35TSBuffer) writePreamble(streams []scte35TestES) {
	b.write(PIDPAT, true, append([]byte{0x00}, scte35TestPAT()...))
	b.write(scte35TestPMTPID, true, append([]byte{0x00}, scte35TestPMT(streams)...))
}

func (b *scte35TSBuffer) demuxer() *Demuxer {
	return NewDemuxer(context.Background(), bytes.NewReader(b.buf.Bytes()), DemuxerOptPacketSize(188))
}

// scte35Next requires the next DemuxerData call to succeed and return data.
func scte35Next(t *testing.T, dmx *Demuxer) *DemuxerData {
	d, err := dmx.NextData()
	require.NoError(t, err)
	require.NotNil(t, d)
	return d
}

func scte35TestPES(data []byte) []byte {
	p := []byte{0x00, 0x00, 0x01, 0xe0}
	length := 3 + len(data) // optional PES header (3 bytes, header data length 0) + ES data
	p = append(p, byte(length>>8), byte(length), 0x80, 0x00, 0x00)
	return append(p, data...)
}

func scte35DefaultStreams() []scte35TestES {
	return []scte35TestES{
		{streamType: StreamTypeH264Video, pid: scte35TestVideoPID},
		{streamType: StreamTypeSCTE35, pid: scte35TestCuePIDA},
		{streamType: StreamTypeSCTE35, pid: scte35TestCuePIDB},
		{streamType: StreamTypePrivateData, pid: scte35TestOtherPID},
	}
}

func TestSCTE35Contract(t *testing.T) {
	// Two command types in the same cue stream, and two complete sections in a
	// single payload unit start.
	t.Run("two_commands_and_two_sections_per_start", func(t *testing.T) {
		buf := newSCTE35TSBuffer()
		buf.writePreamble(scte35DefaultStreams())

		null := scte35BuildSection(scte35SectionOptions{tier: 0xabc},
			scte35NullCommand(),
			scte35Descriptor(0x80, []byte{0x01, 0x02}))
		timeSignal := scte35BuildSection(scte35SectionOptions{ptsAdjustment: 0x1234, tier: 0x001},
			scte35TimeSignalCommand(0x100000000, true), nil)

		// Both sections in one payload unit start; packet stuffing follows them.
		buf.write(scte35TestCuePIDA, true, append(append([]byte{}, null...), timeSignal...))

		dmx := buf.demuxer()
		pat, err := dmx.NextData()
		require.NoError(t, err)
		assert.NotNil(t, pat.PAT)
		pmt, err := dmx.NextData()
		require.NoError(t, err)
		assert.NotNil(t, pmt.PMT)

		d1, err := dmx.NextData()
		require.NoError(t, err)
		assert.NotNil(t, d1.SCTE35)
		assert.Equal(t, scte35TestCuePIDA, d1.PID)
		assert.True(t, d1.FirstPacket.Header.PayloadUnitStartIndicator)
		assert.Equal(t, scte35TestCuePIDA, d1.FirstPacket.Header.PID)
		assert.Equal(t, SCTE35SpliceCommandTypeNull, d1.SCTE35.Command.Type)
		assert.NotNil(t, d1.SCTE35.Command.Null)
		assert.Equal(t, uint16(0xabc), d1.SCTE35.Tier)
		assert.Len(t, d1.SCTE35.Descriptors, 1)
		assert.Equal(t, uint8(0x80), d1.SCTE35.Descriptors[0].Tag)
		assert.Equal(t, []byte{0x01, 0x02}, d1.SCTE35.Descriptors[0].Data)
		assert.Equal(t, null, d1.SCTE35.Section)

		d2, err := dmx.NextData()
		require.NoError(t, err)
		assert.NotNil(t, d2.SCTE35)
		assert.Equal(t, scte35TestCuePIDA, d2.PID)
		assert.Equal(t, SCTE35SpliceCommandTypeTimeSignal, d2.SCTE35.Command.Type)
		assert.NotNil(t, d2.SCTE35.Command.TimeSignal.SpliceTime)
		assert.Equal(t, uint64(0x100000000), d2.SCTE35.Command.TimeSignal.SpliceTime.PTS)
		assert.Equal(t, uint64(0x100001234), d2.SCTE35.Command.TimeSignal.SpliceTime.AdjustedPTS)
		assert.Empty(t, d2.SCTE35.Descriptors)
		assert.Equal(t, timeSignal, d2.SCTE35.Section)

		// The stored section must not alias a buffer reused by later parsing.
		snapshot := append([]byte{}, d1.SCTE35.Section...)
		_, err = dmx.NextData()
		assert.Equal(t, ErrNoMorePackets, err)
		assert.Equal(t, snapshot, d1.SCTE35.Section)
	})

	// Program splice with a specified time, break duration and avail metadata.
	t.Run("splice_insert_program_scheduled", func(t *testing.T) {
		buf := newSCTE35TSBuffer()
		buf.writePreamble(scte35DefaultStreams())

		duration := &SCTE35BreakDuration{AutoReturn: true, Duration: 450000}
		cmd := scte35InsertProgramCommand(0x11223344, true, false, 900000, true, duration, 0x0a0b, 3, 7)
		section := scte35BuildSection(scte35SectionOptions{ptsAdjustment: 0x1000},
			cmd, scte35Descriptor(0x02, []byte{0xde, 0xad, 0xbe, 0xef}))
		buf.write(scte35TestCuePIDA, true, section)

		dmx := buf.demuxer()
		scte35Next(t, dmx)
		pmt, err := dmx.NextData()
		require.NoError(t, err)
		assert.NotNil(t, pmt.PMT)

		d, err := dmx.NextData()
		require.NoError(t, err)
		in := d.SCTE35.Command.Insert
		assert.NotNil(t, in)
		assert.Equal(t, uint32(0x11223344), in.EventID)
		assert.True(t, in.OutOfNetwork)
		assert.False(t, in.Canceled)
		assert.False(t, in.Immediate)
		assert.True(t, in.ProgramSplice)
		assert.NotNil(t, in.SpliceTime)
		assert.Equal(t, uint64(900000), in.SpliceTime.PTS)
		assert.Equal(t, uint64(904096), in.SpliceTime.AdjustedPTS)
		assert.Nil(t, in.Components)
		assert.NotNil(t, in.Duration)
		assert.True(t, in.Duration.AutoReturn)
		assert.Equal(t, uint64(450000), in.Duration.Duration) // duration, not adjusted
		assert.Equal(t, uint16(0x0a0b), in.UniqueProgramID)
		assert.Equal(t, uint8(3), in.AvailNum)
		assert.Equal(t, uint8(7), in.AvailsExpected)
		// The segmentation descriptor is preserved raw; its internals are not decoded.
		assert.Len(t, d.SCTE35.Descriptors, 1)
		assert.Equal(t, uint8(0x02), d.SCTE35.Descriptors[0].Tag)
		assert.Equal(t, []byte{0xde, 0xad, 0xbe, 0xef}, d.SCTE35.Descriptors[0].Data)
	})

	// Immediate program splice: no splice time is present.
	t.Run("splice_insert_immediate", func(t *testing.T) {
		buf := newSCTE35TSBuffer()
		buf.writePreamble(scte35DefaultStreams())

		cmd := scte35InsertProgramCommand(0x55, false, true, 0, false, nil, 0x0102, 1, 1)
		buf.write(scte35TestCuePIDA, true, scte35BuildSection(scte35SectionOptions{}, cmd, nil))

		dmx := buf.demuxer()
		scte35Next(t, dmx)
		_, err := dmx.NextData()
		require.NoError(t, err)
		d, err := dmx.NextData()
		require.NoError(t, err)
		in := d.SCTE35.Command.Insert
		assert.True(t, in.Immediate)
		assert.True(t, in.ProgramSplice)
		assert.Nil(t, in.SpliceTime, "an immediate splice specifies no PTS")
		assert.False(t, in.OutOfNetwork)
		assert.Nil(t, in.Duration)
	})

	// Component splice: one specified and one unspecified component time.
	t.Run("splice_insert_component", func(t *testing.T) {
		buf := newSCTE35TSBuffer()
		buf.writePreamble(scte35DefaultStreams())

		cmd := scte35InsertComponentCommand(0x99, []struct {
			tag       uint8
			pts       uint64
			specified bool
		}{
			{tag: 0x10, pts: 1000, specified: true},
			{tag: 0x20, specified: false},
		}, 0x0304)
		section := scte35BuildSection(scte35SectionOptions{ptsAdjustment: 250}, cmd, nil)
		buf.write(scte35TestCuePIDA, true, section)

		dmx := buf.demuxer()
		scte35Next(t, dmx)
		scte35Next(t, dmx)
		d, err := dmx.NextData()
		require.NoError(t, err)
		in := d.SCTE35.Command.Insert
		assert.False(t, in.ProgramSplice)
		assert.Len(t, in.Components, 2)
		assert.Equal(t, uint8(0x10), in.Components[0].Tag)
		assert.NotNil(t, in.Components[0].SpliceTime)
		assert.Equal(t, uint64(1000), in.Components[0].SpliceTime.PTS)
		assert.Equal(t, uint64(1250), in.Components[0].SpliceTime.AdjustedPTS)
		assert.Equal(t, uint8(0x20), in.Components[1].Tag)
		assert.Nil(t, in.Components[1].SpliceTime, "unspecified component time must not become zero")
		assert.Nil(t, in.SpliceTime)
		assert.Equal(t, uint16(0x0304), in.UniqueProgramID)
	})

	// Canceled splice event.
	t.Run("splice_insert_cancel", func(t *testing.T) {
		buf := newSCTE35TSBuffer()
		buf.writePreamble(scte35DefaultStreams())

		cmd := scte35InsertCancelCommand(0x77777777)
		buf.write(scte35TestCuePIDA, true, scte35BuildSection(scte35SectionOptions{}, cmd, nil))

		dmx := buf.demuxer()
		scte35Next(t, dmx)
		scte35Next(t, dmx)
		d, err := dmx.NextData()
		require.NoError(t, err)
		in := d.SCTE35.Command.Insert
		assert.True(t, in.Canceled)
		assert.Equal(t, uint32(0x77777777), in.EventID)
		assert.Nil(t, in.SpliceTime)
		assert.Nil(t, in.Components)
		assert.Nil(t, in.Duration)
	})

	// time_signal without a specified PTS stays distinguishable from PTS zero.
	t.Run("time_signal_unspecified_pts", func(t *testing.T) {
		buf := newSCTE35TSBuffer()
		buf.writePreamble(scte35DefaultStreams())

		buf.write(scte35TestCuePIDA, true,
			scte35BuildSection(scte35SectionOptions{}, scte35TimeSignalCommand(0, false), nil))

		dmx := buf.demuxer()
		scte35Next(t, dmx)
		scte35Next(t, dmx)
		d, err := dmx.NextData()
		require.NoError(t, err)
		assert.Nil(t, d.SCTE35.Command.TimeSignal.SpliceTime)
	})

	// An adjustment close to 2^33 wraps the adjusted PTS on 33 bits, while the
	// raw PTS and the break duration are left untouched.
	t.Run("pts_adjustment_near_2pow33_wraps", func(t *testing.T) {
		buf := newSCTE35TSBuffer()
		buf.writePreamble(scte35DefaultStreams())

		const ptsSpace = uint64(1) << 33
		adjustment := ptsSpace - 9000 // 0x1ffffdc d8
		duration := &SCTE35BreakDuration{AutoReturn: false, Duration: 123456}
		cmd := scte35InsertProgramCommand(1, true, false, 10000, true, duration, 0, 0, 0)
		section := scte35BuildSection(scte35SectionOptions{ptsAdjustment: adjustment}, cmd, nil)
		buf.write(scte35TestCuePIDA, true, section)

		dmx := buf.demuxer()
		scte35Next(t, dmx)
		scte35Next(t, dmx)
		d, err := dmx.NextData()
		require.NoError(t, err)
		st := d.SCTE35.Command.Insert.SpliceTime
		assert.Equal(t, uint64(10000), st.PTS)
		assert.Equal(t, uint64(1000), st.AdjustedPTS, "(10000 + (2^33 - 9000)) mod 2^33 = 1000")
		assert.Equal(t, adjustment, d.SCTE35.PTSAdjustment)
		assert.Equal(t, uint64(123456), d.SCTE35.Command.Insert.Duration.Duration)
	})

	// An unknown command keeps its declared bytes and its descriptors still parse.
	t.Run("unknown_command_then_descriptors", func(t *testing.T) {
		buf := newSCTE35TSBuffer()
		buf.writePreamble(scte35DefaultStreams())

		rawCommand := []byte{0x42, 0xaa, 0xbb, 0xcc, 0xdd} // type 0x42 is not implemented
		descriptors := append(
			scte35Descriptor(0x02, []byte{0x01}),
			scte35Descriptor(0x80, []byte{0x09, 0x08, 0x07})...,
		)
		buf.write(scte35TestCuePIDA, true,
			scte35BuildSection(scte35SectionOptions{}, rawCommand, descriptors))

		dmx := buf.demuxer()
		scte35Next(t, dmx)
		scte35Next(t, dmx)
		d, err := dmx.NextData()
		require.NoError(t, err)
		assert.Equal(t, uint8(0x42), d.SCTE35.Command.Type)
		assert.Equal(t, rawCommand, d.SCTE35.Command.Raw)
		assert.Nil(t, d.SCTE35.Command.Null)
		assert.Nil(t, d.SCTE35.Command.Insert)
		assert.Nil(t, d.SCTE35.Command.TimeSignal)
		assert.Len(t, d.SCTE35.Descriptors, 2)
		assert.Equal(t, uint8(0x02), d.SCTE35.Descriptors[0].Tag)
		assert.Equal(t, []byte{0x01}, d.SCTE35.Descriptors[0].Data)
		assert.Equal(t, uint8(0x80), d.SCTE35.Descriptors[1].Tag)
		assert.Equal(t, []byte{0x09, 0x08, 0x07}, d.SCTE35.Descriptors[1].Data)
	})

	// Bad CRC: a recognizable error, no half object, and later media still flows.
	t.Run("bad_crc_then_legal_media", func(t *testing.T) {
		buf := newSCTE35TSBuffer()
		buf.writePreamble(scte35DefaultStreams())

		section := scte35BuildSection(scte35SectionOptions{}, scte35NullCommand(), nil)
		section[len(section)-1] ^= 0xff // corrupt the last CRC byte
		buf.write(scte35TestCuePIDA, true, section)

		// Legal PES on the video PID and a legal cue afterwards.
		pes := scte35TestPES(bytes.Repeat([]byte{0xaa}, 32))
		buf.write(scte35TestVideoPID, true, pes)
		buf.write(scte35TestCuePIDA, true,
			scte35BuildSection(scte35SectionOptions{}, scte35TimeSignalCommand(500, true), nil))

		dmx := buf.demuxer()
		scte35Next(t, dmx) // PAT
		scte35Next(t, dmx) // PMT

		bad, err := dmx.NextData()
		assert.Nil(t, bad)
		assert.Error(t, err)
		assert.True(t, errors.Is(err, ErrCRC32Mismatch))

		media, err := dmx.NextData()
		require.NoError(t, err)
		assert.NotNil(t, media.PES)
		assert.Equal(t, scte35TestVideoPID, media.PID)
		assert.Equal(t, bytes.Repeat([]byte{0xaa}, 32), media.PES.Data)

		cue, err := dmx.NextData()
		require.NoError(t, err)
		assert.NotNil(t, cue.SCTE35)
		assert.Equal(t, uint64(500), cue.SCTE35.Command.TimeSignal.SpliceTime.PTS)
	})

	// A structurally invalid section (command region overruns its length) errors
	// out even with a valid wire CRC, and the next cue/PES are still delivered.
	t.Run("bad_length_then_legal_data", func(t *testing.T) {
		buf := newSCTE35TSBuffer()
		buf.writePreamble(scte35DefaultStreams())

		// Build a section manually with an oversized splice_command_length.
		body := []byte{
			0x00,             // protocol_version 0
			0x00, 0, 0, 0, 0, // pts_adjustment 0
			0x00,       // cw_index
			0xf0, 0x00, // tier 0
			0xf2, 0x00, // splice_command_length = 0x200 (overruns section)
			SCTE35SpliceCommandTypeNull,
			0x00, 0x00, // descriptor_loop_length = 0
		}
		sectionLength := len(body) + 4
		section := []byte{0xfc, byte(sectionLength >> 8), byte(sectionLength)}
		section = append(section, body...)
		crc := scte35TestCRC32(section)
		section = append(section, byte(crc>>24), byte(crc>>16), byte(crc>>8), byte(crc))
		buf.write(scte35TestCuePIDA, true, section)

		buf.write(scte35TestCuePIDA, true,
			scte35BuildSection(scte35SectionOptions{}, scte35NullCommand(), nil))
		buf.write(scte35TestVideoPID, true, scte35TestPES([]byte{0x42}))

		dmx := buf.demuxer()
		scte35Next(t, dmx)
		scte35Next(t, dmx)

		bad, err := dmx.NextData()
		assert.Nil(t, bad)
		assert.Error(t, err)
		assert.True(t, errors.Is(err, ErrSCTE35InvalidSection))

		cue, err := dmx.NextData()
		require.NoError(t, err)
		assert.NotNil(t, cue.SCTE35)
		assert.NotNil(t, cue.SCTE35.Command.Null)

		media, err := dmx.NextData()
		require.NoError(t, err)
		assert.NotNil(t, media.PES)
	})

	// Two cue PIDs interleaved, each section spanning several TS packets.
	t.Run("two_cue_pids_interleaved_across_packets", func(t *testing.T) {
		buf := newSCTE35TSBuffer()
		buf.writePreamble(scte35DefaultStreams())

		// A 220-byte descriptor pushes each section past the 184-byte payload size.
		bigDescriptor := scte35Descriptor(0x80, make([]byte, 220))
		sectionA := scte35BuildSection(scte35SectionOptions{}, scte35NullCommand(), bigDescriptor)
		sectionB := scte35BuildSection(scte35SectionOptions{},
			scte35TimeSignalCommand(777, true), bigDescriptor)
		assert.Greater(t, len(sectionA), 184)
		assert.Greater(t, len(sectionB), 184)

		packetsA := scte35TSPackets(scte35TestCuePIDA, true, sectionA, 0)
		packetsB := scte35TSPackets(scte35TestCuePIDB, true, sectionB, 0)
		assert.Greater(t, len(packetsA), 1)
		assert.Greater(t, len(packetsB), 1)

		// Strict packet-level interleaving of the two PIDs.
		n := len(packetsA)
		if len(packetsB) > n {
			n = len(packetsB)
		}
		for k := 0; k < n; k++ {
			if k < len(packetsA) {
				buf.writeRaw(packetsA[k])
			}
			if k < len(packetsB) {
				buf.writeRaw(packetsB[k])
			}
		}

		dmx := buf.demuxer()
		scte35Next(t, dmx)
		scte35Next(t, dmx)

		var gotA, gotB *DemuxerData
		for k := 0; k < 2; k++ {
			d, err := dmx.NextData()
			require.NoError(t, err)
			switch d.PID {
			case scte35TestCuePIDA:
				gotA = d
			case scte35TestCuePIDB:
				gotB = d
			default:
				t.Fatalf("unexpected PID %d", d.PID)
			}
		}
		assert.NotNil(t, gotA)
		assert.NotNil(t, gotB)
		assert.True(t, gotA.FirstPacket.Header.PayloadUnitStartIndicator)
		assert.True(t, gotB.FirstPacket.Header.PayloadUnitStartIndicator)
		assert.NotNil(t, gotA.SCTE35.Command.Null)
		assert.Equal(t, uint64(777), gotB.SCTE35.Command.TimeSignal.SpliceTime.PTS)
		assert.Equal(t, sectionA, gotA.SCTE35.Section)
		assert.Equal(t, sectionB, gotB.SCTE35.Section)
	})

	// Unsupported envelopes produce specific errors and never guessed parsing.
	t.Run("unsupported_envelopes", func(t *testing.T) {
		cases := []struct {
			name    string
			opts    scte35SectionOptions
			wantErr error
		}{
			{"encrypted", scte35SectionOptions{encrypted: true}, ErrSCTE35Encrypted},
			{"protocol_version_1", scte35SectionOptions{protocol: 1}, ErrSCTE35UnsupportedVersion},
			{"unbounded_command", scte35SectionOptions{unbounded: true}, ErrSCTE35UnboundedCommand},
		}
		for _, tc := range cases {
			t.Run(tc.name, func(t *testing.T) {
				buf := newSCTE35TSBuffer()
				buf.writePreamble(scte35DefaultStreams())
				buf.write(scte35TestCuePIDA, true,
					scte35BuildSection(tc.opts, append([]byte{SCTE35SpliceCommandTypeNull}, 0x00, 0x00), nil))

				dmx := buf.demuxer()
				scte35Next(t, dmx)
				scte35Next(t, dmx)
				d, err := dmx.NextData()
				assert.Nil(t, d)
				assert.Error(t, err)
				assert.True(t, errors.Is(err, tc.wantErr))

				// Recovery: a later valid cue is still delivered.
				buf2 := newSCTE35TSBuffer()
				buf2.writePreamble(scte35DefaultStreams())
				buf2.write(scte35TestCuePIDA, true,
					scte35BuildSection(scte35SectionOptions{}, scte35NullCommand(), nil))
				dmx2 := buf2.demuxer()
				scte35Next(t, dmx2)
				scte35Next(t, dmx2)
				ok, err := dmx2.NextData()
				require.NoError(t, err)
				assert.NotNil(t, ok.SCTE35)
			})
		}
	})

	// Only PIDs declared with stream_type 0x86 in the PMT are treated as SCTE-35.
	t.Run("only_recognized_on_stream_type_0x86", func(t *testing.T) {
		buf := newSCTE35TSBuffer()
		buf.writePreamble(scte35DefaultStreams())

		lookalike := scte35BuildSection(scte35SectionOptions{}, scte35NullCommand(), nil)
		buf.write(scte35TestOtherPID, true, lookalike) // declared as private data (0x06)

		dmx := buf.demuxer()
		scte35Next(t, dmx)
		scte35Next(t, dmx)
		d, err := dmx.NextData()
		assert.Equal(t, ErrNoMorePackets, err)
		assert.Nil(t, d)
	})

	// Table id other than 0xFC on a declared SCTE-35 PID is a recognizable error;
	// no half object is delivered, and the following valid cue still comes through.
	t.Run("wrong_table_id_and_stuffing", func(t *testing.T) {
		buf := newSCTE35TSBuffer()
		buf.writePreamble(scte35DefaultStreams())

		section := scte35BuildSection(scte35SectionOptions{}, scte35NullCommand(), nil)
		payload := append([]byte{}, section...)
		payload = append(payload, 0x03, 0x00, 0x11) // wrong table id, framed section length
		buf.write(scte35TestCuePIDA, true, payload)
		buf.write(scte35TestCuePIDA, true,
			scte35BuildSection(scte35SectionOptions{}, scte35TimeSignalCommand(600, true), nil))

		dmx := buf.demuxer()
		scte35Next(t, dmx)
		scte35Next(t, dmx)
		bad, err := dmx.NextData()
		assert.Nil(t, bad)
		assert.Error(t, err)
		assert.True(t, errors.Is(err, ErrSCTE35InvalidSection))

		cue, err := dmx.NextData()
		require.NoError(t, err)
		assert.NotNil(t, cue.SCTE35)
		assert.Equal(t, uint64(600), cue.SCTE35.Command.TimeSignal.SpliceTime.PTS)
	})
}
