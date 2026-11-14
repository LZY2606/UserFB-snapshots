package astisub_test

import (
	"bytes"
	"math"
	"strconv"
	"strings"
	"testing"
	"time"

	astisub "github.com/asticode/go-astisub"
	"github.com/asticode/go-astisub/task_support/taskcheck"
)

func webvttSegment(local string, mpegts int64, cues string) string {
	return "WEBVTT\nX-TIMESTAMP-MAP=LOCAL:" + local + ",MPEGTS:" + strconv.FormatInt(mpegts, 10) + "\n\n" + cues
}

func TestSegmentClock(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// Constructor validation
		_, err := astisub.NewWebVTTSegmentAssembler(-1)
		c.Check(err != nil, "negative origin90k must be rejected")
		_, err = astisub.NewWebVTTSegmentAssembler(0)
		c.Check(err == nil, "zero origin90k must be accepted: %v", err)

		// Basic mapping: output timeline is relative to the origin
		a, err := astisub.NewWebVTTSegmentAssembler(900000)
		c.Check(err == nil, "assembler creation failed: %v", err)
		err = a.Append(strings.NewReader(webvttSegment("00:00:00.000", 900000, "00:00:01.000 --> 00:00:03.000\nhello\n")))
		c.Check(err == nil, "append failed: %v", err)
		items := a.Result().Items
		c.Check(len(items) == 1 && items[0].StartAt == time.Second && items[0].EndAt == 3*time.Second,
			"basic mapping wrong: %+v", items)

		// Wrap around the 33-bit boundary: 1s before the wrap, then 1s after it
		a, _ = astisub.NewWebVTTSegmentAssembler(1<<33 - 90000)
		c.Check(a.Append(strings.NewReader(webvttSegment("00:00:00.000", 1<<33-90000, "00:00:00.000 --> 00:00:01.000\nbefore\n"))) == nil, "first segment failed")
		err = a.Append(strings.NewReader(webvttSegment("00:00:00.000", 90000, "00:00:00.000 --> 00:00:01.000\nafter\n")))
		c.Check(err == nil, "wrapped segment failed: %v", err)
		items = a.Result().Items
		c.Check(len(items) == 2 && items[1].StartAt == 2*time.Second && items[1].EndAt == 3*time.Second,
			"wrap continuity wrong: %+v", items)

		// First segment picks the lap closest to the origin, even below zero:
		// unwrapped -45000 against origin 45000 shifts cues by -1s
		a, _ = astisub.NewWebVTTSegmentAssembler(45000)
		err = a.Append(strings.NewReader(webvttSegment("00:00:00.000", 1<<33-45000, "00:00:01.500 --> 00:00:02.500\nbelow\n")))
		c.Check(err == nil, "lap below origin failed: %v", err)
		items = a.Result().Items
		c.Check(len(items) == 1 && items[0].StartAt == 500*time.Millisecond,
			"closest lap below origin wrong: %v", items[0].StartAt)

		// Exactly half a turn away from the previous segment is ambiguous
		a, _ = astisub.NewWebVTTSegmentAssembler(0)
		c.Check(a.Append(strings.NewReader(webvttSegment("00:00:00.000", 0, "00:00:00.000 --> 00:00:01.000\none\n"))) == nil, "first segment failed")
		err = a.Append(strings.NewReader(webvttSegment("00:00:00.000", 1<<32, "00:00:00.000 --> 00:00:01.000\ntwo\n")))
		c.Check(err != nil, "half turn distance must be an error")

		// Bad mappings
		a, _ = astisub.NewWebVTTSegmentAssembler(0)
		err = a.Append(strings.NewReader("WEBVTT\n\n00:00:01.000 --> 00:00:02.000\nno map\n"))
		c.Check(err != nil, "missing X-TIMESTAMP-MAP must be an error")
		err = a.Append(strings.NewReader(webvttSegment("00:00:00.000", 1<<33, "00:00:00.000 --> 00:00:01.000\nx\n")))
		c.Check(err != nil, "MPEGTS == 2^33 must be an error")
		err = a.Append(strings.NewReader(webvttSegment("00:00:00.000", -1, "00:00:00.000 --> 00:00:01.000\nx\n")))
		c.Check(err != nil, "negative MPEGTS must be an error")
		err = a.Append(strings.NewReader("WEBVTT\nX-TIMESTAMP-MAP=LOCAL:garbage,MPEGTS:0\n\n00:00:00.000 --> 00:00:01.000\nx\n"))
		c.Check(err != nil, "unparsable map must be an error")

		// Negative mapped time
		a, _ = astisub.NewWebVTTSegmentAssembler(90000)
		err = a.Append(strings.NewReader(webvttSegment("00:00:00.000", 0, "00:00:00.000 --> 00:00:01.000\ntoo early\n")))
		c.Check(err != nil, "negative mapped time must be an error")

		// Time inversion
		a, _ = astisub.NewWebVTTSegmentAssembler(0)
		err = a.Append(strings.NewReader(webvttSegment("00:00:00.000", 0, "00:00:02.000 --> 00:00:01.000\ninverted\n")))
		c.Check(err != nil, "inverted cue must be an error")

		// 90kHz to nanoseconds rounds down, also for negative deltas
		a, _ = astisub.NewWebVTTSegmentAssembler(0)
		c.Check(a.Append(strings.NewReader(webvttSegment("00:00:00.000", 1, "00:00:00.000 --> 00:00:01.000\ntick\n"))) == nil, "append failed")
		c.Check(a.Result().Items[0].StartAt == 11111*time.Nanosecond,
			"positive tick floor wrong: %v", a.Result().Items[0].StartAt)
		a, _ = astisub.NewWebVTTSegmentAssembler(2)
		c.Check(a.Append(strings.NewReader(webvttSegment("00:00:00.000", 1, "00:00:01.000 --> 00:00:02.000\ntick\n"))) == nil, "append failed")
		c.Check(a.Result().Items[0].StartAt == time.Second-11112*time.Nanosecond,
			"negative tick floor wrong: %v", a.Result().Items[0].StartAt)

		// Word level timestamps are mapped too, zero stays zero
		a, _ = astisub.NewWebVTTSegmentAssembler(0)
		err = a.Append(strings.NewReader(webvttSegment("00:00:00.000", 180000, "00:00:00.000 --> 00:00:02.000\none <00:00:01.000>two\n")))
		c.Check(err == nil, "append failed: %v", err)
		items = a.Result().Items
		c.Check(len(items) == 1 && len(items[0].Lines) == 1 && len(items[0].Lines[0].Items) == 2, "word split wrong: %+v", items)
		c.Check(items[0].Lines[0].Items[0].StartAt == 0, "zero word timestamp must stay zero: %v", items[0].Lines[0].Items[0].StartAt)
		c.Check(items[0].Lines[0].Items[1].StartAt == 3*time.Second, "word timestamp mapping wrong: %v", items[0].Lines[0].Items[1].StartAt)

		// StartPeriod re-anchors the clock: LOCAL maps exactly to the declared time
		a, _ = astisub.NewWebVTTSegmentAssembler(0)
		c.Check(a.Append(strings.NewReader(webvttSegment("00:00:00.000", 0, "00:00:00.000 --> 00:00:01.000\none\n"))) == nil, "append failed")
		c.Check(a.StartPeriod(-time.Second) != nil, "negative period start must be rejected")
		err = a.StartPeriod(10 * time.Second)
		c.Check(err == nil, "StartPeriod failed: %v", err)
		err = a.Append(strings.NewReader(webvttSegment("00:00:05.000", 123456, "00:00:05.500 --> 00:00:06.500\ntwo\n")))
		c.Check(err == nil, "first segment of new period failed: %v", err)
		err = a.Append(strings.NewReader(webvttSegment("00:00:05.000", 123456+90000, "00:00:05.000 --> 00:00:06.000\nthree\n")))
		c.Check(err == nil, "continuity within new period failed: %v", err)
		items = a.Result().Items
		c.Check(len(items) == 3, "expected 3 items, got %d", len(items))
		c.Check(items[1].StartAt == 10*time.Second+500*time.Millisecond, "period anchor wrong: %v", items[1].StartAt)
		c.Check(items[2].StartAt == 11*time.Second, "period continuity wrong: %v", items[2].StartAt)

		// Overflow while mapping is an error, not a wrap
		a, _ = astisub.NewWebVTTSegmentAssembler(0)
		c.Check(a.StartPeriod(time.Duration(math.MaxInt64)) == nil, "StartPeriod failed")
		err = a.Append(strings.NewReader(webvttSegment("00:00:00.000", 0, "00:00:01.000 --> 00:00:02.000\nboom\n")))
		c.Check(err != nil, "overflowing mapped time must be an error")
	})
}

