package main
import("encoding/binary";"fmt";"os";"github.com/alfg/mp4")
func main(){
 m,e:=mp4.Open("test/tears-of-steel.mp4");if e!=nil||m.Ftyp==nil||m.Ftyp.MajorBrand!="isom"||m.Moov==nil||len(m.Moov.Traks)==0{panic("normal production parse failed")};if f,ok:=m.Reader.(*os.File);ok{defer f.Close()};fmt.Printf("BASELINE_OK brand=%s tracks=%d\n",m.Ftyp.MajorBrand,len(m.Moov.Traks))
 b:=make([]byte,24);binary.BigEndian.PutUint32(b,1);copy(b[4:],"ftyp");binary.BigEndian.PutUint64(b[8:],24);copy(b[16:],"isom")
 x,err:=mp4.OpenFromBytes(b);brand:="<missing>";if x!=nil&&x.Ftyp!=nil&&x.Ftyp.MajorBrand!=""{brand=x.Ftyp.MajorBrand};fmt.Printf("EXTENDED_FTYP brand=%s error=%v\n",brand,err)
 bad:=[]byte{0,0,0,7,'f','r','e','e'};_,err=mp4.OpenFromBytes(bad);fmt.Printf("SHORT_BOX error=%v\n",err)
}
