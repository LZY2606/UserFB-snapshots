package meltysynth
import("testing";"math";"fmt")
func seedSynth(t *testing.T, class int16, layers int) *Synthesizer {
 wave:=make([]int16,512);for i:=range wave {wave[i]=int16(12000*math.Sin(2*math.Pi*float64(i)/32))}
 h:=&SampleHeader{Start:0,End:400,StartLoop:32,EndLoop:352,SampleRate:48000,OriginalPitch:60}
 inst:=&Instrument{Name:"generated"}; samples:=[]*SampleHeader{h}
 for i:=0;i<layers;i++ { z:=&zone{generators:[]generator{{gen_ExclusiveClass,uint16(class)},{gen_SampleModes,1},{gen_SampleID,0}}};r,e:=createInstrumentRegion(inst,createEmptyZone(),z,samples);if e!=nil {t.Fatal(e)};inst.Regions=append(inst.Regions,r) }
 pre:=&Preset{Name:"seed",PatchNumber:0}; pr,e:=createPresetRegion(pre,createEmptyZone(),&zone{generators:[]generator{{gen_Instrument,0}}},[]*Instrument{inst});if e!=nil {t.Fatal(e)};pre.Regions=[]*PresetRegion{pr}
 sf:=&SoundFont{WaveData:wave,SampleHeaders:samples,Presets:[]*Preset{pre},Instruments:[]*Instrument{inst}}
 set:=NewSynthesizerSettings(48000);set.EnableReverbAndChorus=false;s,e:=NewSynthesizer(sf,set);if e!=nil {t.Fatal(e)};return s
}
func TestSeedAudio(t *testing.T){
 s:=seedSynth(t,0,2);s.NoteOn(0,60,100);if s.voices.activeVoiceCount!=2{t.Fatal("normal layering")}
 l,r:=make([]float32,1024),make([]float32,1024);s.Render(l,r);var energy float64;for i:=range l {energy+=float64(l[i]*l[i]+r[i]*r[i])};if energy<=1e-6 {t.Fatal("silent seed")}
 fmt.Println("MELTY_BASELINE_OK layers=2 pcm_frames=1024")
}
