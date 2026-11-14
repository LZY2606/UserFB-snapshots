// Package fixture constructs deterministic legal COFF objects, without executing them.
package fixture

import "encoding/binary"

var le = binary.LittleEndian
var bigClass = [16]byte{0xc7,0xa1,0xba,0xd1,0xee,0xba,0xa9,0x4b,0xaf,0x20,0xfa,0xf6,0x6a,0xa4,0xdc,0xb8}

// Basic has one real section, one external symbol, and one relocation.
// It intentionally contains no weak-reference example or target implementation.
func Basic(big bool) []byte {
 header, stride := 20,18
 name := "anchor"
 if big { header,stride=56,20; name="long_anchor_name" }
 dataOff:=header+40; relocOff:=dataOff+4; symOff:=relocOff+10
 strings:=[]byte{4,0,0,0}
 if big { strings=append(strings,[]byte(name)...);strings=append(strings,0);le.PutUint32(strings,uint32(len(strings))) }
 b:=make([]byte,symOff+stride+len(strings))
 if big {
  le.PutUint16(b[2:],0xffff);le.PutUint16(b[4:],2);le.PutUint16(b[6:],0x8664)
  copy(b[12:28],bigClass[:]);le.PutUint32(b[44:],1);le.PutUint32(b[48:],uint32(symOff));le.PutUint32(b[52:],1)
 } else {
  le.PutUint16(b,0x8664);le.PutUint16(b[2:],1);le.PutUint32(b[8:],uint32(symOff));le.PutUint32(b[12:],1)
 }
 sh:=b[header:header+40];copy(sh,".text");le.PutUint32(sh[16:],4);le.PutUint32(sh[20:],uint32(dataOff));le.PutUint32(sh[24:],uint32(relocOff));le.PutUint16(sh[32:],1);le.PutUint32(sh[36:],0x60500020)
 copy(b[dataOff:],[]byte{0x90,0x90,0x90,0xc3});le.PutUint16(b[relocOff+8:],0x0004)
 s:=b[symOff:symOff+stride]
 if big { le.PutUint32(s[4:],4);le.PutUint32(s[12:],1);s[18]=2 } else {copy(s,name);le.PutUint16(s[12:],1);s[16]=2}
 copy(b[symOff+stride:],strings);return b
}
