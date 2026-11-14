package main
import (
 "bytes"
 "fmt"
 "os"
 "github.com/go-interpreter/wagon/wasm"
 "github.com/go-interpreter/wagon/disasm"
 "github.com/go-interpreter/wagon/validate"
)
func need(ok bool, msg string) {if !ok {fmt.Fprintln(os.Stderr,msg);os.Exit(1)}}
func main(){
 raw:=[]byte{0,97,115,109,1,0,0,0,1,4,1,96,0,0,3,2,1,0,4,5,1,112,1,1,1,9,7,1,0,65,0,11,1,0,10,9,1,7,0,65,0,17,0,0,11}
 m,e:=wasm.DecodeModule(bytes.NewReader(raw));need(e==nil,fmt.Sprint(e));need(len(m.Elements.Entries)==1&&len(m.Elements.Entries[0].Elems)==1,"legacy element identity")
 var out bytes.Buffer;e=wasm.EncodeModule(&out,m);need(e==nil&&bytes.Equal(raw,out.Bytes()),"legacy module roundtrip")
 fn:=wasm.Function{Sig:&m.Types.Entries[0],Body:&m.Code.Bodies[0]};m.FunctionIndexSpace=[]wasm.Function{fn}
 e=validate.VerifyModule(m);need(e==nil,fmt.Sprint(e))
 ins,e:=disasm.Disassemble(fn.Body.Code);need(e==nil&&len(ins)==2,"instruction decode")
 code,e:=disasm.Assemble(ins);need(e==nil&&bytes.Equal(code,fn.Body.Code),"instruction roundtrip")
 fmt.Printf("BASELINE_OK elements=%d instructions=%d module_bytes=%d\n",len(m.Elements.Entries),len(ins),len(raw))
}
