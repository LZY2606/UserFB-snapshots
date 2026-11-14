package astisub_test

import (
 "bytes"
 "fmt"
 "strings"
 "testing"
 "time"
 astisub "github.com/asticode/go-astisub"
)

func TestSeedSegmentBaseline(t *testing.T) {
 src := "WEBVTT\nX-TIMESTAMP-MAP=LOCAL:00:00:00.000,MPEGTS:8589844592\n\nscene-a\n00:00:01.000 --> 00:00:03.000\nfirst <00:00:02.000>second\n"
 a, err := astisub.ReadFromWebVTT(strings.NewReader(src)); if err != nil {t.Fatal(err)}
 if len(a.Items)!=1 || a.Items[0].String()!="first second" || a.Items[0].StartAt!=time.Second {t.Fatal("native parser changed")}
 if a.Metadata.WebVTTTimestampMap.MpegTS!=8589844592 || a.Items[0].Lines[0].Items[1].StartAt!=2*time.Second {t.Fatal("map/word capture changed")}
 var out bytes.Buffer
 if err=a.WriteToWebVTT(&out);err!=nil {t.Fatal(err)}
 // Basic writer functionality is checked independently of the feature gap.
 b,err:=astisub.ReadFromWebVTT(bytes.NewReader(out.Bytes()));if err!=nil || len(b.Items)!=1 || b.Items[0].String()!="first second" {t.Fatal("native roundtrip failed",err)}
 fmt.Printf("BASELINE_OK cues=%d local_ms=%d word_ms=%d map90k=%d\n",len(a.Items),a.Items[0].StartAt.Milliseconds(),a.Items[0].Lines[0].Items[1].StartAt.Milliseconds(),a.Metadata.WebVTTTimestampMap.MpegTS)
 fmt.Printf("IDENTIFIER_RETAINED=%t\n",strings.Contains(out.String(),"scene-a\n"))
}
