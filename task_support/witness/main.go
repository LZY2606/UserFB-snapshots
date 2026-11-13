package main
import("fmt";"strings"; m "github.com/grafov/m3u8")
func main(){
 input:=`#EXTM3U
#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID="a",NAME="main",URI="a.m3u8"
#EXT-X-MEDIA:TYPE=CLOSED-CAPTIONS,GROUP-ID="cc",NAME="captions",INSTREAM-ID=CC1
#EXT-X-STREAM-INF:BANDWIDTH=100,AUDIO="a",CLOSED-CAPTIONS="cc"
low.m3u8
#EXT-X-STREAM-INF:BANDWIDTH=200,AUDIO="a",CLOSED-CAPTIONS=NONE
high.m3u8
#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID="spare",NAME="spare",URI="spare.m3u8"
`
 p:=m.NewMasterPlaylist();if e:=p.DecodeFrom(strings.NewReader(input),true);e!=nil{panic(e)}
 if len(p.Variants)!=2{panic("variants")}; out:=p.Encode().String()
 if !strings.Contains(out,"low.m3u8")||!strings.Contains(out,"GROUP-ID=\"a\""){panic("normal path")}
 p2:=m.NewMasterPlaylist();if e:=p2.DecodeFrom(strings.NewReader(out),true);e!=nil{panic(e)}
 audio:=0;for _,v:=range p.Variants{for _,a:=range v.Alternatives{if a.Type=="AUDIO"&&a.GroupId=="a"{audio++}}}
 fmt.Printf("variants=%d shared_audio_links=%d orphan_preserved=%v caption_id_preserved=%v\n",len(p.Variants),audio,strings.Contains(out,"spare.m3u8"),strings.Contains(out,"INSTREAM-ID=CC1"))
 fmt.Println("HLS_BASELINE_OK")
}
