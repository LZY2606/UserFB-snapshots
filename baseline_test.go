package flac

import (
 "bytes"
 "encoding/binary"
 "crypto/md5"
 "fmt"
 "io"
 "testing"
 "github.com/mewkiz/flac/frame"
 "github.com/mewkiz/flac/meta"
)

type memoryFile struct { data []byte; pos int64 }
func (m *memoryFile) Write(p []byte)(int,error){
 end:=m.pos+int64(len(p));if end>int64(len(m.data)){m.data=append(m.data,make([]byte,end-int64(len(m.data)))...)}
 copy(m.data[m.pos:end],p);m.pos=end;return len(p),nil
}
func(m *memoryFile) Seek(n int64,w int)(int64,error){switch w{case io.SeekCurrent:n+=m.pos;case io.SeekEnd:n+=int64(len(m.data))};if n<0{return m.pos,fmt.Errorf("negative")};m.pos=n;return n,nil}
func sampleInfo()*meta.StreamInfo{return &meta.StreamInfo{BlockSizeMin:16,BlockSizeMax:16,SampleRate:8000,NChannels:2,BitsPerSample:16}}
func sampleFrame()*frame.Frame{
 f:=&frame.Frame{Header:frame.Header{BlockSize:16,SampleRate:8000,Channels:frame.ChannelsLR,BitsPerSample:16}}
 for ch:=0;ch<2;ch++{s:=make([]int32,16);for i:=range s{s[i]=int32((i*i+ch*7)*11-500)};f.Subframes=append(f.Subframes,&frame.Subframe{SubHeader:frame.SubHeader{Pred:frame.PredVerbatim},NSamples:16,Samples:s})};return f
}
// Real encoder and parser calls document the unmodified production tree.
func TestBaseline(t *testing.T){
 m:=&memoryFile{};e,err:=NewEncoder(m,sampleInfo());if err!=nil{t.Fatal(err)}
 want:=sampleFrame();hash:=md5.New();for block:=0;block<2;block++{
  f:=sampleFrame();for i:=0;i<16;i++{for ch:=0;ch<2;ch++{var b [2]byte;binary.LittleEndian.PutUint16(b[:],uint16(f.Subframes[ch].Samples[i]));hash.Write(b[:])}}
  if err=e.WriteFrame(f);err!=nil{t.Fatal(err)}
 }
 if err=e.Close();err!=nil{t.Fatal(err)}
 stream,err:=New(bytes.NewReader(m.data));if err!=nil{t.Fatal(err)}
 count:=0;for{f,er:=stream.ParseNext();if er==io.EOF{break};if er!=nil{t.Fatal(er)};for ch:=0;ch<2;ch++{for i,x:=range f.Subframes[ch].Samples{if x!=want.Subframes[ch].Samples[i]{t.Fatal("PCM mismatch")}}};count+=int(f.BlockSize)}
 if count!=32 || stream.Info.NSamples!=32 || !bytes.Equal(hash.Sum(nil),stream.Info.MD5sum[:]){t.Fatal("normal audio or digest mismatch")}
 fmt.Printf("FLAC_PRODUCTION samples=%d md5=%x bytes=%d\n",count,stream.Info.MD5sum,len(m.data))
 var out bytes.Buffer;e,err=NewEncoder(&out,sampleInfo());if err!=nil{t.Fatal(err)}
 bad:=sampleFrame();sf:=bad.Subframes[0];sf.Pred=frame.PredFIR;sf.Order=1;sf.CoeffPrec=4;sf.Coeffs=nil;sf.RiceSubframe=&frame.RiceSubframe{Partitions:[]frame.RicePartition{{Param:4}}}
 start:=out.Len();err=e.WriteFrame(bad);if err==nil{t.Fatal("invalid FIR should be rejected")};leaked:=out.Len()-start
 good:=sampleFrame();if err=e.WriteFrame(good);err!=nil{t.Fatal(err)}
 if leaked==0 && good.Num!=0{t.Fatal("rejected frame advanced counter")}
 fmt.Printf("FLAC_RETRY_OUTPUT leaked_bytes=%d next_num=%d\n",leaked,good.Num)
 prefix:=bytes.Repeat([]byte{0x5a},64);m=&memoryFile{data:append([]byte(nil),prefix...),pos:64};e,err=NewEncoder(m,sampleInfo());if err!=nil{t.Fatal(err)}
 if err=e.WriteFrame(sampleFrame());err!=nil{t.Fatal(err)};end:=m.pos;if err=e.Close();err!=nil{t.Fatal(err)}
 fmt.Printf("FLAC_ORIGIN prefix_kept=%t end_restored=%t\n",bytes.Equal(m.data[:64],prefix),m.pos==end)
}
