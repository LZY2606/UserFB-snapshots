package oggvorbis_test

import (
	"bytes"
	"encoding/binary"
	"io"
	"math/rand"
	"os"
	"testing"

	"github.com/jfreymuth/oggvorbis"
)

// This file is the contract test for chained ogg/vorbis support. Every legal
// chain is built from the repository's own fixtures (testdata/test.ogg,
// testdata/long.ogg, testdata/eof_issue.ogg and support/media/alternate.ogg)
// by rearranging pages, assigning fresh serial numbers and recomputing CRCs.
// No external encoder and no network access are required.

var chainCRCTable [256]uint32

func init() {
	for i := range chainCRCTable {
		r := uint32(i) << 24
		for j := 0; j < 8; j++ {
			if r&0x80000000 != 0 {
				r = (r << 1) ^ 0x04c11db7
			} else {
				r <<= 1
			}
		}
		chainCRCTable[i] = r
	}
}

type rawPage struct {
	flag    byte
	granule int64
	serial  uint32
	seq     uint32
	lacing  []byte
	content []byte
}

func parsePages(b []byte) []rawPage {
	var pages []rawPage
	off := 0
	for off < len(b) {
		if string(b[off:off+4]) != "OggS" {
			panic("fixture framing: missing capture pattern")
		}
		flag := b[off+5]
		granule := int64(binary.LittleEndian.Uint64(b[off+6:]))
		serial := binary.LittleEndian.Uint32(b[off+14:])
		seq := binary.LittleEndian.Uint32(b[off+18:])
		nseg := int(b[off+26])
		lacing := append([]byte(nil), b[off+27:off+27+nseg]...)
		contentLen := 0
		for _, s := range lacing {
			contentLen += int(s)
		}
		content := append([]byte(nil), b[off+27+nseg:off+27+nseg+contentLen]...)
		pages = append(pages, rawPage{flag, granule, serial, seq, lacing, content})
		off += 27 + nseg + contentLen
	}
	return pages
}

func encodePage(p rawPage) []byte {
	hdr := make([]byte, 27)
	copy(hdr[:4], "OggS")
	hdr[5] = p.flag
	binary.LittleEndian.PutUint64(hdr[6:], uint64(p.granule))
	binary.LittleEndian.PutUint32(hdr[14:], p.serial)
	binary.LittleEndian.PutUint32(hdr[18:], p.seq)
	hdr[26] = byte(len(p.lacing))
	out := append(append(hdr, p.lacing...), p.content...)
	var crc uint32
	for _, v := range out {
		crc = (crc << 8) ^ chainCRCTable[byte(crc>>24)^v]
	}
	binary.LittleEndian.PutUint32(out[22:], crc)
	return out
}

func encodePages(pages []rawPage) []byte {
	var buf bytes.Buffer
	for _, p := range pages {
		buf.Write(encodePage(p))
	}
	return buf.Bytes()
}

// relabelPages assigns a previously unused serial number to every page while
// leaving packets and flags intact, then recomputes the page CRCs.
func relabelPages(pages []rawPage, serial uint32) []rawPage {
	out := make([]rawPage, len(pages))
	for i, p := range pages {
		p.serial = serial
		out[i] = p
	}
	return out
}

// joinSegments concatenates independently encoded logical streams. Each
// logical stream numbers its pages from zero, as required for a chained ogg
// file.
func joinSegments(segments ...[]rawPage) []rawPage {
	var out []rawPage
	for _, seg := range segments {
		for i, p := range seg {
			p.seq = uint32(i)
			out = append(out, p)
		}
	}
	return out
}

// extractPackets reassembles every packet and records the granule of the page
// on which each packet completed. Only the final packet completed on a page
// carries that page's granule; earlier packets get -1, matching the ogg
// semantics that a page granule describes its final completed packet.
func extractPackets(pages []rawPage) (packets [][]byte, endGranule []int64) {
	var cur []byte
	for _, p := range pages {
		off := 0
		var pagePackets [][]byte
		for _, s := range p.lacing {
			cur = append(cur, p.content[off:off+int(s)]...)
			off += int(s)
			if s < 0xFF {
				pagePackets = append(pagePackets, cur)
				cur = nil
			}
		}
		for i, pkt := range pagePackets {
			packets = append(packets, pkt)
			if i == len(pagePackets)-1 {
				endGranule = append(endGranule, p.granule)
			} else {
				endGranule = append(endGranule, -1)
			}
		}
	}
	return packets, endGranule
}

