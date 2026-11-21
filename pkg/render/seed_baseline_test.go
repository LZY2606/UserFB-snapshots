package render_test
import (
 "fmt"
 "math"
 "testing"
 "github.com/hunterloftis/pbr/pkg/geom"
 "github.com/hunterloftis/pbr/pkg/material"
 "github.com/hunterloftis/pbr/pkg/render"
 "github.com/hunterloftis/pbr/pkg/rgb"
 "github.com/hunterloftis/pbr/pkg/surface"
)
type seedEnv struct{}
func(seedEnv) At(geom.Dir) rgb.Energy{return rgb.Energy{X:2,Y:3,Z:4}}
func TestSeedProductionPath(t *testing.T){
 lamp:=&material.Uniform{Color:rgb.Energy{X:1,Y:2,Z:3},Emission:5}
 sphere:=surface.UnitSphere(lamp)
 scene:=render.NewScene(nil,surface.NewTree(sphere),seedEnv{})
 ray:=geom.NewRay(geom.Vec{Z:2},geom.Dir{Z:-1})
 hit:=render.SeedProductionTrace(scene,ray,7,3,false)
 if hit!=(rgb.Energy{X:5,Y:10,Z:15}) {t.Fatal(hit)}
 miss:=render.SeedProductionTrace(scene,geom.NewRay(geom.Vec{Z:2},geom.Dir{Y:1}),7,3,false)
 if miss!=(rgb.Energy{X:2,Y:3,Z:4}) {t.Fatal(miss)}
 sphere2:=surface.UnitSphere(&material.Uniform{Color:rgb.Energy{X:0.2,Y:0.3,Z:0.4},Metalness:0.37,Roughness:0.4,Specularity:0.05})
 scene2:=render.NewScene(nil,surface.NewTree(sphere2),seedEnv{})
 sum:=rgb.Black
 for i:=int64(0);i<64;i++ {e:=render.SeedProductionTrace(scene2,ray,i,3,false);if math.IsNaN(e.X+e.Y+e.Z)||math.IsInf(e.X+e.Y+e.Z,0){t.Fatal(e)};sum=sum.Plus(e)}
 if sum.Mean()<=0 {t.Fatal(sum)}
 fmt.Printf("PBR production trace: emitter=5,10,15 environment=2,3,4 material_sum=%.8f,%.8f,%.8f samples=64\n",sum.X,sum.Y,sum.Z)
}
