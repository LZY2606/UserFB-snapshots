// Command enhanced_rtmp demonstrates receiving and sending multitrack HEVC
// over the existing FLV/RTMP packet interfaces. It shows a non-zero track id
// and a configuration switch on the same track.
package main

import (
	"bytes"
	"fmt"
	"io"
	"time"

	"github.com/notedit/rtmp/av"
	"github.com/notedit/rtmp/format/flv"
	"github.com/notedit/rtmp/format/rtmp"
)

func main() {
	// Two independent HEVCDecoderConfigurationRecords for the same logical
	// track: the first uses 4-byte NAL length prefixes, the switched one
	// uses 2-byte prefixes.
	oldConfig := buildHVCC(4, []byte{0x40, 0x01})
	newConfig := buildHVCC(2, []byte{0x42, 0x09})

	// Track 2 is a non-zero track, so its messages use OneTrack framing.
	// Track 0 uses plain enhanced messages.
	out := []av.Packet{
		{Type: av.HEVCDecoderConfig, IsKeyFrame: true, TrackID: 2, Time: 0, Data: oldConfig},
		{Type: av.HEVC, IsKeyFrame: true, TrackID: 2, Time: 40 * time.Millisecond,
			CTime: 0, Data: buildFrame(4, []byte{0x26, 0xaa})},

		// Configuration switch on track 2: later frames must use the new
		// 2-byte width; packets already handed out keep their own snapshot.
		{Type: av.HEVCDecoderConfig, IsKeyFrame: true, TrackID: 2, Time: 80 * time.Millisecond, Data: newConfig},
		{Type: av.HEVC, IsKeyFrame: false, TrackID: 2, Time: 120 * time.Millisecond,
			CTime: -3 * time.Millisecond, Data: buildFrame(2, []byte{0x27, 0xbb})},

		// SequenceEnd closes track 2 only; other tracks are untouched.
		{Type: av.HEVCEndOfSequence, IsKeyFrame: true, TrackID: 2, Time: 160 * time.Millisecond},
	}

	var buf bytes.Buffer
	muxer := flv.NewMuxer(&buf)
	muxer.HasVideo = true
	for _, pkt := range out {
		if err := muxer.WritePacket(pkt); err != nil {
			panic(err)
		}
	}

	demuxer := flv.NewDemuxer(bytes.NewReader(buf.Bytes()))
	for {
		pkt, err := demuxer.ReadPacket()
		if err == io.EOF {
			break
		}
		if err != nil {
			panic(err)
		}

		switch pkt.Type {
		case av.HEVCDecoderConfig:
			fmt.Printf("track %d config: NAL width=%d bytes\n", pkt.TrackID, pkt.HEVC.NALLengthSize)
		case av.HEVC:
			fmt.Printf("track %d frame: dts=%v cts=%v nalWidthAtCapture=%d\n",
				pkt.TrackID, pkt.Time, pkt.CTime, pkt.HEVC.NALLengthSize)
		case av.HEVCEndOfSequence:
			fmt.Printf("track %d sequence end\n", pkt.TrackID)
		}
	}

	// The same packets go through the RTMP Conn via WritePacket/ReadPacket;
	// the wire framing (plain vs OneTrack) is chosen from pkt.TrackID and the
	// existing chunk encoder splits header and payload by real message length.
	var wire rtmpWire
	tx := rtmp.NewConn(&wire)
	tx.Stage = rtmp.StageDataStart
	if err := tx.SetWriteChunkSize(32); err != nil {
		panic(err)
	}
	for _, pkt := range out {
		if err := tx.WritePacket(pkt); err != nil {
			panic(err)
		}
	}

	wireLen := wire.Len()

	rx := rtmp.NewConn(&wire)
	rx.Stage = rtmp.StageDataStart
	for i := 0; i < len(out); i++ {
		if _, err := rx.ReadPacket(); err != nil {
			panic(err)
		}
	}
	fmt.Printf("rtmp round trip: %d packets over %d chunked wire bytes\n", len(out), wireLen)
}

// rtmpWire is an in-memory ReadWriteFlusher for the example.
type rtmpWire struct {
	buf bytes.Buffer
}

func (w *rtmpWire) Read(p []byte) (int, error)  { return w.buf.Read(p) }
func (w *rtmpWire) Write(p []byte) (int, error) { return w.buf.Write(p) }
func (w *rtmpWire) Flush() error                { return nil }
func (w *rtmpWire) Len() int                    { return w.buf.Len() }

// buildHVCC builds a minimal valid HEVCDecoderConfigurationRecord.
func buildHVCC(width byte, nalus ...[]byte) []byte {
	b := make([]byte, 0, 23+4*len(nalus))
	fixed := make([]byte, 23)
	fixed[0] = 1
	fixed[13] = 0xf0
	fixed[15] = 0xfc
	fixed[16] = 0xfc
	fixed[17] = 0xf8
	fixed[18] = 0xf8
	fixed[21] = 0xfc | (width-1)&3
	fixed[22] = byte(len(nalus))
	b = append(b, fixed...)
	for _, nalu := range nalus {
		b = append(b, 0x80|(nalu[0]>>1), 0, 1)
		b = append(b, byte(len(nalu)>>8), byte(len(nalu)))
		b = append(b, nalu...)
	}
	return b
}

// buildFrame prefixes each NAL with a width-byte big-endian length.
func buildFrame(width int, nalus ...[]byte) []byte {
	var b []byte
	for _, nalu := range nalus {
		l := len(nalu)
		switch width {
		case 2:
			b = append(b, byte(l>>8), byte(l))
		case 4:
			b = append(b, byte(l>>24), byte(l>>16), byte(l>>8), byte(l))
		default:
			b = append(b, byte(l))
		}
		b = append(b, nalu...)
	}
	return b
}
