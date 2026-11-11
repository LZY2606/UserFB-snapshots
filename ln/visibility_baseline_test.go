package ln
import (
 "fmt"
 "testing"
)
func TestVisibilityBaseline(t *testing.T) {
 s:=&Scene{}
 s.Add(NewSphere(Vector{0,0,2.5},0.1));s.Compile()
 eye:=Vector{0,0,5}
 path:=Path{{-1,0,0},{1,0,0}}
 paths:=path.Filter(&ClipFilter{Identity(),eye,s})
 fmt.Printf("endpoint visibility %v %v; midpoint visibility %v; filtered paths=%d vertices=%d\n",s.Visible(eye,path[0]),s.Visible(eye,path[1]),s.Visible(eye,Vector{}),len(paths),len(paths[0]))
 if len(paths)!=1||len(paths[0])!=2||s.Visible(eye,Vector{}) {t.Fatal("baseline changed")}
}
