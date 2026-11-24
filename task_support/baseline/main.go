package main

import (
 "bytes"
 "debug/dwarf"
 "encoding/binary"
 "fmt"
 "os"
 "path/filepath"

 "github.com/Binject/debug/elf"
)
func must(e error){if e!=nil{panic(e)}}
func need(ok bool,why string){if !ok{panic(why)}}
func section(f *elf.File,n string)[]byte{s:=f.Section(n);need(s!=nil,"missing "+n);b,e:=s.Data();must(e);return b}
func main(){
 // The original production DWARF path on an upstream, ordinary ELF fixture.
 ordinary,e:=elf.Open("elf/testdata/gcc-amd64-linux-exec");must(e);defer ordinary.Close()
 d,e:=ordinary.DWARF();must(e);r:=d.Reader();cu,e:=r.Next();must(e);need(cu!=nil&&cu.Tag==dwarf.TagCompileUnit,"ordinary CU");fmt.Println("ORDINARY_DWARF_OK")
 for _,name:=range []string{"primary","alternate"}{
  dir:=filepath.Join("task_support","samples",name)
  raw,e:=os.ReadFile(filepath.Join(dir,"device.elf"));must(e);image,e:=elf.NewFile(bytes.NewReader(raw));must(e)
  need(image.Class==elf.ELFCLASS64&&image.Data==elf.ELFDATA2LSB&&image.Type==elf.ET_EXEC,"sample format")
  need(image.Section(".symtab")==nil&&image.Section(".dynsym")==nil,"symbols must be absent")
  info:=section(image,".debug_info"); count:=0
  for off:=0;off<len(info);{need(off+20<=len(info),"short skeleton");n:=int(binary.LittleEndian.Uint32(info[off:]));need(n>=16&&off+4+n<=len(info),"unit bounds");need(binary.LittleEndian.Uint16(info[off+4:])==5&&info[off+6]==4,"v5 skeleton");count++;off+=n+4}
  need(count==3,"three skeletons");addr:=section(image,".debug_addr");need(len(addr)==48,"three address contributions")
  p,e:=elf.Open(filepath.Join(dir,"device.elf.dwp"));must(e);index:=section(p,".debug_cu_index");need(len(index)>=16,"index header")
  u:=func(off int)uint32{return binary.LittleEndian.Uint32(index[off:])};need(u(0)==5&&u(8)==3&&u(12)>=3,"CU index dimensions")
  for _,n:=range []string{".debug_info.dwo",".debug_abbrev.dwo",".debug_str.dwo",".debug_str_offsets.dwo",".debug_rnglists.dwo"}{need(len(section(p,n))>0,"empty "+n)}
  fmt.Printf("SAMPLE_STRUCTURE_OK %s skeletons=%d cu_index_rows=%d symbols=absent\n",name,count,u(8));p.Close();image.Close()
 }
 fmt.Println("SPLIT_BASELINE_OK")
}
