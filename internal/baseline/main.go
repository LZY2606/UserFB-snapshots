package main
import (
 "bytes"
 "fmt"
 "github.com/minio/sio"
)
func main() {
 cfg := sio.Config{Key: bytes.Repeat([]byte{7},32), MinVersion:sio.Version20,MaxVersion:sio.Version20, CipherSuites:[]byte{sio.AES_GCM}, Rand:bytes.NewReader(make([]byte,32))}
 plain := bytes.Repeat([]byte("x"), (1<<16)+11)
 var enc bytes.Buffer
 if _,err:=sio.Encrypt(&enc,bytes.NewReader(plain),cfg);err!=nil {panic(err)}
 var good bytes.Buffer
 if _,err:=sio.Decrypt(&good,bytes.NewReader(enc.Bytes()),cfg);err!=nil || !bytes.Equal(good.Bytes(),plain) {panic("roundtrip failed")}
 bad:=append([]byte(nil),enc.Bytes()...); bad[len(bad)-1]^=1
 var early bytes.Buffer
 n,err:=sio.Decrypt(&early,bytes.NewReader(bad),cfg)
 if err==nil || n!=1<<16 || early.Len()!=1<<16 {panic("expected authenticated prefix delivery")}
 var prefix bytes.Buffer
 w,err:=sio.DecryptWriter(&prefix,cfg);if err!=nil {panic(err)}
 if _,err=w.Write(enc.Bytes()[:(1<<16)+32]);err!=nil {panic(err)}
 if err=w.Close();err!=nil || prefix.Len()!=1<<16 {panic("starting writer behavior changed")}
 fmt.Println("baseline: full roundtrip OK; bad tail releases 65536 bytes; prefix-only writer Close=nil")
}
