// Package fixture builds data-only ELF/DWARF images. It contains no typed evaluator.
package fixture

import "encoding/binary"

func u16(b *[]byte, v uint16) { *b=binary.LittleEndian.AppendUint16(*b,v) }
func u32(b *[]byte, v uint32) { *b=binary.LittleEndian.AppendUint32(*b,v) }
func str(b *[]byte, s string) { *b=append(*b, []byte(s)...); *b=append(*b,0) }
// Unit records CU header offset and CU-relative base type offsets.
type Unit struct { Start uint32; Byte, Word, Wide uint64 }
// Sections returns two DWARF5 compilation units with the same relative type
// offsets but different signedness. Their expressions are hand-encoded.
func Sections() (abbrev,info []byte, units []Unit) {
 // CU(children), base_type(name/string,encoding/data1,size/data1), variable.
 abbrev=[]byte{1,0x11,1,0,0,2,0x24,0,3,8,0x3e,0x0b,0x0b,0x0b,0,0,3,0x34,0,3,8,2,0x18,0,0,0}
 for i:=0;i<2;i++ {
  start:=len(info); b:=make([]byte,4);u16(&b,5);b=append(b,1,8);u32(&b,0);b=append(b,1)
  unit:=Unit{Start:uint32(start),Byte:uint64(len(b))}
  b=append(b,2);str(&b,"byte");b=append(b,byte(5+2*i),1)
  unit.Word=uint64(len(b));b=append(b,2);str(&b,"word");b=append(b,5,2)
  unit.Wide=uint64(len(b));b=append(b,2);str(&b,"wide");b=append(b,7,8)
  b=append(b,3);str(&b,"plain");b=append(b,1,0x37)
  b=append(b,3);str(&b,"narrow");expr:=[]byte{0xa4,byte(unit.Byte),1,0xfe,0xa8,byte(unit.Word),0x9f};b=append(b,byte(len(expr)));b=append(b,expr...)
  b=append(b,0);binary.LittleEndian.PutUint32(b,uint32(len(b)-4));info=append(info,b...);units=append(units,unit)
 }
 return
}
// ELF wraps only debug sections in a legal relocatable x86-64 object; no code.
func ELF() []byte {
 ab,info,_:=Sections();names:=[]byte("\x00.shstrtab\x00.debug_abbrev\x00.debug_info\x00")
 b:=make([]byte,64);copy(b,[]byte{0x7f,'E','L','F',2,1,1});binary.LittleEndian.PutUint16(b[16:],1);binary.LittleEndian.PutUint16(b[18:],62);binary.LittleEndian.PutUint32(b[20:],1)
 type sec struct{ name uint32; off,size uint64; kind uint32 };s:=[]sec{{},{1,uint64(len(b)),uint64(len(names)),3}}
 b=append(b,names...);s=append(s,sec{11,uint64(len(b)),uint64(len(ab)),1});b=append(b,ab...);s=append(s,sec{25,uint64(len(b)),uint64(len(info)),1});b=append(b,info...)
 for len(b)%8!=0 {b=append(b,0)};shoff:=len(b)
 for _,x:=range s {h:=make([]byte,64);binary.LittleEndian.PutUint32(h,x.name);binary.LittleEndian.PutUint32(h[4:],x.kind);binary.LittleEndian.PutUint64(h[24:],x.off);binary.LittleEndian.PutUint64(h[32:],x.size);binary.LittleEndian.PutUint64(h[48:],1);b=append(b,h...)}
 binary.LittleEndian.PutUint64(b[40:],uint64(shoff));binary.LittleEndian.PutUint16(b[52:],64);binary.LittleEndian.PutUint16(b[58:],64);binary.LittleEndian.PutUint16(b[60:],4);binary.LittleEndian.PutUint16(b[62:],1);return b
}
