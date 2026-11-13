package flv_test

import (
	"bytes"
	"io"
	"testing"
	"time"

	"github.com/notedit/rtmp/av"
	"github.com/notedit/rtmp/codec/hevc"
	"github.com/notedit/rtmp/format/flv"
	"github.com/notedit/rtmp/format/flv/flvio"
	"github.com/notedit/rtmp/format/rtmp"
	"github.com/notedit/rtmp/task_support/taskcheck"
)

// ---------------------------------------------------------------------------
// Independent byte constructors. None of these go through the production
// reader/writer: FLV tags, Enhanced RTMP v2 video bodies and RTMP chunks are
// assembled and inspected by hand.
// ---------------------------------------------------------------------------

func putU24(b []byte, v uint32) {
	b[0] = byte(v >> 16)
	b[1] = byte(v >> 8)
	b[2] = byte(v)
}

func putI24(b []byte, v int32) {
	b[0] = byte(v >> 16)
	b[1] = byte(v >> 8)
	b[2] = byte(v)
}

func flvFileHeader() []byte {
	return []byte{'F', 'L', 'V', 1, 1, 0, 0, 0, 9, 0, 0, 0, 0}
}

// flvTag wraps one already serialized tag body with an 11 byte FLV tag
// header and the 4 byte previous-tag-size trailer.
func flvTag(tagType byte, ts uint32, body []byte) []byte {
	tag := make([]byte, 11+len(body)+4)
	tag[0] = tagType
	putU24(tag[1:], uint32(len(body)))
	putU24(tag[4:], ts&0xffffff)
	tag[7] = byte(ts >> 24)
	copy(tag[11:], body)
	prev := uint32(11 + len(body))
	tag[11+len(body)+0] = byte(prev >> 24)
	tag[11+len(body)+1] = byte(prev >> 16)
	tag[11+len(body)+2] = byte(prev >> 8)
	tag[11+len(body)+3] = byte(prev)
	return tag
}

func flvContainer(bodies ...[]byte) []byte {
	out := append([]byte(nil), flvFileHeader()...)
	for _, body := range bodies {
		out = append(out, body...)
	}
	return out
}

// flvTagStream concatenates bare tags (no FLV file header) so it can be
// appended to an already-headed stream.
func flvTagStream(bodies ...[]byte) []byte {
	var out []byte
	for _, body := range bodies {
		out = append(out, body...)
	}
	return out
}

// splitFLVTags is a hand-written FLV parser used to inspect muxer output.
func splitFLVTags(raw []byte) (bodies [][]byte, err error) {
	if len(raw) < 13 || string(raw[:3]) != "FLV" {
		return nil, io.ErrUnexpectedEOF
	}
	off := 13
	for off < len(raw) {
		if len(raw)-off < 15 {
			return nil, io.ErrUnexpectedEOF
		}
		datalen := int(raw[off+1])<<16 | int(raw[off+2])<<8 | int(raw[off+3])
		if len(raw)-off < 11+datalen+4 {
			return nil, io.ErrUnexpectedEOF
		}
		bodies = append(bodies, raw[off+11:off+11+datalen])
		off += 11 + datalen + 4
	}
	return
}

// buildHVCC serializes a minimal but structurally valid
// HEVCDecoderConfigurationRecord whose NAL length prefixes use width bytes
// (legal values: 1, 2 or 4).
func buildHVCC(width byte, nalus ...[]byte) []byte {
	b := make([]byte, 0, 23+4*len(nalus))
	fixed := make([]byte, 23)
	fixed[0] = 1 // configurationVersion
	fixed[13] = 0xf0
	fixed[15] = 0xfc
	fixed[16] = 0xfc
	fixed[17] = 0xf8
	fixed[18] = 0xf8
	fixed[21] = 0xfc | (width-1)&3
	fixed[22] = byte(len(nalus))
	b = append(b, fixed...)
	for _, nalu := range nalus {
		b = append(b, 0x80|(nalu[0]>>1)) // array_completeness + NAL type
		b = append(b, 0, 1)              // one NAL in this array
		b = append(b, byte(len(nalu)>>8), byte(len(nalu)))
		b = append(b, nalu...)
	}
	return b
}

// buildFrame prefixes every NAL with a width-byte big-endian length.
func buildFrame(width int, nalus ...[]byte) []byte {
	n := 0
	for _, nalu := range nalus {
		n += width + len(nalu)
	}
	b := make([]byte, 0, n)
	for _, nalu := range nalus {
		l := len(nalu)
		switch width {
		case 1:
			b = append(b, byte(l))
		case 2:
			b = append(b, byte(l>>8), byte(l))
		case 4:
			b = append(b, byte(l>>24), byte(l>>16), byte(l>>8), byte(l))
		}
		b = append(b, nalu...)
	}
	return b
}

func enhPlain(frameType, packetType byte, payload []byte) []byte {
	b := []byte{flvio.EnhancedVideoMarker | (frameType&7)<<4 | packetType&0xf, 'h', 'v', 'c', '1'}
	return append(b, payload...)
}

func enhOneTrack(frameType, innerType, track byte, payload []byte) []byte {
	b := []byte{
		flvio.EnhancedVideoMarker | (frameType&7)<<4 | flvio.EnhancedMultitrack,
		'h', 'v', 'c', '1',
		(flvio.EnhancedOneTrack << 4) | innerType&0xf,
		track,
	}
	return append(b, payload...)
}

type manyItem struct {
	track byte
	body  []byte
}

func enhManyTracks(frameType, innerType byte, items []manyItem) []byte {
	b := []byte{
		flvio.EnhancedVideoMarker | (frameType&7)<<4 | flvio.EnhancedMultitrack,
		'h', 'v', 'c', '1',
		(flvio.EnhancedManyTracks << 4) | innerType&0xf,
	}
	for _, item := range items {
		b = append(b, item.track)
		b = append(b, byte(len(item.body)>>16), byte(len(item.body)>>8), byte(len(item.body)))
		b = append(b, item.body...)
	}
	return b
}

func codedPayload(ctime int32, frame []byte) []byte {
	b := make([]byte, 3)
	putI24(b, ctime)
	return append(b, frame...)
}

func legacyAVCTagBody(avcc []byte) []byte {
	b := []byte{0x17, 0x01, 0, 0, 0} // keyframe, AVC NALU, ctime 0
	return append(b, avcc...)
}

// --- hand-written RTMP chunk codec ----------------------------------------

type rtmpMsg struct {
	csid   uint32
	typeID byte
	ts     uint32
	extTS  bool
	msgLen uint32
	body   []byte
	chunks int
}

