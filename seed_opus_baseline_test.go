package mp4

import (
 "bytes"
 "fmt"
 "io"
 "os"
 "path/filepath"
 "testing"
)

func TestSeedOpusTables(t *testing.T) {
 f, err := os.Create(filepath.Join(t.TempDir(),"opus.mp4"))
 if err != nil {t.Fatal(err)}
 defer f.Close()
 w := NewWriter(f)
 begin := func(b IImmutableBox) *BoxInfo {
  bi, err := w.StartBox(&BoxInfo{Type:b.GetType()}); if err != nil {t.Fatal(err)}
  if _,err = Marshal(w,b,Context{});err != nil {t.Fatal(err)}
  return bi
 }
 end := func(){if _,err:=w.EndBox();err!=nil {t.Fatal(err)}}
 emit := func(b IImmutableBox){begin(b);end()}
 emit(&Ftyp{MajorBrand:BrandISOM(),CompatibleBrands:[]CompatibleBrandElem{{CompatibleBrand:BrandISOM()}}})
 bi,err:=w.StartBox(&BoxInfo{Type:BoxTypeMdat()});if err!=nil{t.Fatal(err)}
 packet:=[]byte{0xf8,0xff,0xfe}
 for i:=0;i<3;i++ {if _,err=w.Write(packet);err!=nil{t.Fatal(err)}}
 end()
 begin(&Moov{})
 emit(&Mvhd{Timescale:1000,DurationV0:40,NextTrackID:8})
 begin(&Trak{})
 emit(&Tkhd{TrackID:7,DurationV0:40})
 begin(&Edts{})
 emit(&Elst{EntryCount:1,Entries:[]ElstEntry{{SegmentDurationV0:40,MediaTimeV0:480,MediaRateInteger:1}}})
 end()
 begin(&Mdia{})
 emit(&Mdhd{Timescale:48000,DurationV0:2400,Language:[3]byte{'u','n','d'}})
 begin(&Minf{})
 begin(&Stbl{})
 begin(&Stsd{EntryCount:1})
 ase:=&AudioSampleEntry{SampleEntry:SampleEntry{DataReferenceIndex:1},ChannelCount:1,SampleSize:16,SampleRate:48000<<16}
 ase.SetType(BoxTypeOpus());begin(ase)
 emit(&DOps{OutputChannelCount:1,PreSkip:3840,InputSampleRate:44100,OutputGain:-256})
 end();end()
 emit(&Stts{EntryCount:2,Entries:[]SttsEntry{{SampleCount:2,SampleDelta:960},{SampleCount:1,SampleDelta:480}}})
 emit(&Stsc{EntryCount:1,Entries:[]StscEntry{{FirstChunk:1,SamplesPerChunk:3,SampleDescriptionIndex:1}}})
 emit(&Stsz{SampleSize:3,SampleCount:3})
 emit(&Stco{EntryCount:1,ChunkOffset:[]uint32{uint32(bi.Offset+bi.HeaderSize)}})
 end();end();end();end();end()
 info,err:=Probe(f);if err!=nil{t.Fatal(err)}
 if len(info.Tracks)!=1 {t.Fatalf("tracks=%d",len(info.Tracks))}
 track:=info.Tracks[0]
 if track.TrackID!=7||track.Timescale!=48000||track.Duration!=2400||len(track.Samples)!=3||len(track.Chunks)!=1 {t.Fatalf("unexpected track: %+v",track)}
 if track.Codec!=CodecUnknown {t.Fatalf("old Probe codec changed: %v",track.Codec)}
 if len(track.EditList)!=1||track.EditList[0].MediaTime!=480||track.EditList[0].SegmentDuration!=40 {t.Fatal("edit table changed")}
 var dops *DOps
 _,err=ReadBoxStructure(f,func(h *ReadHandle)(interface{},error){
  switch h.BoxInfo.Type {
  case BoxTypeMoov(),BoxTypeTrak(),BoxTypeMdia(),BoxTypeMinf(),BoxTypeStbl(),BoxTypeStsd(),BoxTypeOpus(),BoxTypeEdts():return h.Expand()
  case BoxTypeDOps():b,_,err:=h.ReadPayload();if err!=nil{return nil,err};dops=b.(*DOps)
  }
  return nil,nil
 });if err!=nil {t.Fatal(err)}
 if dops==nil||dops.PreSkip!=3840||dops.OutputGain != -256||dops.InputSampleRate!=44100 {t.Fatalf("unexpected dOps: %+v",dops)}
 for i,s:=range track.Samples {
  expected:=uint32(960);if i==2{expected=480}
  if s.Size!=3||s.TimeDelta!=expected {t.Fatalf("sample %d: %+v",i,s)}
  if _,err=f.Seek(int64(track.Chunks[0].DataOffset)+int64(i*3),io.SeekStart);err!=nil{t.Fatal(err)}
  got:=make([]byte,3);if _,err=io.ReadFull(f,got);err!=nil{t.Fatal(err)}
  if !bytes.Equal(got,packet){t.Fatal("production sample location mismatch")}
 }
 fmt.Println("MP4_OPUS_TABLES_OK track=7 packets=3 payload_bytes=9 codec=unknown")
 fmt.Println("MP4_OPUS_WINDOW_PROBE dOps_pre_skip=3840 edit_start=480 edit_duration_48k=1920 edit_end=2400 toc_total=2880 stts_total=2400")
}
