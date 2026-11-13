package main
import("fmt";"errors";g "github.com/Knetic/govaluate")
type params struct{gets int; flag bool}
var sentinel=errors.New("unexpected rhs")
func(p *params)Get(n string)(interface{},error){p.gets++;if n=="flag"{return p.flag,nil};return nil,sentinel}
func main(){
 e,err:=g.NewEvaluableExpression("flag && missing > 0");if err!=nil{panic(err)}
 p:=&params{};v,err:=e.Eval(p);if err!=nil||v!=false||p.gets!=1{panic("short circuit")}
 p=&params{flag:true};_,err=e.Eval(p);if err!=sentinel||p.gets!=2{panic("error propagation")}
 e,err=g.NewEvaluableExpression("x * 2 + 1");if err!=nil{panic(err)};v,err=e.Evaluate(map[string]interface{}{"x":int32(3)});if err!=nil||v!=float64(7){panic("numeric")}
 fmt.Println("GOVALUATE_BASELINE shortcircuit=1 error_identity=ok int32=7")
}
