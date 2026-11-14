package main
import (
 "bytes"
 "fmt"
 "os"
 "github.com/Binject/debug/pe"
 "github.com/Binject/debug/task_support/fixture"
)
func main(){if err:=run();err!=nil{fmt.Fprintln(os.Stderr,err);os.Exit(1)}}
func run() error {
 raw:=fixture.Basic(false)
 f,err:=pe.NewFile(bytes.NewReader(raw));if err!=nil{return fmt.Errorf("ordinary object: %w",err)}
 if len(f.Sections)!=1||len(f.Symbols)!=1||len(f.COFFSymbols)!=1{return fmt.Errorf("ordinary object counts incorrect")}
 data,err:=f.Sections[0].Data();if err!=nil{return err}
 if !bytes.Equal(data,[]byte{0x90,0x90,0x90,0xc3})||f.Symbols[0].Name!="anchor"||f.Symbols[0].SectionNumber!=1||len(f.Sections[0].Relocs)!=1||f.Sections[0].Relocs[0].SymbolTableIndex!=0{return fmt.Errorf("ordinary production values incorrect")}
 fmt.Println("COFF_OK section=.text symbol=anchor raw_slot=0 relocation=0 payload=909090c3")
 big,err:=pe.NewFile(bytes.NewReader(fixture.Basic(true)))
 if err!=nil{fmt.Printf("BIGOBJ_CURRENT_MISSING %v\n",err)}else{
  if len(big.Sections)!=1||len(big.Symbols)!=1||big.Symbols[0].Name!="long_anchor_name"{return fmt.Errorf("BigObj silently decoded wrong")}
  fmt.Println("BIGOBJ_AVAILABLE section=.text symbol=long_anchor_name")
 }
 fmt.Println("BASELINE_OK ordinary_coff_read=1 bigobj_observation=1");return nil
}
