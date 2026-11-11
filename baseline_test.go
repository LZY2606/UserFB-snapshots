package fauxgl
import (
 "testing"
 "math"
)
func TestBaseline(t *testing.T) {
 dc:=NewContext(17,13); dc.Cull=CullNone; dc.AlphaBlend=false
 tri:=NewTriangleForPoints(Vector{-.8,-.7,0},Vector{.7,-.6,0},Vector{.1,.8,0})
 dc.Shader=NewSolidColorShader(Identity(),Color{1,0,0,1})
 first:=dc.DrawTriangle(tri)
 if first.UpdatedPixels==0 { t.Fatal("triangle not rasterized") }
 touched:=0
 for _,z:=range dc.DepthBuffer { if z!=math.MaxFloat64 { touched++ } }
 if uint64(touched)!=first.UpdatedPixels {t.Fatalf("coverage %d vs updates %d",touched,first.UpdatedPixels)}
 dc.Shader=NewSolidColorShader(Identity(),Discard)
 second:=dc.DrawTriangle(tri)
 if second.UpdatedPixels!=0 {t.Fatal("discard updated depth")}
 t.Logf("BASELINE opaque_pixels=%d discarded_updates=%d center_z=%.3f",touched,second.UpdatedPixels,dc.DepthBuffer[6*17+8])
}
func TestHorizontalRepro(t *testing.T) {
 dc:=NewContext(17,13); dc.Cull=CullNone; dc.AlphaBlend=false
 tri:=NewTriangleForPoints(Vector{-.8,-.8,0},Vector{.8,-.8,0},Vector{0,.8,0})
 if dc.DrawTriangle(tri).UpdatedPixels==0 {t.Fatal("missing horizontal triangle")}
}
