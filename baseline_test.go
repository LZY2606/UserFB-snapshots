package wav

import (
	"bytes"
	"encoding/binary"
	"reflect"
	"testing"

	"github.com/go-audio/audio"
)

// Independent wire fixtures, not output from Encoder.
func testLE(b *bytes.Buffer, x interface{}) {
	if err := binary.Write(b, binary.LittleEndian, x); err != nil {
		panic(err)
	}
}
func testChunk(id string, p []byte) []byte {
	var b bytes.Buffer
	b.WriteString(id)
	testLE(&b, uint32(len(p)))
	b.Write(p)
	if len(p)%2 != 0 {
		b.WriteByte(0)
	}
	return b.Bytes()
}
func testFixture(rf64 bool) []byte {
	var fmtBody, pcm, body, out bytes.Buffer
	testLE(&fmtBody, uint16(1))
	testLE(&fmtBody, uint16(2))
	testLE(&fmtBody, uint32(8000))
	testLE(&fmtBody, uint32(32000))
	testLE(&fmtBody, uint16(4))
	testLE(&fmtBody, uint16(16))
	for _, v := range []int16{17, -23, 91, -101} {
		testLE(&pcm, v)
	}
	body.Write(testChunk("fmt ", fmtBody.Bytes()))
	body.Write(testChunk("data", pcm.Bytes()))
	if !rf64 {
		out.WriteString("RIFF")
		testLE(&out, uint32(4+body.Len()))
		out.WriteString("WAVE")
		out.Write(body.Bytes())
		return out.Bytes()
	}
	// RF64/WAVE, first ds64 with no table; the data header uses its 64-bit override.
	var ds bytes.Buffer
	testLE(&ds, uint64(4+36+body.Len()))
	testLE(&ds, uint64(pcm.Len()))
	testLE(&ds, uint64(2))
	testLE(&ds, uint32(0))
	out.WriteString("RF64")
	testLE(&out, uint32(0xffffffff))
	out.WriteString("WAVE")
	out.Write(testChunk("ds64", ds.Bytes()))
	tail := append([]byte(nil), body.Bytes()...)
	binary.LittleEndian.PutUint32(tail[28:32], 0xffffffff)
	out.Write(tail)
	return out.Bytes()
}
func TestBaseline(t *testing.T) {
	want := []int{17, -23, 91, -101}
	d := NewDecoder(bytes.NewReader(testFixture(false)))
	b, err := d.FullPCMBuffer()
	if err != nil || b == nil || !reflect.DeepEqual(b.Data, want) {
		t.Fatalf("RIFF: %v %v", b, err)
	}
	if err = d.Rewind(); err != nil {
		t.Fatal(err)
	}
	for i := 0; i < 2; i++ {
		part := &audio.IntBuffer{Data: make([]int, 2)}
		n, err := d.PCMBuffer(part)
		if err != nil || n != 2 || !reflect.DeepEqual(part.Data, want[i*2:i*2+2]) {
			t.Fatalf("RIFF split: %d %v %v", n, part.Data, err)
		}
	}
	t.Log("RIFF_NORMAL frames=2 samples=[17 -23 91 -101] rewind=ok")
	wire := testFixture(true)
	if len(wire) != 88 || binary.LittleEndian.Uint64(wire[20:28]) != uint64(len(wire)-8) {
		t.Fatal("fixture size")
	}
	rd := NewDecoder(bytes.NewReader(wire))
	rb, rerr := rd.FullPCMBuffer()
	if rerr == nil || rb != nil {
		t.Fatalf("expected current RF64 rejection, got %v %v", rb, rerr)
	}
	t.Logf("RF64_START_REJECTED bytes=%d error=%v", len(wire), rerr)
}