// repaginate rebuilds an ogg stream from packets, putting at most maxSegs
// lacing entries on each page. Packets then span several pages and some pages
// complete no packet at all; continuation flags and CRCs are produced
// correctly.
func repaginate(packets [][]byte, endGranule []int64, serial uint32, maxSegs int) []byte {
	var pages []rawPage
	seq := uint32(0)
	pktIdx, pktOff := 0, 0
	first, needCont := true, false
	for pktIdx < len(packets) || pktOff > 0 {
		p := rawPage{granule: -1, serial: serial, seq: seq}
		if first {
			p.flag |= headerFlagBeginningOfStream
		}
		if needCont {
			p.flag |= headerFlagContinuedPacket
		}
		segs := 0
		content := []byte{}
		for segs < maxSegs && pktIdx < len(packets) {
			pkt := packets[pktIdx]
			remaining := len(pkt) - pktOff
			room := (maxSegs - segs) * 255
			take := remaining
			if take > room {
				take = room
			}
			left := take
			for left > 0 && segs < maxSegs {
				if left >= 255 {
					p.lacing = append(p.lacing, 0xFF)
					left -= 255
				} else {
					p.lacing = append(p.lacing, byte(left))
					left = 0
				}
				segs++
			}
			content = append(content, pkt[pktOff:pktOff+take]...)
			pktOff += take
			if pktOff == len(pkt) {
				p.granule = endGranule[pktIdx]
				pktIdx++
				pktOff = 0
			} else {
				break
			}
		}
		if len(p.lacing) == 0 {
			break
		}
		p.content = content
		pages = append(pages, p)
		needCont = p.lacing[len(p.lacing)-1] == 0xFF
		seq++
		first = false
	}
	pages[len(pages)-1].flag |= headerFlagEndOfStream
	return encodePages(pages)
}

const (
	headerFlagContinuedPacket   = 1
	headerFlagBeginningOfStream = 2
	headerFlagEndOfStream       = 4
)

func mustReadFile(t *testing.T, name string) []byte {
	t.Helper()
	b, err := os.ReadFile(name)
	if err != nil {
		t.Fatalf("read %s: %v", name, err)
	}
	return b
}

func decodeAll(t *testing.T, in io.Reader) ([]float32, *oggvorbis.Format) {
	t.Helper()
	pcm, f, err := oggvorbis.ReadAll(in)
	if err != nil {
		t.Fatalf("ReadAll: %v", err)
	}
	return pcm, f
}

func comparePCM(t *testing.T, got, want []float32, tolerance float32) {
	t.Helper()
	if len(got) != len(want) {
		t.Fatalf("pcm length %d, expected %d", len(got), len(want))
	}
	for i := range got {
		d := got[i] - want[i]
		if d > tolerance || d < -tolerance {
			t.Fatalf("pcm differs at value %d: %v != %v", i, got[i], want[i])
		}
	}
}

func drain(t *testing.T, r *oggvorbis.Reader, bufSize int) []float32 {
	t.Helper()
	var out []float32
	buf := make([]float32, bufSize)
	for {
		n, err := r.Read(buf)
		out = append(out, buf[:n]...)
		if err == io.EOF {
			return out
		}
		if err != nil {
			t.Fatalf("Read after %d values: %v", len(out), err)
		}
		if n == 0 {
			t.Fatal("Read made no progress")
		}
	}
}

// plainReader hides the Seek method so an input is treated as non-seekable.
type plainReader struct{ io.Reader }

type shortReader struct {
	r   io.Reader
	max int
}

func (s shortReader) Read(p []byte) (int, error) {
	if len(p) > s.max {
		p = p[:s.max]
	}
	return s.r.Read(p)
}

func loadFixtures(t *testing.T) (testOgg, longOgg, eofOgg, altOgg []byte, testRaw []float32) {
	testOgg = mustReadFile(t, "testdata/test.ogg")
	longOgg = mustReadFile(t, "testdata/long.ogg")
	eofOgg = mustReadFile(t, "testdata/eof_issue.ogg")
	altOgg = mustReadFile(t, "support/media/alternate.ogg")
	rawFile, err := os.Open("testdata/test.raw")
	if err != nil {
		t.Fatal(err)
	}
	defer rawFile.Close()
	rawSize, _ := rawFile.Seek(0, io.SeekEnd)
	if _, err := rawFile.Seek(0, io.SeekStart); err != nil {
		t.Fatal(err)
	}
	testRaw = make([]float32, rawSize/4)
	if err := binary.Read(rawFile, binary.LittleEndian, testRaw); err != nil {
		t.Fatal(err)
	}
	return
}

