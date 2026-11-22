package main
import ("fmt"; "strings"; "log"; m "github.com/grafov/m3u8")
func main(){
 raw := "#EXTM3U\n#EXT-X-VERSION:6\n#EXT-X-TARGETDURATION:2\n#EXT-X-MEDIA-SEQUENCE:41\n#EXT-X-MAP:URI=\"init-a.mp4\"\n#EXT-X-BYTERANGE:4@10\n#EXTINF:2,first\nsame.mp4\n#EXT-X-PROGRAM-DATE-TIME:2026-01-01T00:00:02Z\n#EXT-X-MAP:URI=\"init-b.mp4\"\n#EXT-X-BYTERANGE:6\n#EXTINF:2,second\nsame.mp4\n#EXT-X-ENDLIST\n"
 p,e:=m.NewMediaPlaylist(0,4);if e!=nil{log.Fatal(e)};if e=p.DecodeFrom(strings.NewReader(raw),true);e!=nil{log.Fatal(e)}
 if p.Count()!=2 || p.Segments[1].Map.URI!="init-b.mp4" || p.Segments[1].ProgramDateTime.IsZero(){log.Fatal("production parse mismatch")}
 encoded:=p.Encode().String();q,e:=m.NewMediaPlaylist(0,4);if e!=nil{log.Fatal(e)};if e=q.DecodeFrom(strings.NewReader(encoded),true);e!=nil{log.Fatal(e)}
 fmt.Printf("decode count=%d seq=%d pdt=%s\n",p.Count(),p.SeqNo,p.Segments[1].ProgramDateTime.UTC().Format("15:04:05"))
 fmt.Printf("implicit_offset=%d encoded_second_map=%t reread_count=%d\n",p.Segments[1].Offset,strings.Contains(encoded,"init-b.mp4"),q.Count())
 r,e:=m.NewMediaPlaylist(0,3);if e!=nil{log.Fatal(e)};for i:=0;i<3;i++{if e=r.Append(fmt.Sprintf("s%d",i),1,"");e!=nil{log.Fatal(e)}}
 for i:=3;i<5;i++{if e=r.Remove();e!=nil{log.Fatal(e)};if e=r.Append(fmt.Sprintf("s%d",i),1,"");e!=nil{log.Fatal(e)}}
 a:=r.GetAllSegments();if len(a)!=3{log.Fatal("window count")};fmt.Printf("wrapped_api_order=%s,%s,%s\n",a[0].URI,a[1].URI,a[2].URI)
 fmt.Println("production witness: ok")
}
