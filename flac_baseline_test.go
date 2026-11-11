package flac_test
import (
 "fmt"
 "os"
 "testing"
 "github.com/mewkiz/flac"
 "github.com/mewkiz/flac/frame"
 "github.com/mewkiz/flac/meta"
)
func TestFLACBaseline(t *testing.T) {
 f,err:=os.CreateTemp(t.TempDir(),"pcm-*.flac");if err!=nil{t.Fatal(err)}
 info:=&meta.StreamInfo{BlockSizeMin:256,BlockSizeMax:256,SampleRate:48000,NChannels:2,BitsPerSample:16}
 enc,err:=flac.NewEncoder(f,info);if err!=nil{t.Fatal(err)}
 pcm:=make([]int32,256);for i:=range pcm{pcm[i]=int32(i*17-2000)}
 input:=&frame.Frame{Header:frame.Header{HasFixedBlockSize:true,BlockSize:256,SampleRate:48000,Channels:frame.ChannelsLR,BitsPerSample:16}}
 for c:=0;c<2;c++{input.Subframes=append(input.Subframes,&frame.Subframe{SubHeader:frame.SubHeader{Pred:frame.PredVerbatim},NSamples:256,Samples:append([]int32(nil),pcm...)})}
 if err:=enc.WriteFrame(input);err!=nil{t.Fatal(err)}
 if err:=enc.Close();err!=nil{t.Fatal(err)}
 stream,err:=flac.ParseFile(f.Name());if err!=nil{t.Fatal(err)};defer stream.Close()
 got,err:=stream.ParseNext();if err!=nil{t.Fatal(err)}
 for _,sf:=range got.Subframes{for i,s:=range sf.Samples{if s!=pcm[i]{t.Fatalf("sample %d got %d",i,s)}}}
 if got.Channels!=frame.ChannelsLR||got.Subframes[0].Pred!=frame.PredVerbatim{t.Fatal("unexpected analysis")}
 raw,err:=os.ReadFile(f.Name());if err!=nil{t.Fatal(err)}
 fmt.Printf("FLAC_BASELINE_OK channels=2 samples=256 pred=verbatim bytes=%d\n",len(raw))
}
