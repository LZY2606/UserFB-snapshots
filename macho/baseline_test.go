package macho
import("bytes";"encoding/binary";"testing")
// Complete, inert Mach-O bytes. No host-generated object or code execution.
func testBindingFixture(wide bool) []byte {
 b:=make([]byte,512);put:=func(o int,v uint32){binary.LittleEndian.PutUint32(b[o:],v)}
 hdr,seg:=28,56;magic:=uint32(Magic32);cpu:=uint32(Cpu386)
 if wide {hdr,seg=32,72;magic=Magic64;cpu=uint32(CpuAmd64)}
 put(0,magic);put(4,cpu);put(8,3);put(12,uint32(TypeExec));put(16,2);put(20,uint32(seg+48))
 o:=hdr;cmd:=uint32(LoadCmdSegment);if wide {cmd=uint32(LoadCmdSegment64)};put(o,cmd);put(o+4,uint32(seg));copy(b[o+8:],"__DATA")
 if wide {binary.LittleEndian.PutUint64(b[o+24:],0x1000);binary.LittleEndian.PutUint64(b[o+32:],768);binary.LittleEndian.PutUint64(b[o+48:],512)} else {put(o+24,0x1000);put(o+28,768);put(o+36,512)}
 o+=seg;put(o,uint32(LoadCmdDylinkInfo));put(o+4,48)
 normal:=[]byte{0x51,0x11,0x70,0x20,0x40,'_','x',0,0x90,0}
 lazy:=[]byte{0x51,0x11,0x70,0x30,0x40,'_','y',0,0x90,0}
 put(o+16,384);put(o+20,uint32(len(normal)));put(o+32,416);put(o+36,uint32(len(lazy)));copy(b[384:],normal);copy(b[416:],lazy)
 return b
}
func TestMachOBaseline(t *testing.T){
 for _,wide:=range []bool{false,true}{
  raw:=testBindingFixture(wide);before:=append([]byte(nil),raw...)
  f,e:=NewFile(bytes.NewReader(raw));if e!=nil {t.Fatal(e)}
  if f.DylinkInfo==nil||!bytes.Equal(f.DylinkInfo.BindingInfoDat,raw[384:394])||!bytes.Equal(f.DylinkInfo.LazyBindingDat,raw[416:426]) {t.Fatal("stream read")}
  sg,ok:=f.Loads[0].(*Segment);if !ok||sg.Addr!=0x1000||sg.Memsz!=768||sg.Filesz!=512 {t.Fatal("segment")}
  got,e:=sg.Data();if e!=nil||!bytes.Equal(got,raw){t.Fatal("segment data",e)}
  if !bytes.Equal(before,raw){t.Fatal("modified input")}
 }
 t.Log("MACHO_STREAM_BASELINE_OK bits=32,64 normal=10 lazy=10 readonly=1")
}
