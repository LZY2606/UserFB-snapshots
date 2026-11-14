// Package fixture builds data-only Mach-O images. No machine code is run.
package fixture
import("bytes";"encoding/binary";"github.com/blacktop/go-macho/types")
const Base uint64=0x100000000
// Image contains two segment-relative slots and a data-only category record.
// Threaded uses legacy arm64e opcodes; plain uses the classic rebase stream.
func Image(threaded bool) []byte {
 var h bytes.Buffer
 write:=func(v any){if err:=binary.Write(&h,binary.LittleEndian,v);err!=nil{panic(err)}}
 segsize:=binary.Size(types.Segment64{}); secsize:=binary.Size(types.Section64{}); dysize:=binary.Size(types.DyldInfoCmd{}); libsize:=40
 write(types.FileHeader{Magic:types.Magic64,CPU:types.CPUArm64,SubCPU:types.CPUSubtypeArm64E,Type:types.MH_DYLIB,NCommands:4,SizeCommands:uint32(2*segsize+3*secsize+dysize+libsize)})
 segment:=func(name string,off uint64,n uint32){s:=types.Segment64{LoadCmd:types.LC_SEGMENT_64,Len:uint32(segsize)+n*uint32(secsize),Addr:Base+off,Memsz:0x1000,Offset:off,Filesz:0x1000,Nsect:n};copy(s.Name[:],name);write(s)}
 section:=func(name string,off,size uint64){s:=types.Section64{Addr:Base+off,Size:size,Offset:uint32(off)};copy(s.Name[:],name);copy(s.Seg[:],"__DATA");write(s)}
 segment("__TEXT",0,0);segment("__DATA",0x1000,3)
 section("__objc_catlist",0x1100,8);section("__objc_const",0x1200,0x100);section("__cstring",0x1400,0x40)
 var bind,rebase []byte
 if threaded{
  // table[0] refers to the external class. APPLY starts at __DATA + 0x100.
  bind=append([]byte{0xd0,1,0x11,0x40},[]byte("_OBJC_CLASS_$_Host\x00")...)
  bind=append(bind,0x51,0x90,0x71,0x80,2,0xd1,0)
 }else{rebase=[]byte{0x11,0x21,0x80,2,0x51,0}}
 write(types.DyldInfoCmd{LoadCmd:types.LC_DYLD_INFO_ONLY,Len:uint32(dysize),RebaseOff:0x1800,RebaseSize:uint32(len(rebase)),BindOff:0x1840,BindSize:uint32(len(bind))})
 write(types.DylibCmd{LoadCmd:types.LC_LOAD_DYLIB,Len:uint32(libsize),NameOffset:24});h.WriteString("/fixture/lib\x00");for h.Len()%8!=0{h.WriteByte(0)}
 data:=make([]byte,0x2000);copy(data,h.Bytes());copy(data[0x1800:],rebase);copy(data[0x1840:],bind)
 set:=func(off int,v uint64){binary.LittleEndian.PutUint64(data[off:],v)}
 set(0x1100,Base+0x1200);set(0x1200,Base+0x1400)
 if threaded{
  // A rebase, followed by the category name rebase, followed by external class bind.
  set(0x1100,Base+0x1200|uint64(0x100/8)<<51)
  set(0x1200,Base+0x1400|1<<51)
  set(0x1208,1<<62)
 }
 copy(data[0x1400:],"Extras\x00")
 return data
}
