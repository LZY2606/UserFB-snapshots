package flv_test

import (
	"bytes"
	"errors"
	"io"
	"testing"
	"time"

	"github.com/notedit/rtmp/av"
	"github.com/notedit/rtmp/format/flv"
	"github.com/notedit/rtmp/format/flv/flvio"
	"github.com/notedit/rtmp/format/rtmp"
	"github.com/notedit/rtmp/task_support/taskcheck"
)

/*
 * Independent byte builders. These intentionally do not call the enhanced
 * muxer/parser code; tests can therefore compare output against bytes built
 * directly from Enhanced RTMP v2.
 */

func putU16(b []byte, v int) {
	b[0] = byte(v >> 8)
	b[1] = byte(v)
}

func putU24(b []byte, v int) {
	b[0] = byte(v >> 16)
	b[1] = byte(v >> 8)
	b[2] = byte(v)
}

func putU32(b []byte, v uint32) {
	b[0] = byte(v >> 24)
	b[1] = byte(v >> 16)
	b[2] = byte(v >> 8)
	b[3] = byte(v)
}

func u24(b []byte) int {
	return int(uint32(b[0])<<16 | uint32(b[1])<<8 | uint32(b[2]))
}

func i24Bytes(v int32) []byte {
	b := make([]byte, 3)
	putU24(b, int(v&0xffffff))
	return b
}

func appendLengthPrefixed(out []byte, width int, nals ...[]byte) []byte {
	for _, nal := range nals {
		l := len(nal)
		switch width {
		case 1:
			out = append(out, byte(l))
		case 2:
			var lb [2]byte
			putU16(lb[:], l)
			out = append(out, lb[:]...)
		case 4:
			var lb [4]byte
			putU32(lb[:], uint32(l))
			out = append(out, lb[:]...)
		default:
			panic("bad test width")
		}
		out = append(out, nal...)
	}
	return out
}

func testNAL(marker byte, n int) []byte {
	b := []byte{0x40, 0x01, marker}
	for i := 0; i < n; i++ {
		b = append(b, byte(0xa0+i))
	}
	return b
}

// buildHEVCConfig builds a structurally valid HEVCDecoderConfigurationRecord
// with one array and one NAL unit.
func buildHEVCConfig(width int, marker byte) []byte {
	b := make([]byte, 23)
	b[0] = 1 // configurationVersion
	b[1] = 1
	b[12] = 30 // general_level_idc
	b[13] = 0xf0
	b[15] = 0xfc
	b[16] = 0xfc
	b[17] = 0xf8
	b[18] = 0xf8
	switch width {
	case 1:
		b[21] = 0
	case 2:
		b[21] = 1
	case 4:
		b[21] = 3
	default:
		panic("unsupported test width")
	}
	b[22] = 1 // numOfArrays

	nal := testNAL(marker, 2)
	array := []byte{0xa0, 0x00, 0x01}
	var l [2]byte
	putU16(l[:], len(nal))
	array = append(array, l[:]...)
	array = append(array, nal...)
	return append(b, array...)
}

func buildHEVCFrame(width int, marker byte) []byte {
	nal1 := []byte{0x02, 0x01, marker, 0x10, 0x11}
	nal2 := []byte{0x02, 0x01, marker ^ 0xff, 0x20}
	return appendLengthPrefixed(nil, width, nal1, nal2)
}

func codedContent(ctsMS int32, frame []byte) []byte {
	return append(i24Bytes(ctsMS), frame...)
}

func exPlain(packetType, frameType byte, body []byte) []byte {
	out := []byte{0x80 | (frameType << 4) | packetType}
	out = append(out, "hvc1"...)
	return append(out, body...)
}

func exOneTrack(frameType, innerType, track byte, content []byte) []byte {
	out := []byte{0x80 | (frameType << 4) | flvio.EX_PACKET_MULTITRACK}
	out = append(out, innerType&0x0f)
	out = append(out, "hvc1"...)
	out = append(out, track)
	return append(out, content...)
}

type mtItem struct {
	track   byte
	content []byte
}

func exManyTracks(frameType, innerType byte, items ...mtItem) []byte {
	out := []byte{0x80 | (frameType << 4) | flvio.EX_PACKET_MULTITRACK}
	out = append(out, 0x10|(innerType&0x0f))
	out = append(out, "hvc1"...)
	for _, item := range items {
		out = append(out, item.track)
		var l [3]byte
		putU24(l[:], len(item.content))
		out = append(out, l[:]...)
		out = append(out, item.content...)
	}
	return out
}

type rawFLVTag struct {
	payload []byte
	ts      uint32
	typ     byte
}

func buildFLV(tags ...rawFLVTag) []byte {
	var buf bytes.Buffer
	// 'FLV', version 1, video flags, header length 9, PreviousTagSize0.
	buf.Write([]byte{'F', 'L', 'V', 0x01, 0x01, 0x00, 0x00, 0x00, 0x09, 0, 0, 0, 0})
	for _, tag := range tags {
		typ := tag.typ
		if typ == 0 {
			typ = flvio.TAG_VIDEO
		}
		var h [11]byte
		h[0] = typ
		putU24(h[1:4], len(tag.payload))
		putU24(h[4:7], int(tag.ts&0xffffff))
		h[7] = byte(tag.ts >> 24)
		buf.Write(h[:])
		buf.Write(tag.payload)
		var prev [4]byte
		putU32(prev[:], uint32(11+len(tag.payload)))
		buf.Write(prev[:])
	}
	return buf.Bytes()
}