// encodeRTMP chunks one message exactly like the wire format, using the
// supplied chunk size and extended timestamps when ts >= 0xffffff.
func encodeRTMP(csid byte, ts uint32, typeID byte, msgID uint32, data []byte, chunkSize int) []byte {
	out := []byte{csid} // fmt 0, one-byte csid
	tsField := ts
	ext := ts >= 0xffffff
	if ext {
		tsField = 0xffffff
	}
	hdr := make([]byte, 11)
	putU24(hdr[0:], tsField)
	putU24(hdr[3:], uint32(len(data)))
	hdr[6] = typeID
	hdr[7] = byte(msgID)
	hdr[8] = byte(msgID >> 8)
	hdr[9] = byte(msgID >> 16)
	hdr[10] = byte(msgID >> 24)
	out = append(out, hdr...)
	if ext {
		out = append(out, byte(ts>>24), byte(ts>>16), byte(ts>>8), byte(ts))
	}

	off := 0
	first := true
	for off < len(data) {
		if !first {
			out = append(out, 0xc0|csid) // fmt 3
			if ext {
				out = append(out, byte(ts>>24), byte(ts>>16), byte(ts>>8), byte(ts))
			}
		}
		n := len(data) - off
		if n > chunkSize {
			n = chunkSize
		}
		out = append(out, data[off:off+n]...)
		off += n
		first = false
	}
	return out
}

// parseRTMP reassembles messages by hand and remembers how many chunks each
// one occupied. SetChunkSize events update the chunk size used afterwards.
func parseRTMP(raw []byte) (msgs []rtmpMsg, err error) {
	p := 0
	chunkSize := 128
	pending := map[uint32]*rtmpMsg{}
	for p < len(raw) {
		if p+1 > len(raw) {
			return nil, io.ErrUnexpectedEOF
		}
		fmt := raw[p] >> 6
		csid := uint32(raw[p] & 0x3f)
		p++
		if csid < 2 {
			return nil, io.ErrUnexpectedEOF // only one-byte csids are used here
		}

		if fmt == 0 {
			if p+11 > len(raw) {
				return nil, io.ErrUnexpectedEOF
			}
			ts := uint32(raw[p])<<16 | uint32(raw[p+1])<<8 | uint32(raw[p+2])
			msgLen := uint32(raw[p+3])<<16 | uint32(raw[p+4])<<8 | uint32(raw[p+5])
			typeID := raw[p+6]
			p += 11
			ext := false
			if ts == 0xffffff {
				if p+4 > len(raw) {
					return nil, io.ErrUnexpectedEOF
				}
				ts = uint32(raw[p])<<24 | uint32(raw[p+1])<<16 | uint32(raw[p+2])<<8 | uint32(raw[p+3])
				p += 4
				ext = true
			}
			pending[csid] = &rtmpMsg{csid: csid, typeID: typeID, ts: ts, extTS: ext, msgLen: msgLen}
		} else if fmt != 3 {
			return nil, io.ErrUnexpectedEOF // the tests only use fmt 0/3
		}

		m := pending[csid]
		if m == nil {
			return nil, io.ErrUnexpectedEOF
		}
		if m.extTS && fmt == 3 {
			if p+4 > len(raw) {
				return nil, io.ErrUnexpectedEOF
			}
			p += 4
		}

		left := int(m.msgLen) - len(m.body)
		n := left
		if n > chunkSize {
			n = chunkSize
		}
		if p+n > len(raw) {
			return nil, io.ErrUnexpectedEOF
		}
		m.body = append(m.body, raw[p:p+n]...)
		m.chunks++
		p += n

		if uint32(len(m.body)) == m.msgLen {
			if m.typeID == 1 { // SetChunkSize
				chunkSize = int(uint32(m.body[0])<<24 | uint32(m.body[1])<<16 | uint32(m.body[2])<<8 | uint32(m.body[3]))
			}
			msgs = append(msgs, *m)
			delete(pending, csid)
		}
	}
	return
}

// --- test transports -------------------------------------------------------

type wire struct {
	bytes.Buffer
}

func (w *wire) Flush() error { return nil }

type shortReadWriter struct {
	buf *bytes.Buffer
	max int
}

func (s *shortReadWriter) Read(p []byte) (int, error) {
	if len(p) > s.max {
		p = p[:s.max]
	}
	return s.buf.Read(p)
}

func (s *shortReadWriter) Write(p []byte) (int, error) { return s.buf.Write(p) }
func (s *shortReadWriter) Flush() error                { return nil }

type shortReader struct {
	r   io.Reader
	max int
}

func (s *shortReader) Read(p []byte) (int, error) {
	if len(p) > s.max {
		p = p[:s.max]
	}
	return s.r.Read(p)
}

func readAllPackets(r av.PacketReader) (pkts []av.Packet, err error) {
	for {
		pkt, e := r.ReadPacket()
		if e != nil {
			if e == io.EOF {
				return pkts, nil
			}
			return pkts, e
		}
		pkts = append(pkts, pkt)
	}
}

// ===========================================================================
// Group 1: enhanced header shapes
// ===========================================================================