func TestSegmentIdentity(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// Identifiers survive the native read/write path, empty ones are numbered
		s, err := astisub.ReadFromWebVTT(strings.NewReader("WEBVTT\n\n007\n00:00:01.000 --> 00:00:02.000\nfirst\n\nscene-a\n00:00:03.000 --> 00:00:04.000\nsecond\n\n00:00:05.000 --> 00:00:06.000\nthird\n"))
		c.Check(err == nil, "read failed: %v", err)
		c.Check(len(s.Items) == 3 && s.Items[0].WebVTTIdentifier == "007" && s.Items[1].WebVTTIdentifier == "scene-a" && s.Items[2].WebVTTIdentifier == "",
			"identifiers not captured: %+v", s.Items)
		var b bytes.Buffer
		c.Check(s.WriteToWebVTT(&b) == nil, "write failed")
		out := b.String()
		c.Check(strings.Contains(out, "007\n00:00:01.000"), "007 identifier lost:\n%s", out)
		c.Check(strings.Contains(out, "scene-a\n00:00:03.000"), "scene-a identifier lost:\n%s", out)
		c.Check(strings.Contains(out, "\n3\n00:00:05.000"), "auto numbering lost:\n%s", out)

		// Same period, same non empty ID, same mapped interval, same content: merged
		a, _ := astisub.NewWebVTTSegmentAssembler(0)
		c.Check(a.Append(strings.NewReader(webvttSegment("00:00:00.000", 0, "a\n00:00:01.000 --> 00:00:02.000\nhello\n"))) == nil, "append failed")
		c.Check(a.Append(strings.NewReader(webvttSegment("00:00:01.000", 90000, "a\n00:00:01.000 --> 00:00:02.000\nhello\n"))) == nil, "append failed")
		c.Check(len(a.Result().Items) == 1, "exact duplicate must be merged, got %d", len(a.Result().Items))

		// Same identity and interval but conflicting content: error
		a, _ = astisub.NewWebVTTSegmentAssembler(0)
		c.Check(a.Append(strings.NewReader(webvttSegment("00:00:00.000", 0, "a\n00:00:01.000 --> 00:00:02.000\nhello\n"))) == nil, "append failed")
		err = a.Append(strings.NewReader(webvttSegment("00:00:01.000", 90000, "a\n00:00:01.000 --> 00:00:02.000\nworld\n")))
		c.Check(err != nil, "conflicting duplicate must be an error")

		// Same ID at another interval is kept
		a, _ = astisub.NewWebVTTSegmentAssembler(0)
		c.Check(a.Append(strings.NewReader(webvttSegment("00:00:00.000", 0, "a\n00:00:01.000 --> 00:00:02.000\nhello\n"))) == nil, "append failed")
		c.Check(a.Append(strings.NewReader(webvttSegment("00:00:01.000", 90000, "a\n00:00:02.000 --> 00:00:03.000\nhello\n"))) == nil, "append failed")
		c.Check(len(a.Result().Items) == 2, "same ID at another interval must be kept, got %d", len(a.Result().Items))

		// Different IDs with the same text and interval are kept
		a, _ = astisub.NewWebVTTSegmentAssembler(0)
		c.Check(a.Append(strings.NewReader(webvttSegment("00:00:00.000", 0, "a\n00:00:01.000 --> 00:00:02.000\nhello\n"))) == nil, "append failed")
		c.Check(a.Append(strings.NewReader(webvttSegment("00:00:01.000", 90000, "b\n00:00:01.000 --> 00:00:02.000\nhello\n"))) == nil, "append failed")
		c.Check(len(a.Result().Items) == 2, "different IDs must be kept, got %d", len(a.Result().Items))

		// Duplicates without IDs are kept
		a, _ = astisub.NewWebVTTSegmentAssembler(0)
		c.Check(a.Append(strings.NewReader(webvttSegment("00:00:00.000", 0, "00:00:01.000 --> 00:00:02.000\nhello\n"))) == nil, "append failed")
		c.Check(a.Append(strings.NewReader(webvttSegment("00:00:01.000", 90000, "00:00:01.000 --> 00:00:02.000\nhello\n"))) == nil, "append failed")
		c.Check(len(a.Result().Items) == 2, "duplicates without ID must be kept, got %d", len(a.Result().Items))

		// StartPeriod forgets dedup identities
		a, _ = astisub.NewWebVTTSegmentAssembler(0)
		c.Check(a.Append(strings.NewReader(webvttSegment("00:00:00.000", 0, "a\n00:00:01.000 --> 00:00:02.000\nhello\n"))) == nil, "append failed")
		c.Check(a.StartPeriod(10*time.Second) == nil, "StartPeriod failed")
		c.Check(a.Append(strings.NewReader(webvttSegment("00:00:00.000", 5, "a\n00:00:01.000 --> 00:00:02.000\nhello\n"))) == nil, "append failed")
		c.Check(len(a.Result().Items) == 2, "identity must not leak across periods, got %d", len(a.Result().Items))

		// Result is sorted by start time, stable for ties
		a, _ = astisub.NewWebVTTSegmentAssembler(0)
		c.Check(a.Append(strings.NewReader(webvttSegment("00:00:00.000", 0, "b\n00:00:03.000 --> 00:00:04.000\nlate\n\na\n00:00:01.000 --> 00:00:02.000\nearly\n\nx\n00:00:01.000 --> 00:00:01.500\ntie one\n\ny\n00:00:01.000 --> 00:00:01.500\ntie two\n"))) == nil, "append failed")
		items := a.Result().Items
		got := []string{items[0].WebVTTIdentifier, items[1].WebVTTIdentifier, items[2].WebVTTIdentifier, items[3].WebVTTIdentifier}
		c.Check(len(items) == 4 && got[0] == "a" && got[1] == "x" && got[2] == "y" && got[3] == "b",
			"stable sort wrong: %v", got)

		// A redefined region must not change the layout of older cues
		a, _ = astisub.NewWebVTTSegmentAssembler(0)
		c.Check(a.Append(strings.NewReader(webvttSegment("00:00:00.000", 0, "REGION\nid:r\nwidth:40%\n\nc1\n00:00:01.000 --> 00:00:02.000 region:r\none\n"))) == nil, "append failed")
		c.Check(a.Append(strings.NewReader(webvttSegment("00:00:01.000", 90000, "REGION\nid:r\nwidth:80%\n\nc2\n00:00:02.000 --> 00:00:03.000 region:r\ntwo\n"))) == nil, "append failed")
		res := a.Result()
		items = res.Items
		c.Check(len(res.Regions) == 2, "redefined region must produce a second region, got %d", len(res.Regions))
		c.Check(items[0].Region != nil && items[0].Region.InlineStyle.WebVTTWidth == "40%", "old cue layout changed: %+v", items[0].Region)
		c.Check(items[1].Region != nil && items[1].Region.InlineStyle.WebVTTWidth == "80%", "new cue layout wrong: %+v", items[1].Region)
		c.Check(items[0].Region.ID == "r" && items[1].Region.ID != "r", "region renaming wrong: %q vs %q", items[0].Region.ID, items[1].Region.ID)
		c.Check(res.Regions[items[0].Region.ID] == items[0].Region && res.Regions[items[1].Region.ID] == items[1].Region,
			"region references do not match the region table")
		b.Reset()
		c.Check(res.WriteToWebVTT(&b) == nil, "write failed")
		out = b.String()
		c.Check(strings.Contains(out, "width:40%") && strings.Contains(out, "width:80%"), "both region layouts must be written:\n%s", out)
		c.Check(strings.Contains(out, "region:"+items[0].Region.ID+"\n") && strings.Contains(out, "region:"+items[1].Region.ID+"\n"),
			"cue region references must follow the renaming:\n%s", out)

		// An identically redefined region is reused
		a, _ = astisub.NewWebVTTSegmentAssembler(0)
		c.Check(a.Append(strings.NewReader(webvttSegment("00:00:00.000", 0, "REGION\nid:r\nwidth:40%\n\nc1\n00:00:01.000 --> 00:00:02.000 region:r\none\n"))) == nil, "append failed")
		c.Check(a.Append(strings.NewReader(webvttSegment("00:00:01.000", 90000, "REGION\nid:r\nwidth:40%\n\nc2\n00:00:02.000 --> 00:00:03.000 region:r\ntwo\n"))) == nil, "append failed")
		res = a.Result()
		c.Check(len(res.Regions) == 1, "identical region must be reused, got %d", len(res.Regions))
		c.Check(res.Items[0].Region == res.Items[1].Region, "identical regions must be shared")
	})
}

