package mp3_test
import (
 "bytes"
 "io"
 "os"
 "testing"
 mp3 "github.com/hajimehoshi/go-mp3"
)
func TestSeekBaseline(t *testing.T) {
 data,e:=os.ReadFile("example/mpeg2.mp3"); if e!=nil {t.Fatal(e)}
 d,e:=mp3.NewDecoder(bytes.NewReader(data)); if e!=nil {t.Fatal(e)}
 pcm,e:=io.ReadAll(d); if e!=nil||len(pcm)==0 {t.Fatalf("decode %d %v",len(pcm),e)}
 mismatch:=false
 for _,pos:=range []int64{9216,18432,36864,46080} {
  d,e=mp3.NewDecoder(bytes.NewReader(data)); if e!=nil {t.Fatal(e)}
  _,e=d.Seek(pos,io.SeekStart); if e!=nil {t.Logf("seek %d error=%v",pos,e);mismatch=true;continue}
  got:=make([]byte,512); _,e=io.ReadFull(d,got);if e!=nil {t.Fatal(e)}
  if !bytes.Equal(got,pcm[pos:pos+512]) {mismatch=true;t.Logf("seek %d differs from sequential PCM",pos)}
 }
 if !mismatch {t.Fatal("expected current seek discrepancy")}
 t.Logf("baseline decoded=%d rate=%d; seek discrepancy reproduced",len(pcm),d.SampleRate())
}
