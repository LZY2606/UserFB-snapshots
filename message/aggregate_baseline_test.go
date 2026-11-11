package message

import (
	"bytes"
	"encoding/binary"
	"io"
	"strings"
	"testing"
)

func TestAggregateBaseline(t *testing.T) {
	tag := func(kind byte, ts uint32, payload []byte) []byte {
		n := len(payload)
		b := []byte{kind, byte(n >> 16), byte(n >> 8), byte(n), byte(ts >> 16), byte(ts >> 8), byte(ts), byte(ts >> 24), 0, 0, 0}
		b = append(b, payload...)
		return binary.BigEndian.AppendUint32(b, uint32(11+n))
	}
	audio := []byte{0xaf, 1, 0x12, 0x34}
	video := []byte{0x17, 1, 0, 0, 0, 0x65}
	body := append(tag(8, 1000, audio), tag(9, 1040, video)...)
	var msg Message
	err := NewDecoder(bytes.NewReader(body)).Decode(TypeIDAggregateMessage, &msg)
	if err != nil && !strings.Contains(err.Error(), "Not implemented: AggregateMessage") {
		t.Fatalf("unexpected aggregate status: %v", err)
	}
	aggregateErr := err
	if err := NewDecoder(bytes.NewReader(audio)).Decode(TypeIDAudioMessage, &msg); err != nil {
		t.Fatal(err)
	}
	got, err := io.ReadAll(msg.(*AudioMessage).Payload)
	if err != nil || !bytes.Equal(got, audio) {
		t.Fatalf("ordinary audio: %x %v", got, err)
	}
	if aggregateErr != nil {
		t.Log("baseline: ordinary audio payload roundtrip ok; valid two-tag aggregate unsupported")
	} else {
		t.Log("baseline: ordinary audio payload roundtrip ok; aggregate supported")
	}
}
