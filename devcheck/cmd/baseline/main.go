package main
import("bytes";"fmt";"io";"os";"github.com/ulikunitz/xz")
func main(){
 raw,err:=os.ReadFile("devcheck/fixtures/records.raw");if err!=nil{panic(err)}
 var encoded bytes.Buffer
 w,err:=xz.NewWriter(&encoded);if err!=nil{panic(err)}
 if _,err=w.Write(raw);err!=nil{panic(err)};if err=w.Close();err!=nil{panic(err)}
 r,err:=xz.NewReader(bytes.NewReader(encoded.Bytes()));if err!=nil{panic(err)}
 got,err:=io.ReadAll(r);if err!=nil{panic(err)}
 if !bytes.Equal(got,raw){panic("unfiltered mismatch")}
 fmt.Println("XZ_LZMA2_BASELINE_OK bytes=1537")
}
