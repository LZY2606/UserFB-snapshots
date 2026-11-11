/*
MIT License

Copyright 2016 Comcast Cable Communications Management, LLC

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

package retime

import (
	"github.com/Comcast/gots/v3/packet"
)

// payloadChunk records where one TS payload lives inside an output packet and
// at which logical stream offset its first byte sits. PES headers and PSI
// sections can be squeezed by adaptation fields across several packets while
// other PIDs are interleaved, so timestamp fields are located by walking
// these chunks instead of guessing fixed file offsets.
type payloadChunk struct {
	pkt          *packet.Packet
	start, end   int
	logicalStart int
}

// chunkStream concatenates the payloads of one PID into a logical byte stream
// while every byte stays anchored in the output packet that carries it.
// Logical offsets may contain holes (for example the cue pointer_field byte);
// such offsets are simply never read.
type chunkStream struct {
	chunks []payloadChunk
	length int
}

// add appends a contiguous payload fragment after the current stream end.
func (s *chunkStream) add(pkt *packet.Packet, start, end int) {
	s.addAt(s.length, pkt, start, end)
}

// addAt places a payload fragment at an explicit logical offset.
func (s *chunkStream) addAt(base int, pkt *packet.Packet, start, end int) {
	s.chunks = append(s.chunks, payloadChunk{pkt: pkt, start: start, end: end, logicalStart: base})
	if endAt := base + (end - start); endAt > s.length {
		s.length = endAt
	}
}

func (s *chunkStream) reset() {
	s.chunks = nil
	s.length = 0
}

// byteAt reads a logical byte assembled across TS payload boundaries.
func (s *chunkStream) byteAt(offset int) byte {
	for _, c := range s.chunks {
		n := c.end - c.start
		if offset >= c.logicalStart && offset < c.logicalStart+n {
			return c.pkt[c.start+(offset-c.logicalStart)]
		}
	}
	panic("retime: logical stream offset out of range")
}

// setByteAt writes a logical byte back into the TS packet that owns it.
func (s *chunkStream) setByteAt(offset int, value byte) {
	for _, c := range s.chunks {
		n := c.end - c.start
		if offset >= c.logicalStart && offset < c.logicalStart+n {
			c.pkt[c.start+(offset-c.logicalStart)] = value
			return
		}
	}
	panic("retime: logical stream offset out of range")
}

// payloadBounds returns the half open byte range of the packet payload inside
// the 188 byte array, or ok=false when there is no payload.
func payloadBounds(pkt *packet.Packet) (int, int, bool) {
	if !packet.ContainsPayload(pkt) {
		return 0, 0, false
	}
	start := 4
	if packet.ContainsAdaptationField(pkt) {
		start += 1 + int(pkt[4])
	}
	if start > packet.PacketSize {
		start = packet.PacketSize
	}
	return start, packet.PacketSize, true
}