// TestChainPCM verifies that a chained file decodes to the concatenation of
// each segment's independent decode, with every sample compared.
func TestChainPCM(t *testing.T) {
	testOgg, longOgg, eofOgg, altOgg, testRaw := loadFixtures(t)

	t.Run("mono_chain_matches_concatenated_segments", func(t *testing.T) {
		p1, f1 := decodeAll(t, bytes.NewReader(testOgg))
		pLong, _ := decodeAll(t, bytes.NewReader(longOgg))
		if f1.Channels != 1 {
			t.Fatalf("test.ogg channels=%d, want 1", f1.Channels)
		}
		chain := encodePages(joinSegments(
			parsePages(testOgg),
			relabelPages(parsePages(testOgg), 101),
			relabelPages(parsePages(longOgg), 202),
		))
		want := append(append(append([]float32(nil), p1...), p1...), pLong...)

		for _, bufSize := range []int{1, 7, 64, 1024, 4096, len(want)} {
			r, err := oggvorbis.NewReader(bytes.NewReader(append([]byte(nil), chain...)))
			if err != nil {
				t.Fatalf("bufSize=%d: %v", bufSize, err)
			}
			if r.Length() != int64(len(want)) {
				t.Fatalf("bufSize=%d length=%d, want %d", bufSize, r.Length(), len(want))
			}
			got := drain(t, r, bufSize)
			comparePCM(t, got, want, 2e-5)
		}

		// The first segment must still match test.raw sample for sample.
		r, err := oggvorbis.NewReader(bytes.NewReader(chain))
		if err != nil {
			t.Fatal(err)
		}
		head := make([]float32, len(testRaw))
		for n := 0; n < len(head); {
			k, err := r.Read(head[n:])
			n += k
			if err != nil {
				t.Fatalf("first-segment read: %v", err)
			}
		}
		comparePCM(t, head, testRaw, 2e-5)
	})

	t.Run("stereo_chain_with_distinct_setup", func(t *testing.T) {
		// alternate.ogg and eof_issue.ogg are both stereo but have different
		// setup packets; the no-EOS fixture may only be the final segment.
		pa, fa := decodeAll(t, bytes.NewReader(altOgg))
		pe, _ := decodeAll(t, bytes.NewReader(eofOgg))
		if fa.Channels != 2 || fa.SampleRate != 44100 {
			t.Fatalf("alternate format=%+v", fa)
		}
		chain := encodePages(joinSegments(
			parsePages(altOgg),
			relabelPages(parsePages(altOgg), 303),
			relabelPages(parsePages(eofOgg), 404),
		))
		want := append(append(append([]float32(nil), pa...), pa...), pe...)

		r, err := oggvorbis.NewReader(bytes.NewReader(chain))
		if err != nil {
			t.Fatal(err)
		}
		if r.SampleRate() != 44100 || r.Channels() != 2 {
			t.Fatalf("file-level format changed: %d Hz %d ch", r.SampleRate(), r.Channels())
		}
		if r.Length() != int64(len(want)/2) {
			t.Fatalf("length=%d want %d", r.Length(), len(want)/2)
		}
		got := drain(t, r, 512)
		comparePCM(t, got, want, 1e-4)

		// File-level metadata stays that of the first segment.
		if r.Bitrate() != fa.Bitrate {
			t.Fatal("Bitrate changed away from the first segment's value")
		}
	})

	t.Run("short_read_source", func(t *testing.T) {
		p1, _ := decodeAll(t, bytes.NewReader(testOgg))
		chain := encodePages(joinSegments(
			parsePages(testOgg),
			relabelPages(parsePages(testOgg), 505),
		))
		want := append(append([]float32(nil), p1...), p1...)
		for _, max := range []int{1, 2, 3, 17, 64, 257} {
			r, err := oggvorbis.NewReader(shortReader{r: bytes.NewReader(chain), max: max})
			if err != nil {
				t.Fatalf("max=%d: %v", max, err)
			}
			got := drain(t, r, 333)
			comparePCM(t, got, want, 2e-5)
		}
	})

	t.Run("non_seekable_streaming", func(t *testing.T) {
		p1, _ := decodeAll(t, bytes.NewReader(testOgg))
		chain := encodePages(joinSegments(
			parsePages(testOgg),
			relabelPages(parsePages(testOgg), 606),
		))
		want := append(append([]float32(nil), p1...), p1...)
		got, f := decodeAll(t, plainReader{bytes.NewReader(chain)})
		if f.Channels != 1 {
			t.Fatalf("channels=%d", f.Channels)
		}
		comparePCM(t, got, want, 2e-5)

		// Length must stay unknown until the physical EOF is confirmed.
		r, err := oggvorbis.NewReader(plainReader{bytes.NewReader(chain)})
		if err != nil {
			t.Fatal(err)
		}
		buf := make([]float32, 4096)
		delivered := 0
		for delivered < 44100 {
			n, err := r.Read(buf)
			delivered += n
			if err != nil {
				t.Fatalf("first segment delivery: %v", err)
			}
			if r.Length() != 0 {
				t.Fatalf("Length=%d before physical EOF", r.Length())
			}
		}
	})

	t.Run("every_sample_around_segment_junction", func(t *testing.T) {
		p1, _ := decodeAll(t, bytes.NewReader(testOgg))
		chain := encodePages(joinSegments(
			parsePages(testOgg),
			relabelPages(parsePages(testOgg), 707),
		))
		want := append(append([]float32(nil), p1...), p1...)
		r, err := oggvorbis.NewReader(bytes.NewReader(chain))
		if err != nil {
			t.Fatal(err)
		}
		if err := r.SetPosition(44100 - 256); err != nil {
			t.Fatal(err)
		}
		one := make([]float32, 1)
		for frame := 44100 - 256; frame < 44100+256; frame++ {
			n, err := r.Read(one)
			if err != nil {
				t.Fatalf("frame %d: %v", frame, err)
			}
			if n != 1 {
				t.Fatalf("frame %d: got %d values", frame, n)
			}
			if d := one[0] - want[frame]; d > 2e-5 || d < -2e-5 {
				t.Fatalf("junction frame %d: %v != %v", frame, one[0], want[frame])
			}
			if r.Position() != int64(frame+1) {
				t.Fatalf("position %d, want %d", r.Position(), frame+1)
			}
		}
	})
}

