package astisub_test

import (
	"os"
	"strings"
	"time"

	astisub "github.com/asticode/go-astisub"
)

// Assemble WebVTT segments straddling the 33-bit MPEG-TS wrap and a clock
// period break, then edit the resulting track and write it back out.
func ExampleWebVTTSegmentAssembler() {
	// The origin is 1s before the 33-bit MPEG-TS wrap (2^33 * 1000/90 ms)
	a, err := astisub.NewWebVTTSegmentAssembler(1<<33 - 90000)
	if err != nil {
		panic(err)
	}

	// A segment just before the wrap
	if err = a.Append(strings.NewReader("WEBVTT\nX-TIMESTAMP-MAP=LOCAL:00:00:00.000,MPEGTS:8589844592\n\nintro\n00:00:00.500 --> 00:00:01.500\nbefore the wrap\n")); err != nil {
		panic(err)
	}
	// A segment just after the wrap: continuity is kept across 2^33
	if err = a.Append(strings.NewReader("WEBVTT\nX-TIMESTAMP-MAP=LOCAL:00:00:00.000,MPEGTS:45000\n\n00:00:00.000 --> 00:00:01.000\nafter the wrap\n")); err != nil {
		panic(err)
	}
	// A clock period break: the next segment's LOCAL maps exactly to 10s
	if err = a.StartPeriod(10 * time.Second); err != nil {
		panic(err)
	}
	if err = a.Append(strings.NewReader("WEBVTT\nX-TIMESTAMP-MAP=LOCAL:00:01:00.000,MPEGTS:900000\n\n00:01:01.000 --> 00:01:02.000\nafter the break\n")); err != nil {
		panic(err)
	}

	// The assembled track can still be edited
	s := a.Result()
	s.Items[0].Lines[0].Items[0].Text = "Edited before the wrap"
	s.Items[2].StartAt -= time.Second
	s.Items[2].EndAt -= time.Second

	if err = s.WriteToWebVTT(os.Stdout); err != nil {
		panic(err)
	}

	// Output:
	// WEBVTT
	//
	// intro
	// 00:00:00.500 --> 00:00:01.500
	// Edited before the wrap
	//
	// 2
	// 00:00:01.500 --> 00:00:02.500
	// after the wrap
	//
	// 3
	// 00:00:10.000 --> 00:00:11.000
	// after the break
}