func firstFLVTagPayload(flvBytes []byte, index int) []byte {
	off := 13
	for i := 0; ; i++ {
		if off+11 > len(flvBytes) {
			return nil
		}
		n := u24(flvBytes[off+1 : off+4])
		start := off + 11
		end := start + n
		if end > len(flvBytes) {
			return nil
		}
		if i == index {
			return flvBytes[start:end]
		}
		off = end + 4
	}
}

type tagQueue struct {
	tags []flvio.Tag
	pos  int
}

func (q *tagQueue) next() (flvio.Tag, error) {
	if q.pos >= len(q.tags) {
		return flvio.Tag{}, io.EOF
	}
	tag := q.tags[q.pos]
	q.pos++
	return tag, nil
}

func rawTag(raw []byte, ts uint32) flvio.Tag {
	tag := flvio.Tag{Type: flvio.TAG_VIDEO, Time: ts}
	if err := tag.Parse(raw); err != nil {
		panic(err)
	}
	return tag
}

func ms(d time.Duration) int64 {
	return int64(d / time.Millisecond)
}

/*
 * Header tests.
 */

func TestEnhancedVideoHeaders(t *testing.T) {
	rec := taskcheck.New(t, "TestEnhancedVideoHeaders")
	defer rec.Finish()

	// Case 1: ordinary enhanced hvc1 SequenceStart written by FLV muxer.
	c := rec.Case("plain enhanced hvc1 header")
	cfg := buildHEVCConfig(4, 0x11)
	var buf bytes.Buffer
	mux := flv.NewMuxer(&buf)
	mux.HasVideo = true
	if err := mux.WritePacket(av.Packet{Type: av.HEVCDecoderConfig, Time: 123 * time.Millisecond, Data: cfg}); err != nil {
		t.Fatal(err)
	}
	payload := firstFLVTagPayload(buf.Bytes(), 0)
	c.Check("payload has full configuration", len(payload) >= 5+len(cfg))
	if len(payload) >= 5 {
		c.Check("enhanced key SequenceStart first byte", payload[0] == 0x90, payload[0])
		c.Check("hvc1 FourCC", string(payload[1:5]) == "hvc1", string(payload[1:5]))
		c.Check("HVCC record follows FourCC", bytes.Equal(payload[5:], cfg))
	}

	dem := flv.NewDemuxer(bytes.NewReader(buf.Bytes()))
	pkt, err := dem.ReadPacket()
	c.Check("muxed config reads back", err == nil)
	c.Check("read config type", pkt.Type == av.HEVCDecoderConfig, pkt.Type)
	c.Check("default track is zero", pkt.TrackID == 0, pkt.TrackID)

	// Case 2: OneTrack for nonzero track, including negative composition time.
	c = rec.Case("OneTrack header and negative composition time")
	frame := buildHEVCFrame(4, 0x22)
	buf.Reset()
	mux = flv.NewMuxer(&buf)
	trackCfg := buildHEVCConfig(4, 0x23)
	if err := mux.WritePacket(av.Packet{Type: av.HEVCDecoderConfig, TrackID: 3, Time: 190 * time.Millisecond, Data: trackCfg}); err != nil {
		t.Fatal(err)
	}
	p := av.Packet{
		Type: av.HEVC, IsKeyFrame: true, TrackID: 3,
		Time: 200 * time.Millisecond, CTime: -50 * time.Millisecond, Data: frame,
	}
	if err := mux.WritePacket(p); err != nil {
		t.Fatal(err)
	}
	payload = firstFLVTagPayload(buf.Bytes(), 1)
	c.Check("OneTrack outer byte", len(payload) > 0 && payload[0] == 0x96, payload)
	if len(payload) >= 10 {
		c.Check("OneTrack mode with CodedFrames", payload[1] == 0x01, payload[1])
		c.Check("shared hvc1 FourCC", string(payload[2:6]) == "hvc1")
		c.Check("one-byte track id", payload[6] == 3, payload[6])
		c.Check("signed -50ms composition bytes", bytes.Equal(payload[7:10], []byte{0xff, 0xff, 0xce}), payload[7:10])
		c.Check("NAL payload follows composition time", bytes.Equal(payload[10:], frame))
	}
	dem = flv.NewDemuxer(bytes.NewReader(buf.Bytes()))
	if _, err = dem.ReadPacket(); err != nil {
		t.Fatal(err)
	}
	pkt, err = dem.ReadPacket()
	c.Check("OneTrack packet reads back", err == nil)
	c.Check("track 3 preserved", pkt.TrackID == 3)
	c.Check("negative CTime preserved", ms(pkt.CTime) == -50, pkt.CTime)
	c.Check("DTS is not replaced by CTS", ms(pkt.Time) == 200 && ms(pkt.Time+pkt.CTime) == 150)

	// Case 3: independently constructed ManyTracks input.
	c = rec.Case("ManyTracks header layout")
	cfg0 := buildHEVCConfig(4, 0x31)
	cfg1 := buildHEVCConfig(1, 0x41)
	rawMany := exManyTracks(1, flvio.EX_PACKET_SEQUENCE_START,
		mtItem{track: 0, content: cfg0},
		mtItem{track: 7, content: cfg1},
	)
	c.Check("ManyTracks outer byte", rawMany[0] == 0x90|0x06, rawMany[0])
	c.Check("ManyTracks mode and SequenceStart", rawMany[1] == 0x10, rawMany[1])
	c.Check("ManyTracks shared FourCC", string(rawMany[2:6]) == "hvc1")
	c.Check("first item track", rawMany[6] == 0)
	c.Check("first item 24-bit length", u24(rawMany[7:10]) == len(cfg0))
	c.Check("second item track at declared boundary", rawMany[10+len(cfg0)] == 7)

	dem = flv.NewDemuxer(bytes.NewReader(buildFLV(rawFLVTag{payload: rawMany, ts: 40})))
	p0, err0 := dem.ReadPacket()
	p1, err1 := dem.ReadPacket()
	c.Check("first ManyTracks config", err0 == nil)
	c.Check("second ManyTracks config", err1 == nil)
	c.Check("ManyTracks order/tracks", p0.TrackID == 0 && p1.TrackID == 7)
	c.Check("ManyTracks config widths", p0.HEVC.NALLengthSize == 4 && p1.HEVC.NALLengthSize == 1)

	// Case 4: negative composition time with separate DTS/PTS.
	c = rec.Case("negative composition time semantics")
	rawCfg := exPlain(flvio.EX_PACKET_SEQUENCE_START, 1, cfg)
	rawFrame := exPlain(flvio.EX_PACKET_CODED_FRAMES, 1, codedContent(-50, frame))
	flvBytes := buildFLV(
		rawFLVTag{payload: rawCfg, ts: 10},
		rawFLVTag{payload: rawFrame, ts: 100},
	)
	dem = flv.NewDemuxer(bytes.NewReader(flvBytes))
	if _, err := dem.ReadPacket(); err != nil {
		t.Fatal(err)
	}
	pkt, err = dem.ReadPacket()
	c.Check("negative CTime frame accepted", err == nil)
	c.Check("CTime is -50ms", ms(pkt.CTime) == -50)
	c.Check("outer timestamp remains DTS 100ms", ms(pkt.Time) == 100)
	c.Check("display time is DTS+CTS=50ms", ms(pkt.Time+pkt.CTime) == 50)
	c.Check("composition bytes are sign extended", bytes.Contains(rawFrame, []byte{0xff, 0xff, 0xce}))

	// Case 5: truncation and out-of-bounds lengths.
	c = rec.Case("truncated enhanced messages are errors")
	truncFourCC := []byte{0x90, 'h', 'v'}
	dem = flv.NewDemuxer(bytes.NewReader(buildFLV(rawFLVTag{payload: truncFourCC})))
	_, err = dem.ReadPacket()
	c.Check("truncated FourCC rejected", err != nil)
	truncConfig := exPlain(flvio.EX_PACKET_SEQUENCE_START, 1, cfg[:len(cfg)-3])
	dem = flv.NewDemuxer(bytes.NewReader(buildFLV(rawFLVTag{payload: truncConfig})))
	_, err = dem.ReadPacket()
	c.Check("truncated HVCC rejected", err != nil)
	badMany := []byte{0x96, 0x10, 'h', 'v', 'c', '1', 9, 0xff, 0xff, 0xff, 0x00}
	dem = flv.NewDemuxer(bytes.NewReader(buildFLV(rawFLVTag{payload: badMany})))
	_, err = dem.ReadPacket()
	c.Check("ManyTracks length overflow rejected", err != nil)
	truncCT := exPlain(flvio.EX_PACKET_CODED_FRAMES, 1, []byte{0, 0})
	dem = flv.NewDemuxer(bytes.NewReader(buildFLV(
		rawFLVTag{payload: rawCfg},
		rawFLVTag{payload: truncCT, ts: 2},
	)))
	_, err = dem.ReadPacket() // config
	if err != nil {
		t.Fatal(err)
	}
	_, err = dem.ReadPacket()
	c.Check("truncated composition time rejected", err != nil)
}

