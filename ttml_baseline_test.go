package astisub_test

import (
	"fmt"
	"strings"
	"testing"
	"time"

	astisub "github.com/asticode/go-astisub"
)

func TestTimedTTMLBaseline(t *testing.T) {
	raw := `<tt xmlns="http://www.w3.org/ns/ttml"><body><div><p begin="10s" end="20s">A<span begin="2s" end="8s">B<span begin="1s" dur="10s">C</span>D</span>E</p></div></body></tt>`
	s, err := astisub.ReadFromTTML(strings.NewReader(raw))
	if err != nil {
		t.Fatal(err)
	}
	expected := []struct {
		start, end time.Duration
		text       string
	}{
		{10 * time.Second, 12 * time.Second, "AE"},
		{12 * time.Second, 13 * time.Second, "ABDE"},
		{13 * time.Second, 18 * time.Second, "ABCDE"},
		{18 * time.Second, 20 * time.Second, "AE"},
	}
	if len(s.Items) != len(expected) {
		t.Fatalf("items=%+v", s.Items)
	}
	for i, want := range expected {
		if s.Items[i].StartAt != want.start || s.Items[i].EndAt != want.end || s.Items[i].String() != want.text {
			t.Fatalf("item %d=%s..%s %q, want %s..%s %q", i, s.Items[i].StartAt, s.Items[i].EndAt, s.Items[i].String(), want.start, want.end, want.text)
		}
	}
	fmt.Println("TTML_BASELINE_OK items=4 text=AE,ABDE,ABCDE,AE interval=10s..20s")
}
