package main
import (
 "bytes"
 "encoding/binary"
 "fmt"
 "io"
 "os"
 "time"
 "github.com/nareix/joy4/av"
 "github.com/nareix/joy4/codec/aacparser"
 "github.com/nareix/joy4/format/mp4"
 "github.com/nareix/joy4/format/mp4/mp4io"
)
func must(err error) {if err!=nil {panic(err)}}
func main() {
 f,e:=os.CreateTemp("", "reference-baseline-*.mp4"); must(e); defer os.Remove(f.Name()); defer f.Close()
 c,e:=aacparser.NewCodecDataFromMPEG4AudioConfigBytes([]byte{0x12,0x10}); must(e)
 m:=mp4.NewMuxer(f); must(m.WriteHeader([]av.CodecData{c}))
 for i:=0;i<5;i++ {must(m.WritePacket(av.Packet{Idx:0,Time:time.Duration(i)*20*time.Millisecond,Data:[]byte{byte(i),0xa5,0x5a}}))};must(m.WriteTrailer())
 _,e=f.Seek(0,0);must(e);d:=mp4.NewDemuxer(f);ss,e:=d.Streams();must(e);if len(ss)!=1 {panic("streams")}
 for i:=0;i<5;i++ {p,e:=d.ReadPacket();must(e);if !bytes.Equal(p.Data,[]byte{byte(i),0xa5,0x5a})||p.Time!=time.Duration(i)*20*time.Millisecond {panic("sample")}}
 _,e=d.ReadPacket();if e!=io.EOF {panic("EOF")};must(d.SeekToTime(40*time.Millisecond));p,e:=d.ReadPacket();must(e);if p.Data[0]!=2 {panic("seek")}
 fmt.Println("SELF_CONTAINED packets=5 bytes=15 seek_sample=2 EOF=ok")
 raw:=append(make([]byte,12),[]byte("media-A.bin\x00")...);binary.BigEndian.PutUint32(raw,uint32(len(raw)));copy(raw[4:8],"url ")
 var u mp4io.DataReferUrl;_,e=u.Unmarshal(raw,0);must(e);b:=make([]byte,u.Len());u.Marshal(b)
 fmt.Printf("URL_ROUNDTRIP input=%d output=%d location_preserved=%t\n",len(raw),len(b),bytes.Equal(raw,b))
}
