package astisub_test
import (
 "fmt"
 "strings"
 "testing"
 "time"
 astisub "github.com/asticode/go-astisub"
)
func TestTimedTTMLBaseline(t *testing.T) {
 raw:=`<tt xmlns="http://www.w3.org/ns/ttml"><body><div><p begin="10s" end="20s">A<span begin="2s" end="8s">B<span begin="1s" dur="10s">C</span>D</span>E</p></div></body></tt>`
 s,err:=astisub.ReadFromTTML(strings.NewReader(raw));if err!=nil{t.Fatal(err)}
 if len(s.Items)!=1||s.Items[0].StartAt!=10*time.Second||s.Items[0].EndAt!=20*time.Second{t.Fatalf("items=%+v",s.Items)}
 if s.Items[0].String()!="ABDE"{t.Fatalf("text=%q",s.Items[0].String())}
 fmt.Println("TTML_BASELINE_OK items=1 text=ABDE interval=10s..20s")
}
