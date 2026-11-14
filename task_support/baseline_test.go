package task_support
import("testing";"fmt";"bytes";"os";"path/filepath";"image";"image/jpeg";"encoding/binary";m "github.com/icza/mjpeg")
func u(b []byte)int{return int(binary.LittleEndian.Uint32(b))}
func TestBaseline(t *testing.T){
 f:=filepath.Join(t.TempDir(),"normal.avi");w,e:=m.New(f,2,1,2);if e!=nil{t.Fatal(e)};var frame bytes.Buffer;if e=jpeg.Encode(&frame,image.NewRGBA(image.Rect(0,0,2,1)),nil);e!=nil{t.Fatal(e)}
 for i:=0;i<3;i++{if e=w.AddFrame(frame.Bytes());e!=nil{t.Fatal(e)}};if e=w.Close();e!=nil{t.Fatal(e)};b,e:=os.ReadFile(f);if e!=nil{t.Fatal(e)}
 if string(b[:4])!="RIFF"||u(b[4:8])!=len(b)-8||string(b[8:12])!="AVI "{t.Fatal("RIFF")}
 movi,idx:=-1,-1
 for p:=12;p+8<=len(b);{n:=u(b[p+4:p+8]);if string(b[p:p+4])=="LIST"&&string(b[p+8:p+12])=="movi"{movi=p+8};if string(b[p:p+4])=="idx1"{idx=p};p+=8+n+(n&1)}
 if movi<0||idx<0||u(b[idx+4:idx+8])!=48{t.Fatal("index")}
 for i:=0;i<3;i++{v:=b[idx+8+i*16:];p:=movi+u(v[8:12]);n:=u(v[12:16]);if string(b[p:p+4])!="00dc"||!bytes.Equal(b[p+8:p+8+n],frame.Bytes()){t.Fatal("indexed frame",i)}}
 if _,e=os.Stat(f+".idx_");!os.IsNotExist(e){t.Fatal("index temp remains")};fmt.Println("BASELINE_AVI frames=3 idx1=3 riff=1")
}