/*
 * Track/state tests.
 */

func TestEnhancedVideoTracks(t *testing.T) {
	rec := taskcheck.New(t, "TestEnhancedVideoTracks")
	defer rec.Finish()

	// Case 1: two tracks use independent NAL length widths.
	c := rec.Case("two tracks use different NAL width state")
	cfg4 := buildHEVCConfig(4, 0x11)
	cfg1 := buildHEVCConfig(1, 0x22)
	configs := exManyTracks(1, flvio.EX_PACKET_SEQUENCE_START,
		mtItem{track: 0, content: cfg4},
		mtItem{track: 5, content: cfg1},
	)
	frame4 := buildHEVCFrame(4, 0x33)
	frame1 := buildHEVCFrame(1, 0x44)
	frames := exManyTracks(1, flvio.EX_PACKET_CODED_FRAMES,
		mtItem{track: 0, content: codedContent(4, frame4)},
		mtItem{track: 5, content: codedContent(-6, frame1)},
	)
	q := &tagQueue{tags: []flvio.Tag{rawTag(configs, 10), rawTag(frames, 40)}}
	dec := flv.NewPacketDecoder()
	pc0, e0 := dec.ReadPacket(q.next)
	pc1, e1 := dec.ReadPacket(q.next)
	pf0, e2 := dec.ReadPacket(q.next)
	pf1, e3 := dec.ReadPacket(q.next)
	c.Check("config track 0", e0 == nil)
	c.Check("config track 5", e1 == nil)
	c.Check("frame track 0", e2 == nil)
	c.Check("frame track 5", e3 == nil)
	c.Check("config tracks ordered", pc0.TrackID == 0 && pc1.TrackID == 5)
	c.Check("independent widths 4 and 1", pc0.HEVC.NALLengthSize == 4 && pc1.HEVC.NALLengthSize == 1)
	c.Check("frame order follows member order", pf0.TrackID == 0 && pf1.TrackID == 5)
	c.Check("frame 0 uses width 4 snapshot", pf0.HEVC.NALLengthSize == 4)
	c.Check("frame 5 uses width 1 snapshot", pf1.HEVC.NALLengthSize == 1)
	c.Check("frame 0 bytes preserved", bytes.Equal(pf0.Data, frame4))
	c.Check("frame 5 bytes preserved", bytes.Equal(pf1.Data, frame1))
	c.Check("independent composition offsets", ms(pf0.CTime) == 4 && ms(pf1.CTime) == -6)

	// Case 2: replacing a track configuration changes width immediately.
	c = rec.Case("same track configuration switch is immediate")
	oldCfg := buildHEVCConfig(4, 0x55)
	newCfg := buildHEVCConfig(2, 0x66)
	oldFrame := buildHEVCFrame(4, 0x77)
	newFrame := buildHEVCFrame(2, 0x88)
	q = &tagQueue{tags: []flvio.Tag{
		rawTag(exPlain(0, 1, oldCfg), 1),
		rawTag(exPlain(1, 1, codedContent(0, oldFrame)), 2),
		rawTag(exPlain(0, 1, newCfg), 3),
		rawTag(exPlain(1, 1, codedContent(0, newFrame)), 4),
	}}
	dec = flv.NewPacketDecoder()
	oc, err := dec.ReadPacket(q.next)
	of, err2 := dec.ReadPacket(q.next)
	nc, err3 := dec.ReadPacket(q.next)
	nf, err4 := dec.ReadPacket(q.next)
	c.Check("old config read", err == nil)
	c.Check("old width-4 frame read", err2 == nil)
	c.Check("new config read", err3 == nil)
	c.Check("new width-2 frame read", err4 == nil)
	c.Check("packet order", oc.Type == av.HEVCDecoderConfig && of.Type == av.HEVC && nc.Type == av.HEVCDecoderConfig && nf.Type == av.HEVC)
	c.Check("old snapshot width remains 4", of.HEVC.NALLengthSize == 4)
	c.Check("new config width is 2", nc.HEVC.NALLengthSize == 2)
	c.Check("new frame uses width 2 immediately", nf.HEVC.NALLengthSize == 2)
	c.Check("new frame bytes accepted", bytes.Equal(nf.Data, newFrame))

	// Case 3: delivered snapshots and copied bytes resist mutation.
	c = rec.Case("packet configuration and payload snapshots are independent")
	snapCfg := buildHEVCConfig(4, 0x99)
	snapFrame := buildHEVCFrame(4, 0xaa)
	rawCfg := exPlain(0, 1, snapCfg)
	rawFrame := exPlain(1, 1, codedContent(8, snapFrame))
	q = &tagQueue{tags: []flvio.Tag{rawTag(rawCfg, 5), rawTag(rawFrame, 6)}}
	dec = flv.NewPacketDecoder()
	if _, err := dec.ReadPacket(q.next); err != nil {
		t.Fatal(err)
	}
	snapPkt, err := dec.ReadPacket(q.next)
	c.Check("snapshot frame read", err == nil)
	wantData := append([]byte(nil), snapFrame...)
	wantRaw := append([]byte(nil), snapCfg...)
	for i := range snapCfg {
		snapCfg[i] = 0xff
	}
	for i := range rawFrame {
		rawFrame[i] = 0x00
	}
	for i := range rawCfg {
		rawCfg[i] = 0xee
	}
	c.Check("packet payload copied away from input buffer", bytes.Equal(snapPkt.Data, wantData))
	c.Check("config snapshot raw copied", bytes.Equal(snapPkt.HEVC.Raw, wantRaw))
	c.Check("HEVCConfig field is independent copy", bytes.Equal(snapPkt.HEVCConfig, wantRaw))
	c.Check("snapshot width unchanged after input mutation", snapPkt.HEVC.NALLengthSize == 4)

	replacement := buildHEVCConfig(1, 0xbb)
	q.tags = []flvio.Tag{rawTag(exPlain(0, 1, replacement), 7)}
	q.pos = 0
	_, err = dec.ReadPacket(q.next)
	c.Check("replacement config accepted", err == nil)
	c.Check("delivered snapshot survives active config replacement", snapPkt.HEVC.NALLengthSize == 4 && bytes.Equal(snapPkt.HEVC.Raw, wantRaw))

	// Case 4: duplicate SequenceStart members in one message apply in order.
	c = rec.Case("SequenceStart members apply in member order")
	firstCfg := buildHEVCConfig(4, 0xab)
	secondCfg := buildHEVCConfig(1, 0xcd)
	configOrder := exManyTracks(1, flvio.EX_PACKET_SEQUENCE_START,
		mtItem{track: 4, content: firstCfg},
		mtItem{track: 4, content: secondCfg},
	)
	q = &tagQueue{tags: []flvio.Tag{rawTag(configOrder, 11)}}
	firstPkt, e0 := dec.ReadPacket(q.next)
	secondPkt, e1 := dec.ReadPacket(q.next)
	c.Check("both ordered config packets returned", e0 == nil && e1 == nil)
	c.Check("first packet keeps first snapshot", firstPkt.HEVC.NALLengthSize == 4)
	c.Check("second packet keeps second snapshot", secondPkt.HEVC.NALLengthSize == 1)
	orderedFrame := buildHEVCFrame(1, 0xce)
	q.tags = []flvio.Tag{rawTag(exOneTrack(1, flvio.EX_PACKET_CODED_FRAMES_X, 4, orderedFrame), 12)}
	q.pos = 0
	orderedPkt, err := dec.ReadPacket(q.next)
	c.Check("last SequenceStart wins for active track", err == nil && orderedPkt.HEVC.NALLengthSize == 1)
	c.Check("ordered frame bytes preserved", bytes.Equal(orderedPkt.Data, orderedFrame))

	// Case 5: SequenceEnd clears one track only.
	c = rec.Case("SequenceEnd isolates tracks")
	cfg0 := buildHEVCConfig(4, 0xcc)
	cfg1 = buildHEVCConfig(1, 0xdd)
	q = &tagQueue{tags: []flvio.Tag{
		rawTag(exManyTracks(1, 0, mtItem{track: 0, content: cfg0}, mtItem{track: 8, content: cfg1}), 10),
	}}
	dec = flv.NewPacketDecoder()
	if _, err := dec.ReadPacket(q.next); err != nil {
		t.Fatal(err)
	}
	if _, err := dec.ReadPacket(q.next); err != nil {
		t.Fatal(err)
	}
	end8 := exOneTrack(1, flvio.EX_PACKET_SEQUENCE_END, 8, nil)
	q.tags = []flvio.Tag{rawTag(end8, 20)}
	q.pos = 0
	_, err = dec.ReadPacket(q.next)
	c.Check("SequenceEnd tag is consumed and then EOF", errors.Is(err, io.EOF), err)

	good0 := exPlain(flvio.EX_PACKET_CODED_FRAMES_X, 1, buildHEVCFrame(4, 0xee))
	q.tags = append(q.tags, rawTag(good0, 30))
	pkt, err := dec.ReadPacket(q.next)
	c.Check("other track still configured", err == nil)
	c.Check("other track packet returned", pkt.TrackID == 0 && pkt.Type == av.HEVC)

	bad8 := exOneTrack(1, flvio.EX_PACKET_CODED_FRAMES_X, 8, buildHEVCFrame(1, 0xff))
	q.tags = append(q.tags, rawTag(bad8, 31))
	_, err = dec.ReadPacket(q.next)
	c.Check("ended track requires new SequenceStart", err != nil)
	// Case 5: a bad final member is atomic and does not pollute config.
	c = rec.Case("corrupt final member neither delivers nor half-updates")
	stableCfg := buildHEVCConfig(4, 0x12)
	stableFrame := buildHEVCFrame(4, 0x34)
	q = &tagQueue{tags: []flvio.Tag{
		rawTag(exPlain(0, 1, stableCfg), 1),
		rawTag(exPlain(3, 1, stableFrame), 2),
	}}
	dec = flv.NewPacketDecoder()
	if _, err := dec.ReadPacket(q.next); err != nil {
		t.Fatal(err)
	}
	good, err := dec.ReadPacket(q.next)
	c.Check("stable frame before corruption", err == nil)
	c.Check("stable width is 4", good.HEVC.NALLengthSize == 4)

	wouldSwitch := buildHEVCConfig(1, 0x56)
	badMessage := exManyTracks(1, 0,
		mtItem{track: 0, content: wouldSwitch},
		mtItem{track: 9, content: []byte{0x01, 0x00, 0x00}}, // truncated HVCC fixed header
	)
	q.tags = []flvio.Tag{rawTag(badMessage, 3)}
	q.pos = 0
	badPkt, badErr := dec.ReadPacket(q.next)
	c.Check("malformed final config makes whole message fail", badErr != nil)
	c.Check("failed call returns no packet", badPkt.Type == 0 && len(badPkt.Data) == 0)

	q.tags = []flvio.Tag{rawTag(exPlain(3, 1, stableFrame), 4)}
	q.pos = 0
	after, err := dec.ReadPacket(q.next)
	c.Check("old width-4 config remains valid", err == nil)
	c.Check("failed tentative width switch rolled back", after.HEVC.NALLengthSize == 4)
	c.Check("old frame data still validates and copies", bytes.Equal(after.Data, stableFrame))

	// A coded-frame ManyTracks message must pass association for every member
	// before its first packet is delivered.
	badFrameMessage := exManyTracks(1, flvio.EX_PACKET_CODED_FRAMES_X,
		mtItem{track: 0, content: stableFrame},
		mtItem{track: 9, content: buildHEVCFrame(1, 0x9a)},
	)
	q.tags = []flvio.Tag{rawTag(badFrameMessage, 5)}
	q.pos = 0
	badPkt, badErr = dec.ReadPacket(q.next)
	c.Check("last unconfigured member rejects frame batch", badErr != nil)
	c.Check("frame batch delivers no early packet", badPkt.Type == 0)
	q.tags = []flvio.Tag{rawTag(exPlain(3, 1, stableFrame), 6)}
	q.pos = 0
	after, err = dec.ReadPacket(q.next)
	c.Check("configuration survives rejected frame batch", err == nil && after.HEVC.NALLengthSize == 4)

	// Case 6: explicit structural association errors.
	c = rec.Case("unknown codec and unsupported structures are explicit")
	unknownFourCC := []byte{0x90, 'x', 'x', 'x', 'x'}
	dec = flv.NewPacketDecoder()
	q = &tagQueue{tags: []flvio.Tag{rawTag(unknownFourCC, 0)}}
	_, err = dec.ReadPacket(q.next)
	c.Check("unknown FourCC rejected", err != nil)
	unsupportedPacket := exPlain(7, 1, nil)
	q.tags = []flvio.Tag{rawTag(unsupportedPacket, 0)}
	q.pos = 0
	_, err = dec.ReadPacket(q.next)
	c.Check("unsupported packet type rejected", err != nil)
	badMode := []byte{0x96, 0x20, 'h', 'v', 'c', '1'}
	q.tags = []flvio.Tag{rawTag(badMode, 0)}
	q.pos = 0
	_, err = dec.ReadPacket(q.next)
	c.Check("unsupported multitrack mode rejected", err != nil)
	unconfigured := exOneTrack(1, 3, 2, buildHEVCFrame(1, 0x78))
	q.tags = []flvio.Tag{rawTag(unconfigured, 0)}
	q.pos = 0
	_, err = dec.ReadPacket(q.next)
	c.Check("unconfigured track rejected", err != nil)
	illegalWidthCfg := buildHEVCConfig(4, 0x79)
	illegalWidthCfg[21] = 2 // lengthSizeMinusOne=2 would select an unsupported 3-byte width
	q.tags = []flvio.Tag{rawTag(exPlain(0, 1, illegalWidthCfg), 0)}
	q.pos = 0
	_, err = dec.ReadPacket(q.next)
	c.Check("three-byte NAL length width rejected", err != nil)
	badNAL := exPlain(3, 1, []byte{0x00, 0x00, 0x00, 0x05, 0x40}) // declares five bytes but contains one
	q.tags = []flvio.Tag{
		rawTag(exPlain(0, 1, stableCfg), 0),
		rawTag(badNAL, 1),
	}
	q.pos = 0
	_, _ = dec.ReadPacket(q.next)
	_, err = dec.ReadPacket(q.next)
	c.Check("truncated NAL unit rejected", err != nil)
}

