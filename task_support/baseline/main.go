package main
import (
 "fmt"
 "math"
 "math/rand"
 r "github.com/unixpickle/model3d/render3d"
 m "github.com/unixpickle/model3d/model3d"
)
func main() {
 rand.Seed(7)
 light:=r.NewSphereAreaLight(&m.Sphere{Center:m.XYZ(0,-3,3),Radius:0.75},r.NewColor(15))
 scene:=r.JoinedObject{light,&r.ColliderObject{Collider:&m.Sphere{Center:m.Coord3D{},Radius:1},Material:&r.LambertMaterial{DiffuseColor:r.NewColor(.6)}}}
 tracer:=&r.BidirPathTracer{Camera:r.NewCameraAt(m.XYZ(0,-5,0),m.Coord3D{},math.Pi/8),Light:light,MaxDepth:3,MaxLightDepth:3,NumSamples:256}
 img:=r.NewImage(3,3);tracer.Render(img,scene)
 total:=0.0;for _,c:=range img.Data {v:=c.Sum();if math.IsNaN(v)||math.IsInf(v,0)||v<0 {panic("invalid radiance")};total+=v}
 if total<=0 {panic("black baseline")}
 fmt.Printf("BPT_BASELINE pixels=%d finite_nonblack=true energy=%.9g\n",len(img.Data),total)
 tri:=&m.Triangle{m.XYZ(-1,0,0),m.XYZ(1,0,0),m.XYZ(0,0,1)}
 mesh:=m.NewMesh();mesh.Add(tri);ml:=r.NewMeshAreaLight(mesh,r.NewColor(2));rng:=rand.New(rand.NewSource(19))
 sum:=0.;for i:=0;i<1000;i++ {p,_,e:=ml.SampleLight(rng);if e.Sum()!=6 {panic("emission changed")};sum+=p.Z}
 if math.Abs(sum/1000-1.0/3)>.05 {panic("bad native sampling")}
 fmt.Printf("MESH_BASELINE samples=1000 mean_z=%.9g power=%.9g\n",sum/1000,ml.TotalEmission())
}
