package gaad
import("fmt";"testing")
func testAAC() []byte {
 var body []byte;nb:=0
 put:=func(v uint64,n int){for i:=n-1;i>=0;i--{if nb%8==0{body=append(body,0)};body[nb/8]|=byte((v>>i)&1)<<uint(7-nb%8);nb++}}
 align:=func(){for nb%8!=0{put(0,1)}}
 put(0,3);put(0,4);put(0,8);put(0,1);put(0,2);put(0,1);put(0,6);put(0,1);put(0,3)
 put(4,3);put(2,4);put(1,1);put(3,8);align();put(0x414243,24)
 put(7,3);align()
 l:=7+len(body);head:=[]byte{0xff,0xf1,0x50,0x40|byte(l>>11),byte(l>>3),byte(l&7)<<5|0x1f,0xfc}
 return append(head,body...)
}
func TestBaseline(t *testing.T){
 a,err:=ParseADTS(testAAC());if err!=nil{t.Fatal(err)}
 if a.SamplingFrequency!=44100||len(a.Single_channel_elements)!=1||len(a.Data_stream_elements)!=1{t.Fatalf("aac=%+v",a)}
 e:=a.Data_stream_elements[0];if e.Element_instance_tag!=2||string(e.Data_stream_byte[2])!="ABC"{t.Fatalf("dse=%+v",e)}
 fmt.Println("BASELINE_OK unaligned-sce-aligned-dse")
}
