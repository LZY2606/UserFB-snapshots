package main
import("bytes";"encoding/binary";"fmt";"strings";"github.com/blacktop/go-macho/pkg/fixupchains";"github.com/blacktop/go-macho/types")
func fixture(multi bool,start uint16)*fixupchains.DyldChainedFixups{
 starts:=[]uint16{start};if multi{starts=[]uint16{0x8001,0x10,0x8030}}
 size:=uint32(22+2*len(starts));end:=uint32(36)+size
 h:=fixupchains.DyldChainedFixupsHeader{StartsOffset:28,ImportsOffset:end,SymbolsOffset:end,ImportsFormat:fixupchains.DC_IMPORT}
 var b bytes.Buffer;put:=func(v any){if e:=binary.Write(&b,binary.LittleEndian,v);e!=nil{panic(e)}}
 put(h);put(uint32(1));put(uint32(8))
 put(fixupchains.DyldChainedStartsInSegment{Size:size,PageSize:4096,PointerFormat:fixupchains.DYLD_CHAINED_PTR_64,PageCount:1});put(starts)
 mem:=make([]byte,4096);binary.LittleEndian.PutUint64(mem[start:],0x1234);binary.LittleEndian.PutUint64(mem[0x30:],0x5678)
 var r types.MachoReader=types.NewCustomSectionReader(bytes.NewReader(mem),nil,0,int64(len(mem)))
 return fixupchains.NewChainedFixups(bytes.NewReader(b.Bytes()),&r,binary.LittleEndian)
}
func main(){
 a,e:=fixture(false,0x10).Parse();if e!=nil||len(a.Starts[0].Fixups)!=1||a.Starts[0].Fixups[0].Offset()!=0x10||a.Starts[0].Fixups[0].(fixupchains.Rebase).Target()!=0x1234{panic("single chain failed")}
 fmt.Println("SINGLE_CHAIN_OK offset=10 target=1234")
 miss:=fixture(false,0x14);if _,e:=miss.GetFixupAtOffset(0x14);e!=fixupchains.ErrNoFixupAtOffset{panic("unexpected lazy result")};p,e:=miss.Parse();if e!=nil||p.Starts[0].Fixups[0].Offset()!=0x14{panic("eager stride failed")};fmt.Println("CURRENT_STRIDE4_LOOKUP_MISSES offset=14 eager=present")
 hit:=false
 func(){defer func(){if v:=recover();v!=nil{hit=strings.Contains(fmt.Sprint(v),"index out of range [1] with length 1");fmt.Printf("MULTISTART_ERROR %v\n",v)}}();_,e:=fixture(true,0x10).Parse();if e!=nil{panic(e)}}()
 if !hit{panic("expected current multistart panic")};fmt.Println("CURRENT_MULTISTART_PANIC_CONFIRMED")
}
