package astisub_test

import (
 "bytes"
 "fmt"
 "strings"
 "testing"
 astisub "github.com/asticode/go-astisub"
)

func TestSeedTTML(t *testing.T) {
 xml := `<tt xmlns="http://www.w3.org/ns/ttml" xmlns:tts="http://www.w3.org/ns/ttml#styling"><head><styling><style xml:id="base" tts:color="red"/><style xml:id="a" style="base"/><style xml:id="b" style="base"/></styling></head><body><div><p begin="1s" end="2s" style="a">hello</p></div></body></tt>`
 s, err := astisub.ReadFromTTML(strings.NewReader(xml))
 if err != nil || len(s.Items)!=1 || s.Items[0].String()!="hello" { t.Fatalf("read: %v %#v",err,s) }
 linked:=0
 for _,id:=range []string{"a","b"} { if s.Styles[id].Style!=nil {linked++} }
 if linked!=1 {t.Fatalf("current fanout witness changed: %d",linked)}
 var out bytes.Buffer
 if err=s.WriteToTTML(&out);err!=nil {t.Fatal(err)}
 if _,err=astisub.ReadFromTTML(&out);err!=nil {t.Fatal(err)}
 _,err=astisub.ReadFromTTML(strings.NewReader(strings.Replace(xml,`style="a">hello`,`style="a b">hello`,1)))
 if err==nil {t.Fatal("expected current multi-reference rejection")}
 fmt.Println("SEED_TTML_OK fanout=1/2 multi_ref=rejected roundtrip=ok")
}