/*
 * Stream/chunk tests and independent RTMP byte reassembly.
 */

type bytesReadWriteFlusher struct {
	buf bytes.Buffer
}

func (b *bytesReadWriteFlusher) Read(p []byte) (int, error)  { return b.buf.Read(p) }
func (b *bytesReadWriteFlusher) Write(p []byte) (int, error) { return b.buf.Write(p) }
func (b *bytesReadWriteFlusher) Flush() error                { return nil }
func (b *bytesReadWriteFlusher) Bytes() []byte               { return b.buf.Bytes() }

type shortReadWriteFlusher struct {
	buf     bytes.Buffer
	readMax int
}

func (s *shortReadWriteFlusher) Read(p []byte) (int, error) {
	if s.readMax > 0 && len(p) > s.readMax {
		p = p[:s.readMax]
	}
	return s.buf.Read(p)
}
func (s *shortReadWriteFlusher) Write(p []byte) (int, error) { return s.buf.Write(p) }
func (s *shortReadWriteFlusher) Flush() error                { return nil }

type reassembledMessage struct {
	csid      uint32
	typeID    byte
	timestamp uint32
	data      []byte
	chunks    int
}

func reassembleRTMP(raw []byte) ([]reassembledMessage, error) {
	type state struct {
		msg  reassembledMessage
		left int
	}
	states := map[uint32]*state{}
	var out []reassembledMessage
	pos := 0
	chunkSize := 128

	for pos < len(raw) {
		start := pos
		b0 := raw[pos]
		pos++
		format := b0 >> 6
		csid := uint32(b0 & 0x3f)
		if csid == 0 {
			if pos >= len(raw) {
				return nil, errors.New("truncated two-byte basic header")
			}
			csid = uint32(raw[pos]) + 64
			pos++
		} else if csid == 1 {
			if pos+2 > len(raw) {
				return nil, errors.New("truncated three-byte basic header")
			}
			csid = uint32(raw[pos])<<8 | uint32(raw[pos+1]) + 64
			pos += 2
		}

		var st *state
		switch format {
		case 0:
			if pos+11 > len(raw) {
				return nil, errors.New("truncated type0 header")
			}
			ts24 := uint32(raw[pos])<<16 | uint32(raw[pos+1])<<8 | uint32(raw[pos+2])
			msgLen := int(raw[pos+3])<<16 | int(raw[pos+4])<<8 | int(raw[pos+5])
			typeID := raw[pos+6]
			pos += 11
			ts := ts24
			if ts24 == 0xffffff {
				if pos+4 > len(raw) {
					return nil, errors.New("truncated extended timestamp")
				}
				ts = uint32(raw[pos])<<24 | uint32(raw[pos+1])<<16 | uint32(raw[pos+2])<<8 | uint32(raw[pos+3])
				pos += 4
			}
			st = &state{msg: reassembledMessage{csid: csid, typeID: typeID, timestamp: ts}, left: msgLen}
			states[csid] = st
		case 3:
			st = states[csid]
			if st == nil {
				return nil, errors.New("type3 chunk without state")
			}
			if st.msg.timestamp >= 0xffffff {
				if pos+4 > len(raw) {
					return nil, errors.New("truncated continuation extended timestamp")
				}
				pos += 4
			}
		default:
			return nil, errors.New("test parser supports only type0/type3")
		}

		n := st.left
		if n > chunkSize {
			n = chunkSize
		}
		if pos+n > len(raw) {
			return nil, errors.New("truncated chunk payload")
		}
		st.msg.data = append(st.msg.data, raw[pos:pos+n]...)
		pos += n
		st.left -= n
		st.msg.chunks++
		if st.left == 0 {
			out = append(out, st.msg)
			if st.msg.typeID == 1 && len(st.msg.data) >= 4 {
				chunkSize = int(uint32(st.msg.data[0])<<24 | uint32(st.msg.data[1])<<16 | uint32(st.msg.data[2])<<8 | uint32(st.msg.data[3]))
			}
		}
		_ = start
	}
	return out, nil
}

