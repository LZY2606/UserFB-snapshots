package pt
import("testing";"math/rand";"math")
func TestSeedRGBTransport(t *testing.T){
 scene:=&Scene{Color:Color{.2,.4,.8}};scene.Add(NewSphere(V(0,0,-3),1,DiffuseMaterial(Color{.5,.5,.5})));scene.Compile()
 r:=Ray{V(0,0,0),V(0,0,-1)};h:=scene.Intersect(r);if !h.Ok() || math.Abs(h.T-2)>1e-9 {t.Fatal("sphere hit",h)}
 s:=NewSampler(1,1);s.DirectLighting=false;c:=s.Sample(scene,r,rand.New(rand.NewSource(41)))
 if math.Abs(c.R-.1)>1e-12||math.Abs(c.G-.2)>1e-12||math.Abs(c.B-.4)>1e-12 {t.Fatal(c)}
 t.Logf("OLD_RGB_TRANSPORT %.9g %.9g %.9g",c.R,c.G,c.B)
}
