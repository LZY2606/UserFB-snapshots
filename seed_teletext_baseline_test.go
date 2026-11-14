package astisub

import (
 "bytes"
 "fmt"
 "os"
 "strings"
 "testing"
 "time"
)
func TestSeedTeletextBaseline(t *testing.T) {
 b,e:=os.ReadFile("task_support/fixtures/basic.ts"); if e!=nil {t.Fatal(e)}
 s,e:=ReadFromTeletext(bytes.NewReader(b),TeletextOptions{PID:256,Page:888}); if e!=nil {t.Fatal(e)}
 if len(s.Items)!=2 {t.Fatalf("items=%d",len(s.Items))}
 for n,want:=range []string{"hello","world"} {
  i:=s.Items[n];if i.String()!=want || i.StartAt!=time.Duration(2*n)*time.Second || i.EndAt!=time.Duration(2*n+2)*time.Second {t.Fatalf("page=%d text=%q times=%v/%v",n,i.String(),i.StartAt,i.EndAt)}
 }
 var out bytes.Buffer;if e=s.WriteToSRT(&out);e!=nil {t.Fatal(e)}
 if !strings.Contains(out.String(),"00:00:02,000") || !strings.Contains(out.String(),"world") {t.Fatal(out.String())}
 fmt.Println("BASELINE_TELETEXT_OK pages=2 text=hello,world end=4s")
}