func TestEnhancedVideoStream(t *testing.T) {
	rec := taskcheck.New(t, "TestEnhancedVideoStream")
	defer rec.Finish()

	cfg0 := buildHEVCConfig(4, 0xa1)
	cfg2 := buildHEVCConfig(2, 0xa2)
	frame0 := buildHEVCFrame(4, 0xa3)
	frame2 := buildHEVCFrame(2, 0xa4)

	// Case 1: real FLV muxer/demuxer, one ManyTracks message yields packets.
	c := rec.Case("FLV muxer and demuxer preserve one-message order")
	var flvBuf bytes.Buffer
	mux := flv.NewMuxer(&flvBuf)
	mux.HasVideo = true
	configTag := flvio.Tag{
		Type: flvio.TAG_VIDEO, Time: 100, FrameType: flvio.FRAME_KEY,
		Enhanced: true, ExMultitrack: true, ExManyTracks: true, ExInnerPacket: 0, ExFourCC: "hvc1",
		Data: exManyTracks(1, 0, mtItem{track: 0, content: cfg0}, mtItem{track: 2, content: cfg2})[6:],
	}
	frameItems := exManyTracks(1, 1,
		mtItem{track: 0, content: codedContent(10, frame0)},
		mtItem{track: 2, content: codedContent(-12, frame2)},
	)
	frameTag := flvio.Tag{
		Type: flvio.TAG_VIDEO, Time: 140, FrameType: flvio.FRAME_KEY,
		Enhanced: true, ExMultitrack: true, ExManyTracks: true, ExInnerPacket: 1, ExFourCC: "hvc1",
		Data: frameItems[6:],
	}
	c.Check("mux ManyTracks config tag", mux.WriteTag(configTag) == nil)
	c.Check("mux ManyTracks frame tag", mux.WriteTag(frameTag) == nil)
	expectedConfigWire := exManyTracks(1, 0, mtItem{track: 0, content: cfg0}, mtItem{track: 2, content: cfg2})
	expectedFrameWire := frameItems
	c.Check("muxed config output bytes", bytes.Equal(firstFLVTagPayload(flvBuf.Bytes(), 0), expectedConfigWire))
	c.Check("muxed frame output bytes", bytes.Equal(firstFLVTagPayload(flvBuf.Bytes(), 1), expectedFrameWire))

	dem := flv.NewDemuxer(bytes.NewReader(flvBuf.Bytes()))
	ps := make([]av.Packet, 0, 4)
	for i := 0; i < 4; i++ {
		pkt, err := dem.ReadPacket()
		if err != nil {
			t.Fatalf("packet %d: %v", i, err)
		}
		ps = append(ps, pkt)
	}
	c.Check("FLV order tracks", ps[0].TrackID == 0 && ps[1].TrackID == 2 && ps[2].TrackID == 0 && ps[3].TrackID == 2)
	c.Check("FLV order types", ps[0].Type == av.HEVCDecoderConfig && ps[2].Type == av.HEVC)
	c.Check("FLV widths", ps[0].HEVC.NALLengthSize == 4 && ps[1].HEVC.NALLengthSize == 2)
	c.Check("FLV DTS/CTS separation", ms(ps[2].Time) == 140 && ms(ps[2].CTime) == 10 && ms(ps[3].CTime) == -12)

	// Case 2: real RTMP Conn with tiny chunks and independent raw byte parse.
	c = rec.Case("RTMP Conn chunks headers smaller than chunk size")
	rw := &bytesReadWriteFlusher{}
	tx := rtmp.NewConn(rw)
	tx.Stage = rtmp.StageDataStart
	rx := rtmp.NewConn(rw)
	rx.Stage = rtmp.StageDataStart
	c.Check("set tiny write chunk size", tx.SetWriteChunkSize(4) == nil)

	rtmpConfigTag := configTag
	rtmpFrameTag := frameTag
	c.Check("RTMP WriteTag config", tx.WriteTag(rtmpConfigTag) == nil)
	c.Check("RTMP WriteTag frames", tx.WriteTag(rtmpFrameTag) == nil)

	msgs, err := reassembleRTMP(rw.Bytes())
	c.Check("independent RTMP byte reassembly", err == nil)
	var videoMsgs []reassembledMessage
	for _, msg := range msgs {
		if msg.typeID == flvio.TAG_VIDEO {
			videoMsgs = append(videoMsgs, msg)
		}
	}
	c.Check("two video RTMP messages", len(videoMsgs) == 2, len(videoMsgs))
	if len(videoMsgs) == 2 {
		c.Check("config raw message exact", bytes.Equal(videoMsgs[0].data, expectedConfigWire))
		c.Check("frame raw message exact", bytes.Equal(videoMsgs[1].data, expectedFrameWire))
		c.Check("tiny chunk created continuations", videoMsgs[1].chunks >= 4, videoMsgs[1].chunks)
	}

	rps := make([]av.Packet, 0, 4)
	for i := 0; i < 4; i++ {
		pkt, rerr := rx.ReadPacket()
		c.Check("RTMP ordered packet "+string(rune('1'+i)), rerr == nil)
		if rerr == nil {
			rps = append(rps, pkt)
		}
	}
	if len(rps) == 4 {
		c.Check("RTMP track order", rps[0].TrackID == 0 && rps[1].TrackID == 2 && rps[2].TrackID == 0 && rps[3].TrackID == 2)
		c.Check("RTMP payloads", bytes.Equal(rps[2].Data, frame0) && bytes.Equal(rps[3].Data, frame2))
	}

	// Case 3: one-byte reader and OneTrack header split across chunks.
	c = rec.Case("short reader handles split enhanced header")
	srw := &shortReadWriteFlusher{readMax: 1}
	stx := rtmp.NewConn(srw)
	stx.Stage = rtmp.StageDataStart
	srx := rtmp.NewConn(srw)
	srx.Stage = rtmp.StageDataStart
	c.Check("set chunk size five", stx.SetWriteChunkSize(5) == nil)
	track := byte(6)
	c.Check("write track6 config", stx.WritePacket(av.Packet{Type: av.HEVCDecoderConfig, TrackID: track, Time: 50 * time.Millisecond, Data: cfg2}) == nil)
	c.Check("write track6 frame", stx.WritePacket(av.Packet{
		Type: av.HEVC, IsKeyFrame: false, TrackID: track, Time: 80 * time.Millisecond, CTime: 7 * time.Millisecond, Data: frame2,
	}) == nil)
	sp0, serr0 := srx.ReadPacket()
	sp1, serr1 := srx.ReadPacket()
	c.Check("short reader config", serr0 == nil)
	c.Check("short reader frame", serr1 == nil)
	c.Check("short reader track", sp0.TrackID == 6 && sp1.TrackID == 6)
	c.Check("short reader width/payload", sp1.HEVC.NALLengthSize == 2 && bytes.Equal(sp1.Data, frame2))
	c.Check("short reader timing", ms(sp1.Time) == 80 && ms(sp1.CTime) == 7)

	// Case 4: extended timestamp followed by legacy AVC stays aligned.
	c = rec.Case("extended timestamp and following legacy AVC alignment")
	erw := &bytesReadWriteFlusher{}
	etx := rtmp.NewConn(erw)
	etx.Stage = rtmp.StageDataStart
	erx := rtmp.NewConn(erw)
	erx.Stage = rtmp.StageDataStart
	c.Check("small chunks for ext timestamp", etx.SetWriteChunkSize(4) == nil)
	bigDTS := 17_000_000 * time.Millisecond
	c.Check("write ext-time config", etx.WritePacket(av.Packet{Type: av.HEVCDecoderConfig, Time: bigDTS, Data: cfg0}) == nil)
	c.Check("write ext-time HEVC frame", etx.WritePacket(av.Packet{
		Type: av.HEVC, IsKeyFrame: true, Time: bigDTS + 40*time.Millisecond, CTime: 25 * time.Millisecond, Data: frame0,
	}) == nil)
	avcData := bytes.Repeat([]byte{0x00, 0x00, 0x00, 0x01, 0x65}, 20)
	c.Check("write following legacy AVC", etx.WritePacket(av.Packet{
		Type: av.H264, IsKeyFrame: true, Time: 1000 * time.Millisecond, CTime: -2 * time.Millisecond, Data: avcData,
	}) == nil)

	emsgs, eerr := reassembleRTMP(erw.Bytes())
	c.Check("raw ext-timestamp stream reassembles", eerr == nil)
	var evideo []reassembledMessage
	for _, msg := range emsgs {
		if msg.typeID == flvio.TAG_VIDEO {
			evideo = append(evideo, msg)
		}
	}
	c.Check("three video messages after chunk control", len(evideo) == 3, len(evideo))
	if len(evideo) == 3 {
		c.Check("extended DTS visible in raw RTMP", evideo[1].timestamp == uint32(17_000_040), evideo[1].timestamp)
		c.Check("legacy AVC follows at normal timestamp", evideo[2].timestamp == 1000)
		c.Check("legacy AVC header bytes", len(evideo[2].data) >= 5 && bytes.Equal(evideo[2].data[:5], []byte{0x17, 0x01, 0xff, 0xff, 0xfe}), evideo[2].data[:5])
	}

	ep0, err := erx.ReadPacket()
	ep1, err2 := erx.ReadPacket()
	ap, err3 := erx.ReadPacket()
	c.Check("ext config read", err == nil)
	c.Check("ext config track/type", ep0.TrackID == 0 && ep0.Type == av.HEVCDecoderConfig)
	c.Check("ext HEVC frame read", err2 == nil)
	c.Check("following AVC read", err3 == nil)
	c.Check("ext timestamp decoded", ms(ep1.Time) == 17_000_040, ep1.Time)
	c.Check("ext composition offset decoded", ms(ep1.CTime) == 25)
	c.Check("legacy AVC type unchanged", ap.Type == av.H264)
	c.Check("legacy AVC payload and timing", bytes.Equal(ap.Data, avcData) && ms(ap.Time) == 1000 && ms(ap.CTime) == -2)
}
