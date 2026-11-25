package main

import (
 "bytes"
 "fmt"
 "os"
 "github.com/Eyevinn/mp4ff/bits"
 "github.com/Eyevinn/mp4ff/mp4"
)

func must(ok bool, msg string) { if !ok { fmt.Fprintln(os.Stderr,msg); os.Exit(1) } }
func fragment(seq uint32, base uint64, payload byte, explicit bool) []byte {
 f,e:=mp4.CreateFragment(seq,1); must(e==nil,"create")
 f.AddFullSample(mp4.FullSample{Sample:mp4.Sample{Dur:10,Size:1,Flags:mp4.SyncSampleFlags},DecodeTime:base,Data:[]byte{payload}})
 if !explicit {
  t:=f.Moof.Traf; c:=t.Children[:0]
  for _,b:=range t.Children {if b.Type()!="tfdt" {c=append(c,b)}}
  t.Children=c; t.Tfdt=nil
 }
 var b bytes.Buffer; e=f.Encode(&b); must(e==nil,"encode"); return b.Bytes()
}
func main(){
 a:=fragment(1,100,'A',true); b:=fragment(2,110,'B',false)
 d:=mp4.FragmentDecoder{}; trex:=mp4.CreateTrex(1)
 var got []uint64
 for i,data:=range [][]byte{a,b} {
  pos:=uint64(0); if i==1 {pos=uint64(len(a))}
  f,e:=d.DecodeSR(bits.NewFixedSliceReader(data),pos); must(e==nil,"decode")
  ss,e:=f.GetFullSamples(trex);must(e==nil&&len(ss)==1,"samples")
  must(ss[0].Data[0]==byte('A'+i),"payload")
  got=append(got,ss[0].DecodeTime)
 }
 must(got[0]==100&&got[1]==0,"default decoder time")
 fmt.Printf("DEFAULT_DECODER_DTS=%v payload=AB\n",got)
 var times []uint64
 sf,e:=mp4.InitDecodeStream(bytes.NewReader(append(a,b...)),mp4.WithMaxFragments(1),mp4.WithFragmentCallback(func(f *mp4.Fragment,sa mp4.SampleAccessor)error{
  s,e:=sa.GetSample(1,1);if e!=nil{return e};times=append(times,s.DecodeTime);return nil
 }))
 must(e==nil,"init stream");e=sf.ProcessFragments();must(e==nil,"process stream")
 must(len(times)==2&&times[0]==100&&times[1]==0,"default stream time")
 fmt.Printf("DEFAULT_STREAM_DTS=%v fragments=2\n",times)
 fmt.Println("BASELINE_OK")
}
