package demangle
import "testing"
func TestBaseline(t *testing.T){
 cases:=map[string]string{"_RC3foo":"foo","_RINvC3foo3barlE":"foo::bar::<i32>"}
 for in,want:=range cases{got,e:=ToString(in);if e!=nil||got!=want{t.Fatalf("%s: %q %v",in,got,e)}}
 got,e:=ToString("_RINvC3foo3barlE",NoTemplateParams);if e!=nil||got!="foo::bar::<>"{t.Fatalf("%q %v",got,e)}
 if _,e=ToString("_RC9foo");e==nil{t.Fatal("truncation accepted")}
 t.Log("BASELINE_RUST=foo::bar::<i32> NO_TEMPLATE=foo::bar::<> TRUNCATION=rejected")
}
