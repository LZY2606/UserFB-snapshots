package mail
import (
 "bytes"
 "fmt"
 "io"
 "os"
 "testing"
)
func TestPresentationBaseline(t *testing.T) {
 f,e:=os.Open("testdata/presentation.eml"); if e!=nil {t.Fatal(e)}; defer f.Close()
 r,e:=CreateReader(f); if e!=nil {t.Fatal(e)}; defer r.Close()
 var bodies [][]byte
 for { p,e:=r.NextPart(); if e==io.EOF {break}; if e!=nil {t.Fatal(e)}
  body,e:=io.ReadAll(p.Body); if e!=nil {t.Fatal(e)}; bodies=append(bodies,body)
  fmt.Printf("FLAT_PART %d kind=%T bytes=%d\n",len(bodies),p.Header,len(body))
 }
 if len(bodies)!=3 || !bytes.Equal(bodies[1],[]byte{1,2,3}) || !bytes.Contains(bodies[2],[]byte("cid:logo")) {t.Fatalf("unexpected parts: %q",bodies)}
 fmt.Println("BASELINE_MIME_FLATTEN_OK")
}