// TestChainSeek exercises global-coordinate seeking across segments, pages
// and end of file.
func TestChainSeek(t *testing.T) {
	testOgg, longOgg, eofOgg, altOgg, _ := loadFixtures(t)

	monoChain := encodePages(joinSegments(
		parsePages(testOgg),
		relabelPages(parsePages(testOgg), 111),
		relabelPages(parsePages(longOgg), 222),
	))
	p1, _ := decodeAll(t, bytes.NewReader(testOgg))
	pLong, _ := decodeAll(t, bytes.NewReader(longOgg))
	monoRef := append(append(append([]float32(nil), p1...), p1...), pLong...)

	t.Run("boundary_and_end_positions", func(t *testing.T) {
		r, err := oggvorbis.NewReader(bytes.NewReader(monoChain))
		if err != nil {
			t.Fatal(err)
		}
		total := r.Length()
		bounds := []int64{0, 1, 44099, 44100, 44101, 88199, 88200, 88201, total - 1}
		for _, pos := range bounds {
			if err := r.SetPosition(pos); err != nil {
				t.Fatalf("seek %d: %v", pos, err)
			}
			if r.Position() != pos {
				t.Fatalf("position %d, want %d", r.Position(), pos)
			}
			buf := make([]float32, 64)
			n, err := r.Read(buf)
			if err != nil && err != io.EOF {
				t.Fatalf("read at %d: %v", pos, err)
			}
			for i := 0; i < n; i++ {
				if d := buf[i] - monoRef[int(pos)+i]; d > 2e-5 || d < -2e-5 {
					t.Fatalf("sample at %d+%d mismatch", pos, i)
				}
			}
		}
		// Exactly at the end and beyond: the next read must be EOF.
		for _, pos := range []int64{total, total + 1, total + 1000} {
			if err := r.SetPosition(pos); err != nil {
				t.Fatalf("seek %d: %v", pos, err)
			}
			if _, err := r.Read(make([]float32, 8)); err != io.EOF {
				t.Fatalf("pos %d: want EOF, got %v", pos, err)
			}
		}
		// Seeking backwards after reaching EOF works again.
		if err := r.SetPosition(44100); err != nil {
			t.Fatal(err)
		}
		buf := make([]float32, 16)
		if _, err := r.Read(buf); err != nil {
			t.Fatalf("post-EOF backward seek: %v", err)
		}
		for i := range buf {
			if d := buf[i] - monoRef[44100+i]; d > 2e-5 || d < -2e-5 {
				t.Fatal("post-EOF samples mismatch")
			}
		}
		// Negative positions error and leave the playback position unchanged.
		if err := r.SetPosition(1234); err != nil {
			t.Fatal(err)
		}
		if _, err := r.Read(make([]float32, 8)); err != nil {
			t.Fatal(err)
		}
		posBefore := r.Position()
		if err := r.SetPosition(-1); err == nil {
			t.Fatal("negative position accepted")
		}
		if r.Position() != posBefore {
			t.Fatalf("position changed %d -> %d after failed seek", posBefore, r.Position())
		}
	})

	t.Run("random_interleaved_access", func(t *testing.T) {
		pa, _ := decodeAll(t, bytes.NewReader(altOgg))
		// alt (7104) + alt' (7104): many segments, same stereo setup family.
		chain := encodePages(joinSegments(
			parsePages(altOgg),
			relabelPages(parsePages(altOgg), 333),
			relabelPages(parsePages(altOgg), 444),
		))
		ref := append(append(append([]float32(nil), pa...), pa...), pa...)
		r, err := oggvorbis.NewReader(bytes.NewReader(chain))
		if err != nil {
			t.Fatal(err)
		}
		total := r.Length()
		rnd := rand.New(rand.NewSource(20260714))
		for i := 0; i < 60; i++ {
			pos := rnd.Int63n(total + 10)
			if err := r.SetPosition(pos); err != nil {
				t.Fatalf("iter %d seek %d: %v", i, pos, err)
			}
			if pos >= total {
				if _, e := r.Read(make([]float32, 4)); e != io.EOF {
					t.Fatalf("iter %d: want EOF at %d", i, pos)
				}
				continue
			}
			frames := 1 + rnd.Intn(500)
			buf := make([]float32, frames*2)
			n, e := r.Read(buf)
			if e != nil && e != io.EOF {
				t.Fatalf("iter %d read: %v", i, e)
			}
			base := int(pos) * 2
			for j := 0; j < n; j++ {
				if base+j >= len(ref) {
					break
				}
				if d := buf[j] - ref[base+j]; d > 1e-4 || d < -1e-4 {
					t.Fatalf("iter %d pos %d sample %d mismatch", i, pos, j)
				}
			}
		}
		// Finally, a full sequential decode from zero still matches.
		if err := r.SetPosition(0); err != nil {
			t.Fatal(err)
		}
		comparePCM(t, drain(t, r, 1024), ref, 1e-4)
	})

	t.Run("seek_inside_repaginated_stream", func(t *testing.T) {
		pages := parsePages(testOgg)
		pkts, grans := extractPackets(pages)
		raw := mustReadFile(t, "testdata/test.raw")
		rawSize := len(raw) / 4
		for _, maxSegs := range []int{1, 2, 3} {
			rep := repaginate(pkts, grans, pages[0].serial, maxSegs)
			r, err := oggvorbis.NewReader(bytes.NewReader(rep))
			if err != nil {
				t.Fatalf("maxSegs=%d: %v", maxSegs, err)
			}
			for _, pos := range []int64{0, 1, 127, 128, 129, 1000, 20000, r.Length() - 1} {
				if err := r.SetPosition(pos); err != nil {
					t.Fatalf("maxSegs=%d seek %d: %v", maxSegs, pos, err)
				}
				buf := make([]float32, 256)
				n, e := r.Read(buf)
				if e != nil && e != io.EOF {
					t.Fatalf("maxSegs=%d read: %v", maxSegs, e)
				}
				for j := 0; j < n && int(pos)+j < rawSize; j++ {
					if d := buf[j] - loadRaw(t)[int(pos)+j]; d > 2e-5 || d < -2e-5 {
						t.Fatalf("maxSegs=%d pos %d sample %d mismatch", maxSegs, pos, j)
					}
				}
			}
		}
	})

	t.Run("non_seekable_input_rejected", func(t *testing.T) {
		r, err := oggvorbis.NewReader(plainReader{bytes.NewReader(monoChain)})
		if err != nil {
			t.Fatal(err)
		}
		before := r.Position()
		if err := r.SetPosition(10); err == nil {
			t.Fatal("SetPosition accepted on non-seekable input")
		}
		if r.Position() != before {
			t.Fatal("position changed after rejected seek")
		}
	})

	t.Run("seek_then_playback_across_distinct_setup_boundary", func(t *testing.T) {
		// alternate.ogg and eof_issue.ogg are both stereo but have different
		// setup packets; the no-EOS fixture is the final segment.
		pa, _ := decodeAll(t, bytes.NewReader(altOgg))
		pe, _ := decodeAll(t, bytes.NewReader(eofOgg))
		chain := encodePages(joinSegments(
			parsePages(altOgg),
			relabelPages(parsePages(eofOgg), 7777),
		))
		ref := append(append([]float32(nil), pa...), pe...)
		r, err := oggvorbis.NewReader(bytes.NewReader(chain))
		if err != nil {
			t.Fatal(err)
		}
		const boundary = 7104
		pos := int64(boundary - 200)
		if err := r.SetPosition(pos); err != nil {
			t.Fatal(err)
		}
		buf := make([]float32, 1000*2)
		var got []float32
		for len(got) < len(buf) {
			n, e := r.Read(buf[len(got):])
			got = append(got, buf[:n]...)
			if e == io.EOF {
				break
			}
			if e != nil {
				t.Fatal(e)
			}
		}
		base := int(pos) * 2
		for i := range got {
			if base+i >= len(ref) {
				break
			}
			if d := got[i] - ref[base+i]; d > 1e-4 || d < -1e-4 {
				t.Fatalf("sample at global frame %d mismatch", pos+int64(i/2))
			}
		}
	})

	t.Run("sequential_position_never_moves_back", func(t *testing.T) {
		r, err := oggvorbis.NewReader(bytes.NewReader(monoChain))
		if err != nil {
			t.Fatal(err)
		}
		prev := int64(0)
		buf := make([]float32, 37)
		for {
			n, e := r.Read(buf)
			if r.Position() < prev {
				t.Fatalf("position moved back %d -> %d", prev, r.Position())
			}
			prev = r.Position()
			if e == io.EOF {
				break
			}
			if e != nil {
				t.Fatal(e)
			}
			if n == 0 {
				t.Fatal("no progress")
			}
		}
		if r.Position() != r.Length() {
			t.Fatalf("final position %d, want length %d", r.Position(), r.Length())
		}
	})
}

