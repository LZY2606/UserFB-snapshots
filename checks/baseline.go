package main
import("bytes";"context";"encoding/binary";"fmt";"www.velocidex.com/golang/go-ntfs/parser")
type image struct{*bytes.Reader;size int64}
func(i image)Ranges()[]parser.Range{return []parser.Range{{Offset:0,Length:i.size}}}
func main(){
 b:=make([]byte,256)
 for i,name:=range []string{"old.txt","other.txt","new.txt"}{
  r:=b[i*80:];binary.LittleEndian.PutUint32(r,80);binary.LittleEndian.PutUint16(r[4:],2)
  binary.LittleEndian.PutUint64(r[8:],uint64(40+i%2)|uint64(3)<<48)
  binary.LittleEndian.PutUint64(r[16:],5|uint64(1)<<48)
  binary.LittleEndian.PutUint64(r[24:],uint64(80*i))
  reason:=uint32(0x100);if i==0{reason=0x1000};if i==2{reason=0x2000}
  binary.LittleEndian.PutUint32(r[40:],reason)
  binary.LittleEndian.PutUint16(r[56:],uint16(2*len(name)));binary.LittleEndian.PutUint16(r[58:],60)
  for j,c:=range []byte(name){r[60+2*j]=c}
 }
 opts:=parser.GetDefaultOptions();opts.DisableFullPathResolution=true
 ntfs:=parser.GetNTFSContextFromRawMFT(bytes.NewReader(nil),4096,1024,opts)
 count:=0
 for r:=range parser.ParseUSN(context.Background(),ntfs,image{bytes.NewReader(b),int64(len(b))},0){
  if !r.Validate(){panic("invalid record")};fmt.Printf("USN %d FRN %d:%d %s %v\n",r.Usn(),r.FileReferenceNumberID(),r.FileReferenceNumberSequence(),r.FullPath(),r.Reason());count++
 }
 if count!=3{panic(count)};fmt.Println("USN_BASELINE_OK 3")
}