func TestEnhancedVideoHeaders(t *testing.T) {
	rec := taskcheck.New(t)
	defer rec.Finish()

	hvcc := buildHVCC(4, []byte{0x40, 0x01}, []byte{0x42, 0x02, 0x03})
	frame := buildFrame(4, []byte{0x26, 0xaa}, []byte{0x28, 0xbb})

	// Case 1: plain hvc1 SequenceStart followed by CodedFrames.
	rec.Case("plain hvc1 sequence start and coded frames")
	{
		cfgTag := flvTag(flvio.TAG_VIDEO, 100, enhPlain(flvio.FRAME_KEY, flvio.EnhancedSequenceStart, hvcc))
		frameTag := flvTag(flvio.TAG_VIDEO, 120, enhPlain(flvio.FRAME_INTER, flvio.EnhancedCodedFrames, codedPayload(5, frame)))
		raw := flvContainer(cfgTag, frameTag)

		// Independent inspection of the hand-built input bytes.
		bodies, e := splitFLVTags(raw)
		if e != nil {
			t.Fatal(e)
		}
		rec.Check("plain header byte is 0x90 (key, SequenceStart)", bodies[0][0] == 0x90)
		rec.Check("plain FourCC is hvc1", string(bodies[0][1:5]) == "hvc1")

		pkts, err := readAllPackets(flv.NewDemuxer(bytes.NewReader(raw)))
		rec.Check("demuxer accepts plain enhanced stream", err == nil)
		rec.Check("first packet is HEVC config on track 0", pkts[0].Type == av.HEVCDecoderConfig && pkts[0].TrackID == 0)
		rec.Check("config exposes 4-byte NAL length width", pkts[0].HEVC != nil && pkts[0].HEVC.NALLengthSize == 4)
		rec.Check("config bytes preserved", bytes.Equal(pkts[0].Data, hvcc))
		rec.Check("second packet is an HEVC frame", pkts[1].Type == av.HEVC && pkts[1].TrackID == 0)
		rec.Check("composition time parsed as +5ms", pkts[1].CTime == 5*time.Millisecond)
		rec.Check("decode timestamp is the tag timestamp", pkts[1].Time == 120*time.Millisecond)
		rec.Check("frame payload excludes the composition time prefix", bytes.Equal(pkts[1].Data, frame))
	}

	// Case 2: OneTrack messages on a non-zero track.
	rec.Case("one track header with non-zero track id")
	{
		cfgTag := flvTag(flvio.TAG_VIDEO, 200, enhOneTrack(flvio.FRAME_KEY, flvio.EnhancedSequenceStart, 3, hvcc))
		frameTag := flvTag(flvio.TAG_VIDEO, 240, enhOneTrack(flvio.FRAME_INTER, flvio.EnhancedCodedFrames, 3, codedPayload(-10, frame)))
		raw := flvContainer(cfgTag, frameTag)

		bodies, _ := splitFLVTags(raw)
		rec.Check("OneTrack header byte is 0x96", bodies[0][0] == 0x96)
		rec.Check("OneTrack control byte is 0x00 (OneTrack|SequenceStart)", bodies[0][5] == 0x00)
		rec.Check("track id byte is 3", bodies[0][6] == 3)

		pkts, err := readAllPackets(flv.NewDemuxer(bytes.NewReader(raw)))
		rec.Check("OneTrack stream demuxes", err == nil)
		rec.Check("config carries track id 3", pkts[0].Type == av.HEVCDecoderConfig && pkts[0].TrackID == 3)
		rec.Check("frame carries track id 3 and -10ms ctime", pkts[1].TrackID == 3 && pkts[1].CTime == -10*time.Millisecond)
	}

	// Case 3: one ManyTracks message yields several queued packets.
	rec.Case("many tracks header queues packets in member order")
	{
		hvcc1 := buildHVCC(1, []byte{0x40, 0x11})
		hvcc2 := buildHVCC(4, []byte{0x40, 0x22})
		body := enhManyTracks(flvio.FRAME_KEY, flvio.EnhancedSequenceStart, []manyItem{
			{track: 1, body: hvcc1},
			{track: 2, body: hvcc2},
		})
		tag := flvTag(flvio.TAG_VIDEO, 300, body)
		raw := flvContainer(tag)

		bodies, _ := splitFLVTags(raw)
		rec.Check("ManyTracks control byte is 0x10 (ManyTracks|SequenceStart)", bodies[0][5] == 0x10)
		rec.Check("first item track id and length bytes", bodies[0][6] == 1 &&
			bodies[0][7] == 0 && bodies[0][8] == 0 && bodies[0][9] == byte(len(hvcc1)))

		d := flv.NewDemuxer(bytes.NewReader(raw))
		reads := 0
		r := flv.NewEnhancedPacketReader(func() (flvio.Tag, error) {
			reads++
			return d.ReadTag()
		})
		p1, err := r.ReadPacket()
		rec.Check("first ManyTracks member is track 1 config", err == nil && p1.TrackID == 1 && p1.Type == av.HEVCDecoderConfig)
		p2, err := r.ReadPacket()
		rec.Check("second member is track 2 config", err == nil && p2.TrackID == 2 && p2.Type == av.HEVCDecoderConfig)
		rec.Check("both packets came from one message (one tag read)", reads == 1)
		rec.Check("track 1 uses 1-byte length width", p1.HEVC.NALLengthSize == 1)
		rec.Check("track 2 uses 4-byte length width", p2.HEVC.NALLengthSize == 4)
	}

	// Case 4: negative composition time survives the signed 24-bit field.
	rec.Case("negative composition time")
	{
		body := enhPlain(flvio.FRAME_INTER, flvio.EnhancedCodedFrames, codedPayload(-100, frame))
		raw := flvContainer(flvTag(flvio.TAG_VIDEO, 400, body))

		bodies, _ := splitFLVTags(raw)
		// ctime starts after the 5 byte enhanced header.
		rec.Check("wire ctime bytes are ffff9c for -100",
			bodies[0][5] == 0xff && bodies[0][6] == 0xff && bodies[0][7] == 0x9c)

		pkts, err := readAllPackets(flv.NewDemuxer(bytes.NewReader(raw)))
		rec.Check("config-less frame is rejected", err != nil)
		// Now with a preceding config it must parse the negative ctime.
		raw = flvContainer(
			flvTag(flvio.TAG_VIDEO, 390, enhPlain(flvio.FRAME_KEY, flvio.EnhancedSequenceStart, hvcc)),
			flvTag(flvio.TAG_VIDEO, 400, body),
		)
		pkts, err = readAllPackets(flv.NewDemuxer(bytes.NewReader(raw)))
		rec.Check("negative ctime parsed as -100ms", err == nil && pkts[1].CTime == -100*time.Millisecond)
	}

	// Case 5: truncated headers and out-of-bounds ManyTracks lengths.
	rec.Case("truncated enhanced data is rejected")
	{
		// Enhanced marker present but not enough bytes for the FourCC.
		shortHeader := []byte{flvio.EnhancedVideoMarker, 'h', 'v'}
		raw := flvContainer(flvTag(flvio.TAG_VIDEO, 500, shortHeader))
		_, err := readAllPackets(flv.NewDemuxer(bytes.NewReader(raw)))
		rec.Check("truncated enhanced header returns an error", err != nil)

		bad := []byte{
			flvio.EnhancedVideoMarker | flvio.FRAME_KEY<<4 | flvio.EnhancedMultitrack,
			'h', 'v', 'c', '1',
			flvio.EnhancedManyTracks<<4 | flvio.EnhancedSequenceStart,
			1, 0, 0, 9, // item claims 9 bytes, none follow
		}
		avcTag := flvTag(flvio.TAG_VIDEO, 520, legacyAVCTagBody(buildFrame(4, []byte{0x65, 0x11})))
		raw = flvContainer(flvTag(flvio.TAG_VIDEO, 510, bad), avcTag)
		d := flv.NewDemuxer(bytes.NewReader(raw))
		_, err = d.ReadPacket()
		rec.Check("ManyTracks length overrun returns an error", err != nil)
		pkt, err := d.ReadPacket()
		rec.Check("stream stays aligned: following legacy AVC tag still reads", err == nil && pkt.Type == av.H264)
	}
}

