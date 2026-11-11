package pt
import (
 "fmt"
 "math"
 "math/rand"
 "testing"
)
func TestSphereLightBaseline(t *testing.T) {
 s:=&Scene{}
 light:=NewSphere(Vector{0,0,5},1,LightMaterial(White,1))
 s.Add(light);s.Compile()
 sampler:=NewDirectSampler();sampler.SoftShadows=false
 c:=sampler.sampleLight(s,Ray{Vector{},Vector{0,0,1}},rand.New(rand.NewSource(7)),light)
 fmt.Printf("centered sphere direct RGB %.12f %.12f %.12f; Lambertian reference 0.040000000000\n",c.R,c.G,c.B)
 if math.Abs(c.R-1.0/24)>1e-12 {t.Fatal("baseline changed",c)}
}