func loadRaw(t *testing.T) []float32 {
	t.Helper()
	rawFile, err := os.Open("testdata/test.raw")
	if err != nil {
		t.Fatal(err)
	}
	defer rawFile.Close()
	size, _ := rawFile.Seek(0, io.SeekEnd)
	if _, err := rawFile.Seek(0, io.SeekStart); err != nil {
		t.Fatal(err)
	}
	raw := make([]float32, size/4)
	if err := binary.Read(rawFile, binary.LittleEndian, raw); err != nil {
		t.Fatal(err)
	}
	return raw
}

// TestChainPages focuses on page-level construction: multi-page continuation
// packets, pages without completed packets, tail trimming and error paths.
func TestChainPages(t *testing.T) {
	testOgg, _, eofOgg, altOgg, raw0 := loadFixtures(t)
	pages := parsePages(testOgg)
	pkts, grans := extractPackets(pages)
	raw := raw0

	t.Run("multi_page_continuation_packets", func(t *testing.T) {
		for _, maxSegs := range []int{1, 2, 3, 7} {
			rep := repaginate(pkts, grans, pages[0].serial, maxSegs)
			// maxSegs=1 guarantees pages that complete no packet and packets
			// spanning many pages.
			got, _ := decodeAll(t, bytes.NewReader(rep))
			comparePCM(t, got, raw, 2e-5)

			// The same structure must also stream without seeking.
			gotStream, _ := decodeAll(t, plainReader{bytes.NewReader(append([]byte(nil), rep...))})
			comparePCM(t, gotStream, raw, 2e-5)
		}
	})

	t.Run("unknown_granule_page_never_anchors_sample_zero", func(t *testing.T) {
		// A page made only of continuation lacing values has granule -1.
		rep := repaginate(pkts, grans, pages[0].serial, 1)
		r, err := oggvorbis.NewReader(bytes.NewReader(rep))
		if err != nil {
			t.Fatal(err)
		}
		if r.Length() != 44100 {
			t.Fatalf("length=%d, want 44100", r.Length())
		}
		if err := r.SetPosition(0); err != nil {
			t.Fatal(err)
		}
		buf := make([]float32, 256)
		if _, err := r.Read(buf); err != nil {
			t.Fatal(err)
		}
		for i := range buf {
			if d := buf[i] - raw[i]; d > 2e-5 || d < -2e-5 {
				t.Fatalf("sample %d mismatch after granule-less pages", i)
			}
		}
	})

	t.Run("tail_trimming_uses_eos_granule", func(t *testing.T) {
		rep := repaginate(pkts, grans, pages[0].serial, 2)
		r, err := oggvorbis.NewReader(bytes.NewReader(rep))
		if err != nil {
			t.Fatal(err)
		}
		if r.Length() != 44100 {
			t.Fatalf("trimmed length=%d, want 44100", r.Length())
		}
		got := drain(t, r, 1024)
		if len(got) != 44100 {
			t.Fatalf("decoded %d frames, want 44100", len(got))
		}
		// Chained lengths are the sum of the per-segment trimmed lengths.
		chain := encodePages(joinSegments(
			parsePages(rep),
			relabelPages(parsePages(rep), 818),
		))
		rc, err := oggvorbis.NewReader(bytes.NewReader(chain))
		if err != nil {
			t.Fatal(err)
		}
		if rc.Length() != 88200 {
			t.Fatalf("chain length=%d, want 88200", rc.Length())
		}
	})

	t.Run("bad_crc_is_an_error", func(t *testing.T) {
		bad := append([]byte(nil), testOgg...)
		bad[len(bad)-12] ^= 0xFF
		if _, err := oggvorbis.NewReader(bytes.NewReader(bad)); err == nil {
			t.Fatal("corrupted page accepted")
		}
		if _, _, err := oggvorbis.ReadAll(plainReader{bytes.NewReader(bad)}); err == nil {
			t.Fatal("corrupted page accepted on streaming path")
		}
	})

	t.Run("continuation_flag_mismatch_is_an_error", func(t *testing.T) {
		p := append([]rawPage(nil), pages...)
		p[len(p)-1].flag ^= headerFlagContinuedPacket
		if _, err := oggvorbis.NewReader(bytes.NewReader(encodePages(p))); err == nil {
			t.Fatal("continuation mismatch accepted")
		}
	})

	t.Run("mid_stream_serial_change_is_an_error", func(t *testing.T) {
		p := append([]rawPage(nil), pages...)
		p[1].serial = 987654321
		if _, err := oggvorbis.NewReader(bytes.NewReader(encodePages(p))); err == nil {
			t.Fatal("mid-stream serial change accepted")
		}
	})

	t.Run("segment_without_eos_before_bos_is_an_error", func(t *testing.T) {
		first := append([]rawPage(nil), pages...)
		first[len(first)-1].flag &^= headerFlagEndOfStream
		second := relabelPages(pages, 919)
		if _, err := oggvorbis.NewReader(bytes.NewReader(encodePages(joinSegments(first, second)))); err == nil {
			t.Fatal("missing middle EOS accepted")
		}
	})

	t.Run("dangling_continuation_at_eof_is_an_error", func(t *testing.T) {
		p := append([]rawPage(nil), pages...)
		p[len(p)-1].lacing = append(p[len(p)-1].lacing, 0xFF)
		p[len(p)-1].flag &^= headerFlagEndOfStream
		if _, err := oggvorbis.NewReader(bytes.NewReader(encodePages(p))); err == nil {
			t.Fatal("dangling continuation accepted")
		}
	})

	t.Run("truncated_final_page_is_an_error", func(t *testing.T) {
		trunc := testOgg[:len(testOgg)-7]
		if _, err := oggvorbis.NewReader(bytes.NewReader(trunc)); err == nil {
			t.Fatal("truncated page accepted")
		}
		if _, _, err := oggvorbis.ReadAll(plainReader{bytes.NewReader(trunc)}); err == nil {
			t.Fatal("truncated page accepted on streaming path")
		}
	})

	t.Run("sequence_number_gap_is_an_error", func(t *testing.T) {
		p := append([]rawPage(nil), pages...)
		p[2].seq += 4
		if _, err := oggvorbis.NewReader(bytes.NewReader(encodePages(p))); err == nil {
			t.Fatal("sequence gap accepted")
		}
	})

	t.Run("legacy_no_eos_only_valid_at_clean_physical_eof", func(t *testing.T) {
		// The shipped fixture keeps its legacy compatibility.
		r, err := oggvorbis.NewReader(bytes.NewReader(eofOgg))
		if err != nil {
			t.Fatalf("legacy no-EOS final segment rejected: %v", err)
		}
		if r.Length() != 72384 {
			t.Fatalf("legacy length=%d, want 72384", r.Length())
		}
		// Another BOS after the no-EOS segment is not compatibility.
		bad := encodePages(joinSegments(
			parsePages(eofOgg),
			relabelPages(parsePages(eofOgg), 1212),
		))
		if _, err := oggvorbis.NewReader(bytes.NewReader(bad)); err == nil {
			t.Fatal("BOS after no-EOS segment accepted")
		}
	})

	t.Run("inconsistent_channels_or_rate_is_an_error", func(t *testing.T) {
		// test.ogg is mono, alternate.ogg is stereo at the same rate.
		bad := encodePages(joinSegments(
			parsePages(testOgg),
			relabelPages(parsePages(altOgg), 1313),
		))
		if _, err := oggvorbis.NewReader(bytes.NewReader(bad)); err == nil {
			t.Fatal("inconsistent channel count accepted")
		}
		// The streaming path must reject it as well.
		if _, _, err := oggvorbis.ReadAll(plainReader{bytes.NewReader(bad)}); err == nil {
			t.Fatal("inconsistent channel count accepted on streaming path")
		}
	})

	t.Run("get_length_and_get_format_use_global_coordinates", func(t *testing.T) {
		p1, f1 := decodeAll(t, bytes.NewReader(testOgg))
		chain := encodePages(joinSegments(
			parsePages(testOgg),
			relabelPages(parsePages(testOgg), 1414),
			relabelPages(parsePages(testOgg), 1515),
		))
		length, format, err := oggvorbis.GetLength(bytes.NewReader(chain))
		if err != nil {
			t.Fatal(err)
		}
		if length != 3*int64(len(p1)) {
			t.Fatalf("GetLength=%d, want %d", length, 3*len(p1))
		}
		if format.Channels != f1.Channels || format.SampleRate != f1.SampleRate {
			t.Fatalf("GetFormat mismatch: %+v", format)
		}
		gotFormat, err := oggvorbis.GetFormat(bytes.NewReader(chain))
		if err != nil {
			t.Fatal(err)
		}
		if gotFormat.Channels != f1.Channels || gotFormat.SampleRate != f1.SampleRate {
			t.Fatalf("GetFormat mismatch: %+v", gotFormat)
		}
	})
}

