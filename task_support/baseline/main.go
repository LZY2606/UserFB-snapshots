package main
import (
 "fmt"
 "os"
 er "github.com/yalue/elf_reader"
 "github.com/yalue/elf_reader/task_support/fixtures"
)
func main(){
 for _,wide:=range []bool{false,true}{for _,big:=range []bool{false,true}{
  raw:=fixtures.Image(wide,big);f,e:=er.ParseELFFile(raw);if e!=nil{panic(e)}
  name,e:=f.GetSectionName(3);if e!=nil||name!=".symtab"{panic(fmt.Sprint("name ",name,e))}
  symbols,names,e:=f.GetSymbols(3);if e!=nil||len(symbols)!=2||names[1]!="entry"||symbols[1].GetValue()!=0x401000{panic(fmt.Sprint("symbols ",names,e))}
  rel,e:=f.GetRelocations(4);if e!=nil||len(rel)!=1||rel[0].SymbolIndex()!=1{panic(fmt.Sprint("rel ",e))}
  segment,e:=f.GetSegmentContent(0);if e!=nil||len(segment)!=4||segment[3]!=0xc3{panic(fmt.Sprint("segment ",e))}
  dyn,e:=f.DynamicEntries(6);if e!=nil||len(dyn)!=2||dyn[0].GetValue()!=0x402000{panic(fmt.Sprint("dynamic ",e))}
  // Existing Raw mode intentionally returns borrowed section bytes.
  data,e:=f.GetSectionContent(5);if e!=nil{panic(e)};data[0]=0xcc;if raw[0x600]!=0xcc{os.Exit(7)}
  fmt.Printf("RAW_SYMBOLS_OK wide=%v big=%v name=entry value=401000 reloc_symbol=1\n",wide,big)
  fmt.Println("RAW_ALIAS_OK segment_filesize=4 memorysize=20")
 }}
}