// ===========================================================================
// Group 2: per-track configuration state
// ===========================================================================

func TestEnhancedVideoTracks(t *testing.T) {
	rec := taskcheck.New(t)
	defer rec.Finish()

	// Case 1: two tracks in one message, different NAL length widths.
	rec.Case("two tracks with different nal length widths")
	{
		hvcc1 := buildHVCC(1, []byte{0x40, 1})
		hvcc2 := buildHVCC(4, []byte{0x40, 2})
		frame1 := buildFrame(1, []byte{0x26, 0xaa}, []byte{0x27, 0xbb})
		frame2 := buildFrame(4, []byte{0x26, 0xcc})

		cfgMsg := enhManyTracks(flvio.FRAME_KEY, flvio.EnhancedSequenceStart, []manyItem{
			{track: 1, body: hvcc1},
			{track: 2, body: hvcc2},
		})
		frameMsg := enhManyTracks(flvio.FRAME_INTER, flvio.EnhancedCodedFrames, []manyItem{
			{track: 1, body: codedPayload(0, frame1)},
			{track: 2, body: codedPayload(7, frame2)},
		})
		raw := flvContainer(
			flvTag(flvio.TAG_VIDEO, 0, cfgMsg),
			flvTag(flvio.TAG_VIDEO, 40, frameMsg),
		)
		pkts, err := readAllPackets(flv.NewDemuxer(bytes.NewReader(raw)))
		rec.Check("both tracks configure and frame without error", err == nil && len(pkts) == 4)
		rec.Check("track 1 width is 1 byte", pkts[0].HEVC.NALLengthSize == 1)
		rec.Check("track 2 width is 4 bytes", pkts[1].HEVC.NALLengthSize == 4)
		rec.Check("track 1 frame validated with 1-byte prefixes", bytes.Equal(pkts[2].Data, frame1))
		rec.Check("track 2 frame validated with 4-byte prefixes", bytes.Equal(pkts[3].Data, frame2))
		rec.Check("track 2 keeps its own ctime", pkts[3].CTime == 7*time.Millisecond)

		// A 4-byte-prefixed frame must not be accepted on the 1-byte track.
		wrong := enhOneTrack(flvio.FRAME_INTER, flvio.EnhancedCodedFrames, 1, codedPayload(0, frame2))
		raw = flvContainer(
			flvTag(flvio.TAG_VIDEO, 0, cfgMsg),
			flvTag(flvio.TAG_VIDEO, 40, wrong),
		)
		_, err = readAllPackets(flv.NewDemuxer(bytes.NewReader(raw)))
		rec.Check("track 1 rejects another track's 4-byte framing", err != nil)
	}

	// Case 2: the same track switches configuration, new width applies at once.
	rec.Case("configuration switch on one track takes effect immediately")
	{
		oldHVCC := buildHVCC(4, []byte{0x40, 1})
		newHVCC := buildHVCC(1, []byte{0x42, 9})
		raw := flvContainer(
			flvTag(flvio.TAG_VIDEO, 0, enhPlain(flvio.FRAME_KEY, flvio.EnhancedSequenceStart, oldHVCC)),
			flvTag(flvio.TAG_VIDEO, 40, enhPlain(flvio.FRAME_INTER, flvio.EnhancedCodedFrames, codedPayload(0, buildFrame(4, []byte{0x26, 1})))),
			flvTag(flvio.TAG_VIDEO, 80, enhPlain(flvio.FRAME_KEY, flvio.EnhancedSequenceStart, newHVCC)),
		)
		d := flv.NewDemuxer(bytes.NewReader(raw))
		oldCfg, err := d.ReadPacket()
		rec.Check("initial config is 4-byte width", err == nil && oldCfg.HEVC.NALLengthSize == 4)
		oldFrame, err := d.ReadPacket()
		rec.Check("frame under old config accepted", err == nil && oldFrame.Type == av.HEVC)
		newCfg, err := d.ReadPacket()
		rec.Check("switched config is 1-byte width", err == nil && newCfg.HEVC.NALLengthSize == 1)

		okMsg := flvTag(flvio.TAG_VIDEO, 120, enhPlain(flvio.FRAME_INTER, flvio.EnhancedCodedFrames, codedPayload(0, buildFrame(1, []byte{0x27, 2}))))
		badMsg := flvTag(flvio.TAG_VIDEO, 160, enhPlain(flvio.FRAME_INTER, flvio.EnhancedCodedFrames, codedPayload(0, buildFrame(4, []byte{0x27, 2}))))
		// Clean stream: the 1-byte frame is accepted under the new config.
		raw2 := flvContainer(
			flvTag(flvio.TAG_VIDEO, 0, enhPlain(flvio.FRAME_KEY, flvio.EnhancedSequenceStart, newHVCC)),
			okMsg,
		)
		pkts, err := readAllPackets(flv.NewDemuxer(bytes.NewReader(raw2)))
		rec.Check("1-byte frame accepted after switch", err == nil && len(pkts) == 2)
		// Fresh demuxer where the bad 4-byte frame is the first frame after switch.
		raw3 := flvContainer(
			flvTag(flvio.TAG_VIDEO, 0, enhPlain(flvio.FRAME_KEY, flvio.EnhancedSequenceStart, newHVCC)),
			badMsg,
		)
		_, err = readAllPackets(flv.NewDemuxer(bytes.NewReader(raw3)))
		rec.Check("4-byte frame rejected after switch to 1-byte width", err != nil)
		rec.Check("handed-out snapshot still records the old 4-byte width", oldFrame.HEVC.NALLengthSize == 4)
	}

	// Case 3: snapshots are independent copies.
	rec.Case("packet snapshots survive config switches and buffer reuse")
	{
		hvcc := buildHVCC(4, []byte{0x40, 7})
		frame := buildFrame(4, []byte{0x26, 0xde, 0xad})
		raw := flvContainer(
			flvTag(flvio.TAG_VIDEO, 0, enhPlain(flvio.FRAME_KEY, flvio.EnhancedSequenceStart, hvcc)),
			flvTag(flvio.TAG_VIDEO, 40, enhPlain(flvio.FRAME_INTER, flvio.EnhancedCodedFrames, codedPayload(0, frame))),
		)
		pkts, err := readAllPackets(flv.NewDemuxer(bytes.NewReader(raw)))
		rec.Check("baseline packets read", err == nil && len(pkts) == 2)
		cfgPkt, framePkt := pkts[0], pkts[1]

		for i := range raw {
			raw[i] = 0
		}
		rec.Check("config snapshot bytes survive source buffer reuse", bytes.Equal(cfgPkt.HEVC.ConfigBytes, hvcc))
		rec.Check("frame bytes survive source buffer reuse", bytes.Equal(framePkt.Data, frame))

		newHVCC := buildHVCC(1, []byte{0x42, 8})
		raw2 := flvContainer(
			flvTag(flvio.TAG_VIDEO, 0, enhPlain(flvio.FRAME_KEY, flvio.EnhancedSequenceStart, newHVCC)),
			flvTag(flvio.TAG_VIDEO, 40, enhPlain(flvio.FRAME_INTER, flvio.EnhancedCodedFrames, codedPayload(0, buildFrame(1, []byte{0x27, 1})))),
		)
		pkts2, err := readAllPackets(flv.NewDemuxer(bytes.NewReader(raw2)))
		rec.Check("new track state is independent", err == nil && pkts2[0].HEVC.NALLengthSize == 1)
		rec.Check("old frame snapshot unchanged after another demuxer switched config", framePkt.HEVC.NALLengthSize == 4)
	}

	// Case 4: SequenceEnd removes only that track's state.
	rec.Case("sequence end closes one track without touching others")
	{
		hvcc5 := buildHVCC(4, []byte{0x40, 5})
		hvcc6 := buildHVCC(1, []byte{0x40, 6})
		frame5 := buildFrame(4, []byte{0x26, 5})
		frame6 := buildFrame(1, []byte{0x26, 6})
		setup := flvContainer(
			flvTag(flvio.TAG_VIDEO, 0, enhOneTrack(flvio.FRAME_KEY, flvio.EnhancedSequenceStart, 5, hvcc5)),
			flvTag(flvio.TAG_VIDEO, 1, enhOneTrack(flvio.FRAME_KEY, flvio.EnhancedSequenceStart, 6, hvcc6)),
			flvTag(flvio.TAG_VIDEO, 2, enhOneTrack(flvio.FRAME_INTER, flvio.EnhancedCodedFrames, 5, codedPayload(0, frame5))),
			flvTag(flvio.TAG_VIDEO, 3, enhOneTrack(flvio.FRAME_KEY, flvio.EnhancedSequenceEnd, 5, nil)),
		)
		pkts, err := readAllPackets(flv.NewDemuxer(bytes.NewReader(setup)))
		rec.Check("setup messages read", err == nil && len(pkts) == 4)
		rec.Check("SequenceEnd packet typed and tracked", pkts[3].Type == av.HEVCEndOfSequence && pkts[3].TrackID == 5)

		// After the end: track 5's frame fails, on a continued tag stream.
		track5Follow := flvTagStream(
			flvTag(flvio.TAG_VIDEO, 4, enhOneTrack(flvio.FRAME_INTER, flvio.EnhancedCodedFrames, 5, codedPayload(0, frame5))),
		)
		d := flv.NewDemuxer(io.MultiReader(bytes.NewReader(setup), bytes.NewReader(track5Follow)))
		for i := 0; i < 4; i++ { // drain the four setup packets
			if _, e := d.ReadPacket(); e != nil {
				t.Fatal(e)
			}
		}
		_, err5 := d.ReadPacket()
		rec.Check("frame on ended track 5 is rejected", err5 != nil)

		// A fresh demuxer over the same setup followed by track 6's frame
		// confirms the other track is unaffected by the SequenceEnd.
		track6Follow := flvTagStream(
			flvTag(flvio.TAG_VIDEO, 5, enhOneTrack(flvio.FRAME_INTER, flvio.EnhancedCodedFrames, 6, codedPayload(0, frame6))),
		)
		d2 := flv.NewDemuxer(io.MultiReader(bytes.NewReader(setup), bytes.NewReader(track6Follow)))
		var track6 av.Packet
		for {
			p, e := d2.ReadPacket()
			if e == io.EOF {
				break
			}
			if e != nil {
				t.Fatalf("track 6 should stay valid: %v", e)
			}
			if p.TrackID == 6 && p.Type == av.HEVC {
				track6 = p
			}
		}
		rec.Check("unrelated track 6 still accepts frames", track6.Type == av.HEVC && bytes.Equal(track6.Data, frame6))

		// Re-configuring track 5 makes it usable again.
		reconfig := flvContainer(
			flvTag(flvio.TAG_VIDEO, 6, enhOneTrack(flvio.FRAME_KEY, flvio.EnhancedSequenceStart, 5, hvcc5)),
			flvTag(flvio.TAG_VIDEO, 7, enhOneTrack(flvio.FRAME_INTER, flvio.EnhancedCodedFrames, 5, codedPayload(0, frame5))),
		)
		pkts, err = readAllPackets(flv.NewDemuxer(bytes.NewReader(reconfig)))
		rec.Check("track 5 accepts frames again after a new config", err == nil && len(pkts) == 2 && pkts[1].TrackID == 5)
	}

	// Case 5: a failed multi-track message must not commit half its state.
	rec.Case("corrupted message never pollutes configuration")
	{
		goodHVCC8 := buildHVCC(1, []byte{0x40, 8})
		badHVCC := append([]byte(nil), buildHVCC(4, []byte{0x40, 7})...)
		badHVCC[0] = 2 // illegal configurationVersion

		// First member would configure track 7, second member is invalid:
		// a frame for track 9 that was never configured.
		corrupt := enhManyTracks(flvio.FRAME_KEY, flvio.EnhancedSequenceStart, []manyItem{
			{track: 7, body: buildHVCC(4, []byte{0x40, 7})},
			{track: 9, body: badHVCC},
		})
		frame7 := buildFrame(4, []byte{0x26, 7})
		raw := flvContainer(
			flvTag(flvio.TAG_VIDEO, 0, enhOneTrack(flvio.FRAME_KEY, flvio.EnhancedSequenceStart, 8, goodHVCC8)),
			flvTag(flvio.TAG_VIDEO, 1, corrupt),
		)
		d := flv.NewDemuxer(bytes.NewReader(raw))
		pkt, err := d.ReadPacket()
		rec.Check("track 8 configured before the bad message", err == nil && pkt.TrackID == 8)
		var zero av.Packet
		pkt, err = d.ReadPacket()
		rec.Check("bad message returns an error", err != nil)
		rec.Check("no packet is handed out for the bad message", pkt.Type == zero.Type)

		// Track 7 must remain unconfigured; track 8 must remain usable.
		raw2 := flvContainer(
			flvTag(flvio.TAG_VIDEO, 2, enhOneTrack(flvio.FRAME_INTER, flvio.EnhancedCodedFrames, 7, codedPayload(0, frame7))),
			flvTag(flvio.TAG_VIDEO, 3, enhOneTrack(flvio.FRAME_INTER, flvio.EnhancedCodedFrames, 8, codedPayload(0, buildFrame(1, []byte{0x26, 8})))),
		)
		d2 := flv.NewDemuxer(io.MultiReader(bytes.NewReader(raw), bytes.NewReader(raw2)))
		var firstErr error
		for {
			_, e := d2.ReadPacket()
			if e == io.EOF {
				break
			}
			if e != nil && firstErr == nil {
				firstErr = e
			}
		}
		rec.Check("track 7 stayed unconfigured after the failed commit", firstErr != nil)

		okOnly := flvContainer(
			flvTag(flvio.TAG_VIDEO, 0, enhOneTrack(flvio.FRAME_KEY, flvio.EnhancedSequenceStart, 8, goodHVCC8)),
			flvTag(flvio.TAG_VIDEO, 1, enhOneTrack(flvio.FRAME_INTER, flvio.EnhancedCodedFrames, 8, codedPayload(0, buildFrame(1, []byte{0x26, 8})))),
		)
		pkts, err := readAllPackets(flv.NewDemuxer(bytes.NewReader(okOnly)))
		rec.Check("previously valid track 8 configuration is intact", err == nil && len(pkts) == 2)
	}

	// Case 6: unknown FourCC, unsupported modes and unconfigured tracks.
	rec.Case("unknown fourcc and unsupported modes fail explicitly")
	{
		unknown := append([]byte{
			flvio.EnhancedVideoMarker | flvio.FRAME_KEY<<4 | flvio.EnhancedSequenceStart,
			'h', 'v', 'c', '2',
		}, hvccPlaceholder()...)
		raw := flvContainer(flvTag(flvio.TAG_VIDEO, 0, unknown))
		_, err := readAllPackets(flv.NewDemuxer(bytes.NewReader(raw)))
		rec.Check("unknown FourCC rejected", err != nil)

		badMode := []byte{
			flvio.EnhancedVideoMarker | flvio.FRAME_KEY<<4 | flvio.EnhancedMultitrack,
			'h', 'v', 'c', '1',
			2<<4 | flvio.EnhancedSequenceStart,
			1,
		}
		raw = flvContainer(flvTag(flvio.TAG_VIDEO, 0, badMode))
		_, err = readAllPackets(flv.NewDemuxer(bytes.NewReader(raw)))
		rec.Check("unsupported multitrack mode rejected", err != nil)

		raw = flvContainer(flvTag(flvio.TAG_VIDEO, 0,
			enhPlain(flvio.FRAME_INTER, flvio.EnhancedCodedFrames, codedPayload(0, buildFrame(4, []byte{1})))))
		_, err = readAllPackets(flv.NewDemuxer(bytes.NewReader(raw)))
		rec.Check("frame on unconfigured track rejected", err != nil)
	}

	// Case 7: HVCC validation and the legal 1/2/4-byte length widths.
	rec.Case("hvcc validation and two byte nal width")
	{
		// The 2-byte width works end to end.
		hvcc2 := buildHVCC(2, []byte{0x40, 1})
		frame2 := buildFrame(2, []byte{0x26, 0xaa}, []byte{0x27, 0xbb, 0xcc})
		raw := flvContainer(
			flvTag(flvio.TAG_VIDEO, 0, enhPlain(flvio.FRAME_KEY, flvio.EnhancedSequenceStart, hvcc2)),
			flvTag(flvio.TAG_VIDEO, 40, enhPlain(flvio.FRAME_INTER, flvio.EnhancedCodedFrames, codedPayload(0, frame2))),
		)
		pkts, err := readAllPackets(flv.NewDemuxer(bytes.NewReader(raw)))
		rec.Check("2-byte width config parses", err == nil && pkts[0].HEVC.NALLengthSize == 2)
		rec.Check("2-byte width frame round trips", len(pkts) == 2 && bytes.Equal(pkts[1].Data, frame2))

		// lengthSizeMinusOne == 2 means a 3-byte width, which is not supported.
		illegalWidth := buildHVCC(4, []byte{0x40, 1})
		illegalWidth[21] = 0xfc | 2
		raw = flvContainer(flvTag(flvio.TAG_VIDEO, 0, enhPlain(flvio.FRAME_KEY, flvio.EnhancedSequenceStart, illegalWidth)))
		_, err = readAllPackets(flv.NewDemuxer(bytes.NewReader(raw)))
		rec.Check("3-byte NAL length width rejected", err != nil)

		// Wrong version.
		badVersion := buildHVCC(4, []byte{0x40, 1})
		badVersion[0] = 7
		_, err = hevc.ParseDecoderConfig(badVersion)
		rec.Check("illegal HVCC version rejected", err != nil)

		// Truncated fixed header.
		_, err = hevc.ParseDecoderConfig(buildHVCC(4, []byte{0x40, 1})[:10])
		rec.Check("truncated HVCC fixed header rejected", err != nil)

		// A NAL length that runs past the record is rejected.
		overrun := make([]byte, len(hvcc2))
		copy(overrun, hvcc2)
		overrun[len(overrun)-3] = 0x7f // inflate the 16-bit NAL length
		_, err = hevc.ParseDecoderConfig(overrun)
		rec.Check("HVCC NAL length overrun rejected", err != nil)

		// A frame whose last length prefix overruns its bytes is rejected.
		goodCfg := buildHVCC(1, []byte{0x40, 1})
		badFrame := []byte{2, 0x26, 0x27, 5, 0x28} // second NAL claims 5 bytes, only 1 present
		raw = flvContainer(
			flvTag(flvio.TAG_VIDEO, 0, enhPlain(flvio.FRAME_KEY, flvio.EnhancedSequenceStart, goodCfg)),
			flvTag(flvio.TAG_VIDEO, 40, enhPlain(flvio.FRAME_INTER, flvio.EnhancedCodedFrames, codedPayload(0, badFrame))),
		)
		_, err = readAllPackets(flv.NewDemuxer(bytes.NewReader(raw)))
		rec.Check("frame NAL overrun rejected without emitting packets", err != nil)
	}
}

