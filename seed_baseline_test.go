package fauxgl
import("testing";"fmt";"math")
func TestSeedBaseline(t *testing.T){
 dc:=NewContext(32,24);dc.Cull=CullNone;dc.AlphaBlend=false
 dc.Shader=NewSolidColorShader(Identity(),Color{1,0,0,1})
 info:=dc.DrawLine(NewLineForPoints(Vector{-.7,-.6,0},Vector{.65,.55,0}))
 n:=0;for _,z:=range dc.DepthBuffer{if z<math.MaxFloat64{n++}}
 if n==0||info.UpdatedPixels==0{t.Fatal("line did not reach rasterizer")}
 fmt.Printf("SEED_LINE pixels=%d updated=%d\n",n,info.UpdatedPixels)
}