// countingReadSeeker records the byte ranges read after Enable is called.
type countingReadSeeker struct {
	r       io.ReadSeeker
	ranges  [][2]int64
	enabled bool
}

func (c *countingReadSeeker) Read(p []byte) (int, error) {
	off, _ := c.r.Seek(0, io.SeekCurrent)
	n, err := c.r.Read(p)
	if c.enabled {
		c.ranges = append(c.ranges, [2]int64{off, off + int64(n)})
	}
	return n, err
}

func (c *countingReadSeeker) Seek(offset int64, whence int) (int64, error) {
	return c.r.Seek(offset, whence)
}

// readCounter is a plain reader counting every byte consumed.
type readCounter struct {
	r     io.Reader
	count int64
}

func (c *readCounter) Read(p []byte) (int, error) {
	n, err := c.r.Read(p)
	c.count += int64(n)
	return n, err
}

// TestChainStreaming verifies the sequential path's readahead discipline and
// seek I/O bounds on top of the shared parser.
func TestChainStreaming(t *testing.T) {
	testOgg, _, _, _, _ := loadFixtures(t)
	p1, _ := decodeAll(t, bytes.NewReader(testOgg))

	t.Run("sequential_path_does_not_pre_read_chain", func(t *testing.T) {
		chain := encodePages(joinSegments(
			parsePages(testOgg),
			relabelPages(parsePages(testOgg), 1313),
		))
		counter := &readCounter{r: bytes.NewReader(chain)}
		r, err := oggvorbis.NewReader(plainReader{counter})
		if err != nil {
			t.Fatal(err)
		}
		buf := make([]float32, 512)
		frames := 0
		for frames < len(p1) {
			n, e := r.Read(buf)
			frames += n
			if e != nil {
				t.Fatalf("first segment: %v after %d frames", e, frames)
			}
		}
		// Only the first segment plus at most one lookahead page may be read.
		if counter.count > int64(len(testOgg))+27+255+255*255 {
			t.Fatalf("read %d bytes before finishing segment 1 (chain %d)", counter.count, len(chain))
		}
		if r.Length() != 0 {
			t.Fatalf("Length=%d before physical EOF", r.Length())
		}
	})

	t.Run("samples_are_delivered_before_a_later_error", func(t *testing.T) {
		good := encodePages(joinSegments(
			parsePages(testOgg),
			relabelPages(parsePages(testOgg), 1414),
		))
		bad := append([]byte(nil), good...)
		bad[len(testOgg)+1500] ^= 0x5a // inside segment 2; its CRC now fails
		r, err := oggvorbis.NewReader(plainReader{bytes.NewReader(bad)})
		if err != nil {
			t.Fatalf("NewReader must not pre-read the damage: %v", err)
		}
		buf := make([]float32, 4096)
		frames := 0
		var finalErr error
		for {
			n, e := r.Read(buf)
			frames += n
			if e != nil {
				finalErr = e
				break
			}
		}
		if frames != len(p1) {
			t.Fatalf("delivered %d frames, want the full first segment %d", frames, len(p1))
		}
		if finalErr == nil || finalErr == io.EOF {
			t.Fatalf("want a corruption error, got %v", finalErr)
		}
	})

	t.Run("single_read_call_crosses_segment_boundary", func(t *testing.T) {
		chain := encodePages(joinSegments(
			parsePages(testOgg),
			relabelPages(parsePages(testOgg), 1515),
		))
		r, err := oggvorbis.NewReader(plainReader{bytes.NewReader(chain)})
		if err != nil {
			t.Fatal(err)
		}
		all := make([]float32, 2*len(p1))
		n, err := r.Read(all)
		if err != nil && err != io.EOF {
			t.Fatal(err)
		}
		if n != 2*len(p1) {
			t.Fatalf("single call produced %d values, want %d", n, 2*len(p1))
		}
		want := append(append([]float32(nil), p1...), p1...)
		comparePCM(t, all, want, 2e-5)
	})

	t.Run("seek_reads_stay_inside_target_segment", func(t *testing.T) {
		pages := parsePages(testOgg)
		pkts, grans := extractPackets(pages)
		// Many-page segments so a naive prefix rescan would be obvious.
		seg0 := repaginate(pkts, grans, 11, 1)
		seg1 := repaginate(pkts, grans, 22, 1)
		seg2 := repaginate(pkts, grans, 33, 1)
		chain := bytes.Join([][]byte{seg0, seg1, seg2}, nil)
		counter := &countingReadSeeker{r: bytes.NewReader(chain)}
		r, err := oggvorbis.NewReader(counter)
		if err != nil {
			t.Fatal(err)
		}
		finalSegmentStart := int64(len(seg0) + len(seg1))
		counter.enabled = true
		if err := r.SetPosition(r.Length() - 64); err != nil {
			t.Fatal(err)
		}
		if _, err := r.Read(make([]float32, 128)); err != nil && err != io.EOF {
			t.Fatal(err)
		}
		if len(counter.ranges) == 0 {
			t.Fatal("no reads recorded after seek")
		}
		var minOff, maxOff int64 = 1<<63 - 1, int64(0)
		for _, rg := range counter.ranges {
			if rg[0] < minOff {
				minOff = rg[0]
			}
			if rg[1] > maxOff {
				maxOff = rg[1]
			}
		}
		if minOff < finalSegmentStart {
			t.Fatalf("seek rescanned unrelated prefix: read at %d, segment starts %d", minOff, finalSegmentStart)
		}
		if maxOff > int64(len(chain)) {
			t.Fatalf("read past end of file: %d", maxOff)
		}
	})

	t.Run("eof_compat_requires_complete_page_and_no_pending_packet", func(t *testing.T) {
		// Complete page then physical EOF: accepted using the last granule.
		eofOgg := mustReadFile(t, "testdata/eof_issue.ogg")
		r, err := oggvorbis.NewReader(plainReader{bytes.NewReader(append([]byte(nil), eofOgg...))})
		if err != nil {
			t.Fatal(err)
		}
		if r.Length() != 0 {
			t.Fatalf("streaming length must be unknown until EOF, got %d", r.Length())
		}
		if values := len(drain(t, r, 512)); values/r.Channels() != 72384 {
			t.Fatalf("decoded %d frames, want 72384", values/r.Channels())
		}
	})
}