func hvccPlaceholder() []byte {
	return buildHVCC(4, []byte{0x40, 1})
}

// ===========================================================================
// Group 3: full muxer/demuxer and RTMP conn streams
// ===========================================================================

func TestEnhancedVideoStream(t *testing.T) {
	rec := taskcheck.New(t)
	defer rec.Finish()

	hvcc := buildHVCC(4, []byte{0x40, 0x01}, []byte{0x42, 0x02})
	frame := buildFrame(4, []byte{0x26, 0xaa, 0xbb})
	avcc := buildFrame(4, []byte{0x65, 0x11, 0x22})

	// Case 1: real FLV muxer -> bytes inspected by hand -> real demuxer.
	rec.Case("flv muxer and demuxer round trip with legacy follow-up")
	{
		var buf bytes.Buffer
		m := flv.NewMuxer(&buf)
		m.HasVideo = true
		packets := []av.Packet{
			{Type: av.HEVCDecoderConfig, IsKeyFrame: true, TrackID: 0, Data: hvcc, HEVC: mustHEVC(t, hvcc)},
			{Type: av.HEVC, IsKeyFrame: false, TrackID: 0, Time: 40 * time.Millisecond, CTime: -3 * time.Millisecond, Data: frame, HEVC: mustHEVC(t, hvcc)},
			{Type: av.HEVCDecoderConfig, IsKeyFrame: true, TrackID: 3, Data: hvcc, HEVC: mustHEVC(t, hvcc)},
			{Type: av.HEVCEndOfSequence, IsKeyFrame: true, TrackID: 3},
			{Type: av.H264DecoderConfig, Data: buildFrame(4, []byte{0x67, 1})},
			{Type: av.H264, IsKeyFrame: true, Time: 80 * time.Millisecond, Data: avcc},
		}
		for _, p := range packets {
			if err := m.WritePacket(p); err != nil {
				t.Fatal(err)
			}
		}

		bodies, err := splitFLVTags(buf.Bytes())
		rec.Check("muxer emitted one tag per packet", err == nil && len(bodies) == len(packets))
		rec.Check("plain enhanced header bytes on the wire", bodies[0][0] == 0x90 && string(bodies[0][1:5]) == "hvc1")
		rec.Check("OneTrack header bytes for track 3", bodies[2][0] == 0x96 && bodies[2][5] == 0x00 && bodies[2][6] == 3)
		rec.Check("legacy AVC tag still starts with 0x17", bodies[5][0] == 0x17)

		d := flv.NewDemuxer(bytes.NewReader(buf.Bytes()))
		got, err := readAllPackets(d)
		rec.Check("demuxer reads every packet back", err == nil && len(got) == len(packets))
		rec.Check("HEVC types and track ids preserved", got[0].Type == av.HEVCDecoderConfig && got[2].TrackID == 3 && got[3].Type == av.HEVCEndOfSequence)
		rec.Check("negative ctime and frame bytes preserved", got[1].CTime == -3*time.Millisecond && bytes.Equal(got[1].Data, frame))
		rec.Check("legacy AVC frame follows enhanced data", got[5].Type == av.H264 && bytes.Equal(got[5].Data, avcc) && got[5].IsKeyFrame)
	}

	// Case 2: real RTMP conn, chunk size smaller than the enhanced header.
	rec.Case("rtmp conn with chunk size below enhanced header length")
	{
		w := new(wire)
		tx := rtmp.NewConn(w)
		tx.Stage = rtmp.StageDataStart
		err := tx.SetWriteChunkSize(4)
		rec.Check("writer announces 4-byte chunks", err == nil)

		cfgPkt := av.Packet{Type: av.HEVCDecoderConfig, IsKeyFrame: true, TrackID: 2, Time: 100 * time.Millisecond, Data: hvcc, HEVC: mustHEVC(t, hvcc)}
		framePkt := av.Packet{Type: av.HEVC, IsKeyFrame: false, TrackID: 2, Time: 140 * time.Millisecond, CTime: 9 * time.Millisecond, Data: frame, HEVC: mustHEVC(t, hvcc)}
		if err := tx.WritePacket(cfgPkt); err != nil {
			t.Fatal(err)
		}
		if err := tx.WritePacket(framePkt); err != nil {
			t.Fatal(err)
		}

		msgs, err := parseRTMP(w.Bytes())
		rec.Check("hand parser decodes the chunk stream", err == nil && len(msgs) >= 3)
		var videoMsgs []rtmpMsg
		for _, m := range msgs {
			if m.typeID == flvio.TAG_VIDEO {
				videoMsgs = append(videoMsgs, m)
			}
		}
		rec.Check("two video messages were chunked", len(videoMsgs) == 2)
		rec.Check("small chunks actually used (more chunks than payload bytes/128)", videoMsgs[0].chunks > 1)
		rec.Check("message length covers header plus payload", int(videoMsgs[0].msgLen) == 7+len(hvcc))
		rec.Check("OneTrack header survives chunk boundaries", videoMsgs[0].body[0] == 0x96 && string(videoMsgs[0].body[1:5]) == "hvc1" && videoMsgs[0].body[5] == 0x00 && videoMsgs[0].body[6] == 2)

		rx := rtmp.NewConn(w)
		rx.Stage = rtmp.StageDataStart
		gotCfg, err := rx.ReadPacket()
		rec.Check("receiver rebuilds config packet", err == nil && gotCfg.Type == av.HEVCDecoderConfig && gotCfg.TrackID == 2)
		gotFrame, err := rx.ReadPacket()
		rec.Check("receiver rebuilds frame with ctime and bytes", err == nil && gotFrame.TrackID == 2 && gotFrame.CTime == 9*time.Millisecond && bytes.Equal(gotFrame.Data, frame))
	}

	// Case 3: ManyTracks over real RTMP bytes, plus a one-byte-at-a-time reader.
	rec.Case("many tracks over rtmp and short reader")
	{
		hvcc1 := buildHVCC(1, []byte{0x40, 1})
		hvcc2 := buildHVCC(4, []byte{0x40, 2})
		manyBody := enhManyTracks(flvio.FRAME_KEY, flvio.EnhancedSequenceStart, []manyItem{
			{track: 1, body: hvcc1},
			{track: 2, body: hvcc2},
		})
		avcBody := legacyAVCTagBody(avcc)
		raw := append(
			encodeRTMP(6, 1000, flvio.TAG_VIDEO, 1, manyBody, 128),
			encodeRTMP(6, 1040, flvio.TAG_VIDEO, 1, avcBody, 128)...,
		)

		// Short reader: every Read call returns at most one byte.
		buf := bytes.NewBuffer(raw)
		sw := &shortReadWriter{buf: buf, max: 1}
		rx := rtmp.NewConn(sw)
		rx.Stage = rtmp.StageDataStart
		p1, err := rx.ReadPacket()
		rec.Check("first ManyTracks packet via byte-at-a-time reader", err == nil && p1.TrackID == 1 && p1.Type == av.HEVCDecoderConfig)
		p2, err := rx.ReadPacket()
		rec.Check("second ManyTracks packet keeps member order", err == nil && p2.TrackID == 2 && p2.Type == av.HEVCDecoderConfig)
		p3, err := rx.ReadPacket()
		rec.Check("legacy AVC after enhanced RTMP message", err == nil && p3.Type == av.H264 && bytes.Equal(p3.Data, avcc))

		// Same byte stream through a short FLV-style reader check is covered
		// by wrapping a container; here verify FLV demuxer short reads too.
		flvRaw := flvContainer(
			flvTag(flvio.TAG_VIDEO, 0, manyBody),
			flvTag(flvio.TAG_VIDEO, 1, avcBody),
		)
		pkts, err := readAllPackets(flv.NewDemuxer(&shortReader{r: bytes.NewReader(flvRaw), max: 3}))
		rec.Check("flv demuxer tolerates 3-byte reads", err == nil && len(pkts) == 3 && pkts[2].Type == av.H264)
	}

	// Case 4: extended timestamps on both paths.
	rec.Case("extended timestamps on rtmp and flv")
	{
		w := new(wire)
		tx := rtmp.NewConn(w)
		tx.Stage = rtmp.StageDataStart
		bigTS := uint32(0x1000004)
		// The frame needs a preceding SequenceStart on the same connection.
		cfgPkt := av.Packet{Type: av.HEVCDecoderConfig, IsKeyFrame: true, TrackID: 0, Time: time.Duration(bigTS-40) * time.Millisecond, Data: hvcc, HEVC: mustHEVC(t, hvcc)}
		pkt := av.Packet{Type: av.HEVC, IsKeyFrame: true, TrackID: 0, Time: time.Duration(bigTS) * time.Millisecond, Data: frame, HEVC: mustHEVC(t, hvcc)}
		if err := tx.WritePacket(cfgPkt); err != nil {
			t.Fatal(err)
		}
		if err := tx.WritePacket(pkt); err != nil {
			t.Fatal(err)
		}
		raw := w.Bytes()
		// Both timestamps exceed 0xffffff, so the ffffff marker and the
		// extended timestamp bytes must appear on the wire.
		ffffff := []byte{0xff, 0xff, 0xff}
		extBig := []byte{0x01, 0x00, 0x00, 0x04}
		rec.Check("rtmp timestamp field uses ffffff marker", bytes.Contains(raw, ffffff))
		rec.Check("rtmp extended timestamp carries the full value", bytes.Contains(raw, extBig))

		msgs, err := parseRTMP(raw)
		var videoMsgs2 []rtmpMsg
		for _, m := range msgs {
			if m.typeID == flvio.TAG_VIDEO {
				videoMsgs2 = append(videoMsgs2, m)
			}
		}
		rec.Check("hand parser sees the extended timestamp", err == nil && len(videoMsgs2) == 2 && videoMsgs2[1].extTS && videoMsgs2[1].ts == bigTS)

		// Re-feed the same bytes through a fresh conn with small reads.
		rx := rtmp.NewConn(&shortReadWriter{buf: bytes.NewBuffer(raw), max: 5})
		rx.Stage = rtmp.StageDataStart
		if _, err = rx.ReadPacket(); err != nil {
			t.Fatal(err)
		}
		got, err := rx.ReadPacket()
		rec.Check("receiver restores the extended decode timestamp", err == nil && uint32(got.Time/time.Millisecond) == bigTS)

		// FLV path: the high timestamp byte lives in the tag header.
		flvRaw := flvContainer(flvTag(flvio.TAG_VIDEO, bigTS, enhPlain(flvio.FRAME_KEY, flvio.EnhancedCodedFramesX, frame)))
		// CodedFramesX needs a config first; add it in the same container.
		flvRaw = flvContainer(
			flvTag(flvio.TAG_VIDEO, bigTS-40, enhPlain(flvio.FRAME_KEY, flvio.EnhancedSequenceStart, hvcc)),
			flvTag(flvio.TAG_VIDEO, bigTS, enhPlain(flvio.FRAME_KEY, flvio.EnhancedCodedFramesX, frame)),
		)
		pkts, err := readAllPackets(flv.NewDemuxer(bytes.NewReader(flvRaw)))
		rec.Check("flv demuxer restores the extended timestamp and zero ctime",
			err == nil && uint32(pkts[1].Time/time.Millisecond) == bigTS && pkts[1].CTime == 0)
	}

	// Case 5: malformed enhanced RTMP messages then normal traffic continues.
	rec.Case("rtmp error is explicit and the stream stays aligned")
	{
		truncated := []byte{flvio.EnhancedVideoMarker, 'h', 'v'}
		badFourCC := append([]byte{
			flvio.EnhancedVideoMarker | flvio.FRAME_KEY<<4 | flvio.EnhancedSequenceStart,
			'h', 'v', 'c', '2',
		}, hvcc...)
		raw := append(
			encodeRTMP(6, 1960, flvio.TAG_VIDEO, 1, truncated, 128),
			encodeRTMP(6, 2000, flvio.TAG_VIDEO, 1, badFourCC, 128)...)
		raw = append(raw,
			encodeRTMP(6, 2040, flvio.TAG_VIDEO, 1, legacyAVCTagBody(avcc), 128)...)
		rx := rtmp.NewConn(&shortReadWriter{buf: bytes.NewBuffer(raw), max: 7})
		rx.Stage = rtmp.StageDataStart
		_, err := rx.ReadPacket()
		rec.Check("truncated enhanced header over RTMP returns an error", err != nil)
		_, err = rx.ReadPacket()
		rec.Check("unknown FourCC over RTMP returns an error", err != nil)
		pkt, err := rx.ReadPacket()
		rec.Check("following legacy AVC message still decodes", err == nil && pkt.Type == av.H264 && bytes.Equal(pkt.Data, avcc))
	}
}

func mustHEVC(t *testing.T, hvcc []byte) *hevc.Codec {
	t.Helper()
	c, err := hevc.ParseDecoderConfig(hvcc)
	if err != nil {
		t.Fatalf("build HEVC codec: %v", err)
	}
	return c
}
