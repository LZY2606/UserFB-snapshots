// Package fixture emits data-only modules; it never instantiates or executes Wasm.
package fixture

func U(v uint64) []byte {b:=[]byte{};for {x:=byte(v&127);v>>=7;if v!=0{x|=128};b=append(b,x);if v==0{return b}}}
func sec(b []byte,id byte,p []byte)[]byte{b=append(b,id);b=append(b,U(uint64(len(p)))...);return append(b,p...)}
func head()[]byte{return []byte{0,0x61,0x73,0x6d,1,0,0,0}}
func code(b,body []byte)[]byte{p:=append([]byte{0},body...);p=append(U(uint64(len(p))),p...);p=append([]byte{1},p...);return sec(b,10,p)}
// Legacy is a single-memory MVP module with an i32.load at Body offset 2.
func Legacy()[]byte{b:=head();b=sec(b,1,[]byte{1,0x60,0,0});b=sec(b,3,[]byte{1,0});b=sec(b,5,[]byte{1,1,1,2});return code(b,[]byte{0x41,0,0x28,2,0,0x1a,0x0b})}
func Two32()[]byte{b:=head();return sec(b,5,[]byte{2,1,1,2,1,1,2})}
func One64()[]byte{b:=head();p:=[]byte{1,5};p=append(p,U(1<<32)...);p=append(p,U((1<<32)+1)...);return sec(b,5,p)}
// Mixed imports one function and one memory32, defines two memory64 objects,
// and has a large-offset load, mixed copy, size, grow and active data.
func Mixed()[]byte{
 b:=head();b=sec(b,1,[]byte{1,0x60,0,0})
 // env.f : func0 ; env.ram : memory32(min=1,max=2).
 b=sec(b,2,[]byte{2,3,'e','n','v',1,'f',0,0,3,'e','n','v',3,'r','a','m',2,1,1,2})
 b=sec(b,3,[]byte{1,0});p:=[]byte{2,5};p=append(p,U(1<<32)...);p=append(p,U((1<<32)+1)...);p=append(p,5,0,2);b=sec(b,5,p)
 b=sec(b,7,[]byte{3,1,'a',2,0,1,'b',2,1,1,'c',2,2})
 body:=[]byte{0x42,0,0x28,0x42,1};body=append(body,U(5<<30)...);body=append(body,0x1a,0x42,0,0x41,0,0x41,0,0xfc,10,1,0,0x3f,2,0x1a,0x42,0,0x40,2,0x1a,0x0b)
 b=code(b,body);b=sec(b,11,[]byte{1,2,1,0x42,0,0x0b,3,0x11,0x22,0x33});return b
}
