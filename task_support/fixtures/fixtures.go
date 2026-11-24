// Package fixtures constructs inert PE data for read-only debug analysis.
package fixtures

import "encoding/binary"

var le = binary.LittleEndian
func w16(b []byte, o int, x uint16) { le.PutUint16(b[o:], x) }
func w32(b []byte, o int, x uint32) { le.PutUint32(b[o:], x) }
func pstr(b []byte, o int, s string) { b[o] = byte(len(s)); copy(b[o+1:], s) }

// Wrap stores a CodeView payload in a PE32+ file with three ordinary sections.
// No code is run. codeview is copied; callers retain ownership of their input.
func Wrap(codeview []byte) []byte {
 b:=make([]byte,0x1c00)
 copy(b,"MZ"); w32(b,0x3c,0x80); copy(b[0x80:],"PE\x00\x00")
 w16(b,0x84,0x8664);w16(b,0x86,3);w16(b,0x94,240);w16(b,0x96,0x2022)
 o:=0x98;w16(b,o,0x20b);le.PutUint64(b[o+24:],0x140000000)
 w32(b,o+32,0x1000);w32(b,o+36,0x200);w32(b,o+56,0x7000)
 w32(b,o+60,0x400);w16(b,o+68,3);w32(b,o+108,16)
 w32(b,o+112+6*8,0x5000);w32(b,o+112+6*8+4,28)
 for i,s:=range []struct{name string;rva,raw,size uint32}{{".first",0x1000,0x400,0x400},{".second",0x3000,0x800,0x400},{".debug",0x5000,0xc00,0x1000}} {
  h:=o+240+i*40;copy(b[h:],s.name);w32(b,h+8,s.size);w32(b,h+12,s.rva)
  w32(b,h+16,s.size);w32(b,h+20,s.raw);w32(b,h+36,0x40000040)
 }
 w32(b,0xc00+12,2);w32(b,0xc00+16,uint32(len(codeview)))
 w32(b,0xc00+20,0x5100);w32(b,0xc00+24,0xd00);copy(b[0xd00:],codeview)
 return b
}

func NB10() []byte {
 p:=make([]byte,40);copy(p,"NB10");w32(p,8,0x12345678);w32(p,12,3)
 copy(p[16:],"offline-demo.pdb\x00");return Wrap(p)
}

// Embedded contains module 7 on logical segment 2 and module 9 on segment 1.
// Its source-file and source-line records are deliberately noncontiguous.
// The constructor contains data only, not an address-to-line implementation.
func Embedded(signature string) []byte {
 p:=make([]byte,0x380);copy(p,signature);w32(p,4,0x300)
 // Global segment map: logical 1 -> physical PE section 2 + 0x80;
 // logical 2 -> physical PE section 1 + 0x30.
 w16(p,0x20,2);w16(p,0x22,2)
 for i,d:=range []struct{frame uint16;off uint32}{{2,0x80},{1,0x30}} {
  o:=0x24+i*20;w16(p,o,0x109);w16(p,o+6,d.frame)
  w16(p,o+8,0xffff);w16(p,o+10,0xffff);w32(p,o+12,d.off);w32(p,o+16,0x100)
 }
 module:=func(o int,seg uint16,start,size uint32,name string){
  w16(p,o+4,1);copy(p[o+6:],"CV");w16(p,o+8,seg)
  w32(p,o+12,start);w32(p,o+16,size);pstr(p,o+20,name)
 }
 module(0x60,2,0x10,0x30,"alpha.obj");module(0x90,1,0x40,0x20,"beta.obj")
 // All baseSrcFile and baseSrcLn offsets are relative to their sstSrcModule.
 source:=func(o int,seg uint16,start,end uint32,name string,offsets []uint32,lines []uint16){
  w16(p,o,1);w16(p,o+2,1);w32(p,o+4,0x30)
  w32(p,o+8,start);w32(p,o+12,end);w16(p,o+16,seg)
  f:=o+0x30;w16(p,f,1);w32(p,f+4,0x70)
  w32(p,f+8,start);w32(p,f+12,end);pstr(p,f+16,name)
  t:=o+0x70;w16(p,t,seg);w16(p,t+2,uint16(len(lines)))
  for i,x:=range offsets {w32(p,t+4+i*4,x)}
  for i,x:=range lines {w16(p,t+4+len(lines)*4+i*2,x)}
 }
 source(0xc0,2,0x12,0x2a,"alpha.c",[]uint32{0x14,0x20},[]uint16{7,9})
 source(0x180,1,0x40,0x5f,"beta.c",[]uint32{0x40,0x50},[]uint16{41,44})
 // Directory follows the records; sources precede their module descriptors.
 w16(p,0x300,16);w16(p,0x302,12);w32(p,0x304,5)
 entries:=[]struct{kind,mod uint16;off,size uint32}{
  {0x127,7,0xc0,0xa0},{0x120,9,0x90,0x30},{0x12d,0xffff,0x20,44},
  {0x127,9,0x180,0xa0},{0x120,7,0x60,0x30},
 }
 for i,e:=range entries {o:=0x310+i*12;w16(p,o,e.kind);w16(p,o+2,e.mod);w32(p,o+4,e.off);w32(p,o+8,e.size)}
 return Wrap(p)
}