func TestSegmentRoundTrip(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		a, err := astisub.NewWebVTTSegmentAssembler(1<<33 - 90000)
		c.Check(err == nil, "assembler creation failed: %v", err)
		// One segment just before the 33-bit wrap, one just after, then a new period
		c.Check(a.Append(strings.NewReader(webvttSegment("00:00:00.000", 1<<33-90000,
			"007\n00:00:00.500 --> 00:00:01.500\nfirst <00:00:01.000>word\n"))) == nil, "append failed")
		c.Check(a.Append(strings.NewReader(webvttSegment("00:00:00.000", 45000,
			"scene-a\n00:00:00.000 --> 00:00:01.000\nsecond\n"))) == nil, "append failed")
		c.Check(a.StartPeriod(20*time.Second) == nil, "StartPeriod failed")
		c.Check(a.Append(strings.NewReader(webvttSegment("00:00:10.000", 777,
			"00:00:11.000 --> 00:00:12.000\nthird\n"))) == nil, "append failed")

		res := a.Result()
		c.Check(len(res.Items) == 3, "expected 3 items, got %d", len(res.Items))
		c.Check(res.Items[0].StartAt == 500*time.Millisecond && res.Items[0].EndAt == 1500*time.Millisecond,
			"first cue wrong: %v-%v", res.Items[0].StartAt, res.Items[0].EndAt)
		c.Check(res.Items[1].StartAt == 1500*time.Millisecond && res.Items[1].EndAt == 2500*time.Millisecond,
			"wrapped cue wrong: %v-%v", res.Items[1].StartAt, res.Items[1].EndAt)
		c.Check(res.Items[2].StartAt == 21*time.Second && res.Items[2].EndAt == 22*time.Second,
			"period cue wrong: %v-%v", res.Items[2].StartAt, res.Items[2].EndAt)
		c.Check(res.Items[0].Lines[0].Items[1].StartAt == time.Second,
			"word timestamp wrong: %v", res.Items[0].Lines[0].Items[1].StartAt)
		c.Check(res.Metadata == nil || res.Metadata.WebVTTTimestampMap == nil,
			"assembled result must not carry a timestamp map")

		// The result stays editable: change text and timings, write, read back
		res.Items[0].Lines[0].Items[0].Text = "edited "
		res.Items[2].StartAt = 19 * time.Second
		res.Items[2].EndAt = 20 * time.Second
		var b bytes.Buffer
		c.Check(res.WriteToWebVTT(&b) == nil, "write failed")
		out := b.String()
		c.Check(!strings.Contains(out, "X-TIMESTAMP-MAP"), "timestamp map leaked into output:\n%s", out)
		c.Check(strings.Contains(out, "007\n") && strings.Contains(out, "scene-a\n"), "identifiers lost:\n%s", out)

		back, err := astisub.ReadFromWebVTT(bytes.NewReader(b.Bytes()))
		c.Check(err == nil, "read back failed: %v", err)
		c.Check(len(back.Items) == 3, "read back lost items: %d", len(back.Items))
		c.Check(back.Items[0].String() == "edited word", "edited text lost: %q", back.Items[0].String())
		c.Check(back.Items[0].WebVTTIdentifier == "007" && back.Items[1].WebVTTIdentifier == "scene-a",
			"identifiers not preserved on read back: %q %q", back.Items[0].WebVTTIdentifier, back.Items[1].WebVTTIdentifier)
		c.Check(back.Items[2].StartAt == 19*time.Second && back.Items[2].EndAt == 20*time.Second,
			"edited timings lost: %v-%v", back.Items[2].StartAt, back.Items[2].EndAt)
		c.Check(back.Metadata == nil || back.Metadata.WebVTTTimestampMap == nil,
			"read back must not see a timestamp map")

		// Edits to the result must not leak into the assembler
		res2 := a.Result()
		c.Check(res2.Items[0].Lines[0].Items[0].Text == "first ", "assembler state corrupted by edit: %q", res2.Items[0].Lines[0].Items[0].Text)
		c.Check(res2.Items[2].StartAt == 21*time.Second, "assembler timing corrupted by edit: %v", res2.Items[2].StartAt)

		// And further appends must not leak into previously returned results
		c.Check(a.Append(strings.NewReader(webvttSegment("00:00:10.000", 777+90000,
			"00:00:12.000 --> 00:00:13.000\nfourth\n"))) == nil, "append failed")
		c.Check(len(res.Items) == 3, "previously returned result changed: %d", len(res.Items))
		c.Check(len(a.Result().Items) == 4, "fourth segment missing: %d", len(a.Result().Items))
	})
}
