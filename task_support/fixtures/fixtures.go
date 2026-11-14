// Package fixtures constructs small, legal ELF images without host tools.
package fixtures

import "encoding/binary"

func Image(wide, big bool) []byte {
 b:=make([]byte,0x800)
 var order binary.ByteOrder=binary.LittleEndian
 if big {order=binary.BigEndian}
 w16:=func(o int,v uint16){order.PutUint16(b[o:],v)}
 w32:=func(o int,v uint32){order.PutUint32(b[o:],v)}
 w64:=func(o int,v uint64){order.PutUint64(b[o:],v)}
 copy(b,"\x7fELF");b[4]=1;b[5]=1;b[6]=1
 if wide {b[4]=2};if big {b[5]=2}
 w16(16,2);w16(18,3);w32(20,1)
 names:="\x00.shstrtab\x00.strtab\x00.symtab\x00.rela.text\x00.text\x00.dynamic\x00.bss\x00"
 copy(b[0x400:],names);copy(b[0x480:],"\x00entry\x00")
 nameOffsets:=[]uint32{0,1,11,19,27,38,44,53}
 kinds:=[]uint32{0,3,3,2,4,1,6,8}
 offsets:=[]uint64{0,0x400,0x480,0x500,0x580,0x600,0x680,0x700}
 sizes:=[]uint64{0,uint64(len(names)),7,32,12,4,16,16}
 entries:=[]uint64{0,0,0,16,12,0,8,0}
 if wide {sizes[3]=48;sizes[4]=24;sizes[6]=32;entries[3]=24;entries[4]=24;entries[6]=16}
 if wide {
  w16(18,62);w64(24,0x401000);w64(32,0x40);w64(40,0x100);w16(52,64);w16(54,56);w16(56,1);w16(58,64);w16(60,8);w16(62,1)
  w32(0x40,1);w32(0x44,5);w64(0x48,0x600);w64(0x50,0x401000);w64(0x58,0x401000);w64(0x60,4);w64(0x68,20);w64(0x70,1)
  for i:=0;i<8;i++ {o:=0x100+i*64;w32(o,nameOffsets[i]);w32(o+4,kinds[i]);w64(o+24,offsets[i]);w64(o+32,sizes[i]);w64(o+48,1);w64(o+56,entries[i]);if i==3 {w32(o+40,2);w32(o+44,1)};if i==4 {w32(o+40,3);w32(o+44,5)};if i==5 {w64(o+8,6);w64(o+16,0x401000)}}
  w32(0x518,1);b[0x51c]=0x12;w16(0x51e,5);w64(0x520,0x401000);w64(0x528,4)
  w64(0x580,0x401000);w64(0x588,1<<32|1);w64(0x590,5);w64(0x680,5);w64(0x688,0x402000)
 } else {
  w32(24,0x401000);w32(28,0x40);w32(32,0x100);w16(40,52);w16(42,32);w16(44,1);w16(46,40);w16(48,8);w16(50,1)
  w32(0x40,1);w32(0x44,0x600);w32(0x48,0x401000);w32(0x4c,0x401000);w32(0x50,4);w32(0x54,20);w32(0x58,5);w32(0x5c,1)
  for i:=0;i<8;i++ {o:=0x100+i*40;w32(o,nameOffsets[i]);w32(o+4,kinds[i]);w32(o+16,uint32(offsets[i]));w32(o+20,uint32(sizes[i]));w32(o+32,1);w32(o+36,uint32(entries[i]));if i==3 {w32(o+24,2);w32(o+28,1)};if i==4 {w32(o+24,3);w32(o+28,5)};if i==5 {w32(o+8,6);w32(o+12,0x401000)}}
  w32(0x510,1);w32(0x514,0x401000);w32(0x518,4);b[0x51c]=0x12;w16(0x51e,5)
  w32(0x580,0x401000);w32(0x584,1<<8|1);w32(0x588,5);w32(0x680,5);w32(0x684,0x402000)
 }
 copy(b[0x600:],[]byte{0x90,0x90,0x90,0xc3});return b
}
